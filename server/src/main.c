/**
 * @file main.c
 * @brief FTP 服务器 – Reactor (epoll) + 线程池 架构
 *
 * 单进程设计，包含以下三个主要线程角色：
 *   主线程:   epoll_wait 事件循环 → 接受新连接 + 命令分发到线程池
 *   工作线程池: thread_pool.c 中的工作线程 → 执行文件 I/O 和协议响应
 *   TUI 线程:   每秒刷新一次共享内存中的客户端状态监控表
 *
 * 这种架构相比旧的 fork-per-client（每个客户端 fork 一个子进程）
 * 模型的优势：
 *   - 避免频繁 fork 带来的高开销
 *   - 线程池复用线程，资源利用率更高
 *   - epoll 边缘触发模式减少系统调用次数
 *
 * 编译: cd server && mkdir build && cd build && cmake .. && make
 * 运行: ./bin/ftp_server 0.0.0.0 8888
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>

#include "protocol.h"
#include "handler.h"
#include "ipc_shm.h"
#include "thread_pool.h"

/* ------------------------------------------------------------------ */
/*  常量定义                                                            */
/* ------------------------------------------------------------------ */
#define MAX_EVENTS  1024
#define LISTEN_BACKLOG 128

/* ------------------------------------------------------------------ */
/*  全局变量（跨线程共享）                                               */
/* ------------------------------------------------------------------ */
static client_info_t  *g_shm      = NULL;
static thread_pool_t   g_pool;
static int             g_listen_fd = -1;
static int             g_epfd      = -1;
static volatile bool   g_running   = true;

/* ------------------------------------------------------------------ */
/*  会话表（将文件描述符 fd 映射到 client_session_t* 指针）               */
/*  使用简单的数组实现，因为 MAX_EVENTS 是已知的编译时常量。              */
/*  数组索引即 fd，O(1) 时间即可查找任意连接对应的会话。                  */
/* ------------------------------------------------------------------ */
static client_session_t *g_sessions[MAX_EVENTS];   /* 以 fd 为索引 */
static int               g_next_client_id = 1;      /* 自增客户端 ID */

/* ------------------------------------------------------------------ */
/*  工具函数                                                            */
/* ------------------------------------------------------------------ */
/* 将指定 fd 设置为非阻塞模式。
 * 非阻塞是 Reactor 模式的基础：所有套接字必须是非阻塞的，
 * 这样 epoll_wait 返回后再 read/write 时才不会阻塞整个事件循环。 */
static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* ------------------------------------------------------------------ */
/*  服务器 Socket 初始化（经典的 socket → bind → listen 流程）           */
/*  设置 SO_REUSEADDR 和 SO_REUSEPORT 以便服务器重启时能立即绑定端口。   */
/* ------------------------------------------------------------------ */
static int server_init(const char *ip, const char *port)
{
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return -1; }

    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)atoi(port));
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (bind(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(sockfd); return -1;
    }
    if (listen(sockfd, LISTEN_BACKLOG) < 0) {
        perror("listen"); close(sockfd); return -1;
    }
    return sockfd;
}

/* ------------------------------------------------------------------ */
/*  处理新的客户端连接                                                  */
/*  1. accept() 接受 TCP 连接                                           */
/*  2. 设置非阻塞模式                                                   */
/*  3. 分配并初始化 client_session_t 结构体                             */
/*  4. 将会话注册到全局会话表 g_sessions                                */
/*  5. 将新 fd 以边缘触发 (EPOLLET) 模式添加到 epoll 监听               */
/* ------------------------------------------------------------------ */
static client_session_t *accept_client(void)
{
    struct sockaddr_in cli;
    socklen_t addrlen = sizeof(cli);
    int cfd = accept(g_listen_fd, (struct sockaddr *)&cli, &addrlen);
    if (cfd < 0) return NULL;

    /* 所有客户端 fd 也必须是非阻塞的，与 Reactor 模式保持一致 */
    set_nonblock(cfd);

    /* 分配并初始化客户端会话结构体 */
    client_session_t *sess = calloc(1, sizeof(client_session_t));
    if (!sess) { close(cfd); return NULL; }

    sess->fd       = cfd;
    sess->addr     = cli;
    sess->shm      = g_shm;          /* 指向共享内存，用于状态上报 */
    sess->epoll_fd = g_epfd;         /* 保存 epoll fd，工作线程需要用它重新注册 */
    strncpy(sess->ip, inet_ntoa(cli.sin_addr), sizeof(sess->ip) - 1);
    sess->port     = ntohs(cli.sin_port);

    /* 将会话指针存入全局会话表，以 fd 为索引，方便 O(1) 查找 */
    if (cfd < MAX_EVENTS)
        g_sessions[cfd] = sess;

    /* 将新客户端 fd 加入 epoll 监听（边缘触发模式 EPOLLET）
     * 边缘触发意味着只在状态变化时通知一次，要求应用层必须循环读取直到 EAGAIN，
     * 这样可以减少 epoll_wait 返回次数，提高性能。 */
    struct epoll_event ev;
    ev.events   = EPOLLIN | EPOLLET;
    ev.data.ptr = sess;              /* 携带会话指针，事件到来时可直接获取 */
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, cfd, &ev);

    printf("[server] new connection fd=%d from %s:%d\n", cfd, sess->ip, sess->port);
    return sess;
}

/* ------------------------------------------------------------------ */
/*  命令分发：根据协议头中的 cmd_no 将任务投入线程池                       */
/*                                                                      */
/*  关键设计：在提交任务给工作线程之前，先将该客户端的 fd 从 epoll 中      */
/*  移除 (EPOLL_CTL_DEL)。这样做的原因：                                  */
/*    - 工作线程接管该 fd 的 I/O 操作（读/写文件数据）                    */
/*    - 避免主线程的 reactor 循环和工作线程同时对同一个 fd 进行读写，      */
/*      造成数据竞争（data race）                                        */
/*    - 工作线程完成任务后，会通过 epoll_ctl_add_session() 将 fd 重新      */
/*      注册回 epoll，让主线程恢复对该连接的事件监听                      */
/* ------------------------------------------------------------------ */
static void dispatch_command(client_session_t *sess,
                             unsigned char *payload, int plen)
{
    if (plen < 4) { free(payload); return; }  /* 包太短，不是有效命令 */

    /* 从 payload 前 4 字节解析出小端序的命令号 */
    int cmd_no = get_le32(payload);
    task_t task;
    memset(&task, 0, sizeof(task));
    task.session     = sess;
    task.payload     = payload;     /* 工作线程负责 free 释放内存 */
    task.payload_len = plen;

    switch (cmd_no) {
    case FTP_CMD_LOGIN:
        task.type = TASK_LOGIN;
        /* 从 epoll 中移除 fd —— 工作线程完成登录后会重新注册 */
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
        thread_pool_submit(&g_pool, &task);
        break;

    case FTP_CMD_LS:
        task.type = TASK_LS;
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
        thread_pool_submit(&g_pool, &task);
        break;

    case FTP_CMD_GET:
        task.type = TASK_GET;
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
        thread_pool_submit(&g_pool, &task);
        break;

    case FTP_CMD_PUT:
        task.type = TASK_PUT;
        /* 上传模式：工作线程会直接从 fd 读取文件数据。
         * 必须先从 epoll 移除 fd，否则主线程的 reactor 读取和
         * 工作线程的读取会产生数据竞争，导致文件内容错乱。 */
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
        thread_pool_submit(&g_pool, &task);
        break;

    case FTP_CMD_BYE:
        task.type = TASK_BYE;
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
        thread_pool_submit(&g_pool, &task);
        break;

    case FTP_CMD_LISTDIR:
        task.type = TASK_LISTDIR;
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
        thread_pool_submit(&g_pool, &task);
        break;

    default:
        /* 未知命令，释放 payload 并忽略 */
        free(payload);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  从客户端 fd 读取命令数据（非阻塞 + 边缘触发模式）                    */
/*                                                                      */
/*  由于使用了 EPOLLET（边缘触发），我们必须循环读取直到返回 EAGAIN，      */
/*  否则可能丢失数据——epoll 只会在状态从"无数据"变为"有数据"时通知一次。 */
/*                                                                      */
/*  注意：这里只读取一个完整的命令包就停止循环。原因是 dispatch_command    */
/*  会将该 fd 从 epoll 中移除，工作线程接管后续 I/O。所以继续读取是       */
/*  没有意义的（工作线程完成后会重新将 fd 加入 epoll）。                  */
/* ------------------------------------------------------------------ */
static void handle_client_read(client_session_t *sess)
{
    /* 循环读取所有可用的完整数据包 */
    while (1) {
        int plen;
        unsigned char *payload = read_packet(sess->fd, &plen);
        if (!payload) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                /* 非阻塞 socket 的正常情况：当前没有更多数据可读，
                 * 退出循环等待下一次 epoll 通知。 */
                break;
            }
            /* 真正的错误或客户端断开连接，清理会话资源 */
            printf("[server] fd=%d disconnected (read error)\n", sess->fd);
            epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
            close(sess->fd);
            if (sess->fd < MAX_EVENTS) g_sessions[sess->fd] = NULL;
            free(sess);
            return;
        }
        /* 成功读取到一个完整命令包，交给 dispatch_command 分发处理。
         * 分发后该 fd 已从 epoll 中移除，工作线程会在完成后重新注册。
         * 因此这里不再继续循环读取，直接返回。 */
        dispatch_command(sess, payload, plen);
        break;
    }
}

/* ================================================================== */
/*  TUI 监控线程（独立于主 Reactor 循环运行）                             */
/*                                                                      */
/*  该线程以 1 秒为间隔刷新终端界面，显示：                               */
/*    - 当前在线客户端数量和连接信息                                     */
/*    - 每个客户端的状态（空闲/下载中/上传中），用不同颜色区分            */
/*    - 服务器运行时间                                                   */
/*                                                                      */
/*  数据来源是共享内存 g_shm（client_info_t 数组），各工作线程会          */
/*  实时更新自己负责的客户端状态到这个共享内存区域。                      */
/* ================================================================== */
/* 刷新终端界面，绘制服务器监控面板。
 * 使用 ANSI 转义码实现彩色输出：
 *   \033[0;32m 绿色 —— 空闲客户端
 *   \033[0;33m 黄色 —— 正在下载的客户端
 *   \033[0;31m 红色 —— 正在上传的客户端
 */
static void tui_refresh(client_info_t *shm)
{
    system("clear");  /* 清屏，实现每秒刷新的动画效果 */

    /* 打印标题栏（青色） */
    printf("\033[1;36m");
    printf("============================================================\n");
    printf("  [ Embedded Remote File Manager - Server Monitor ]\n");
    printf("  Architecture: Reactor (epoll) + Thread Pool (%d workers)\n",
           g_pool.num_threads);
    printf("  Shared Dir: %s\n", MY_FTP_BOOT);
    printf("============================================================\033[0m\n");

    /* 统计当前活跃客户端数量 */
    int active = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (shm[i].active) active++;

    printf(" [Online Clients: %d / %d]\n", active, MAX_CLIENTS);
    printf("------------------------------------------------------------\n");
    printf(" %-8s | %-15s | %-6s | %s\n", "CID", "Client IP", "Port", "Current State");
    printf("------------------------------------------------------------\n");

    if (active == 0) {
        printf(" (no connected clients)\n");
    } else {
        /* 遍历共享内存表，打印每个活跃客户端的信息 */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!shm[i].active) continue;
            /* 根据客户端状态选择不同的显示颜色 */
            const char *color = "\033[0;32m";  /* 默认绿色：空闲 */
            if (strstr(shm[i].status, "Download")) color = "\033[0;33m"; /* 黄色：下载中 */
            else if (strstr(shm[i].status, "Upload")) color = "\033[0;31m"; /* 红色：上传中 */
            printf(" %-8d | %-15s | %-6d | %s%s\033[0m\n",
                   shm[i].pid, shm[i].ip, shm[i].port, color, shm[i].status);
        }
    }
    printf("------------------------------------------------------------\n");

    /* 打印当前时间和操作提示 */
    time_t now = time(NULL);
    struct tm *lt = localtime(&now);
    printf(" [%02d:%02d:%02d] Server running. Press Ctrl+C to stop.\n",
           lt->tm_hour, lt->tm_min, lt->tm_sec);
    printf("============================================================\n");
}

/* TUI 线程入口函数：每 1 秒刷新一次监控面板。
 * 线程被设置为 detached 状态，无需主线程 join 回收。
 * 通过全局变量 g_running 控制退出时机。 */
static void *tui_thread_func(void *arg)
{
    client_info_t *shm = (client_info_t *)arg;
    while (g_running) {
        tui_refresh(shm);   /* 刷新终端监控面板 */
        sleep(1);           /* 每秒刷新一次 */
    }
    return NULL;
}

/* ================================================================== */
/*  信号处理函数                                                        */
/*                                                                      */
/*  处理 SIGINT (Ctrl+C) 和 SIGTERM 信号，触发服务器优雅关闭。           */
/*  通过关闭监听 fd 来唤醒阻塞在 epoll_wait 中的主循环，                  */
/*  使其能够检测到 g_running == false 并退出。                           */
/* ================================================================== */
static void sigint_handler(int sig)
{
    (void)sig;               /* 显式标记参数未使用，避免编译器警告 */
    g_running = false;       /* 通知所有线程停止运行 */
    /* 关闭监听 fd 以唤醒 epoll_wait。
     * 当 g_listen_fd 被关闭后，epoll_wait 会返回事件或错误，
     * 主循环得以继续执行并检查 g_running 标志，从而正常退出。 */
    if (g_listen_fd >= 0) close(g_listen_fd);
}

/* ================================================================== */
/*  资源清理函数                                                        */
/*                                                                      */
/*  在服务器退出前调用，确保所有资源被正确释放：                          */
/*    1. 销毁线程池（等待所有工作线程完成并释放资源）                     */
/*    2. 关闭所有活跃的客户端连接并释放会话内存                           */
/*    3. 关闭 epoll 文件描述符                                           */
/*  注意：共享内存 g_shm 的清理在 main 末尾通过 shm_destroy 完成         */
/* ================================================================== */
static void cleanup(void)
{
    /* 销毁线程池：等待所有工作线程完成任务，释放线程池资源 */
    thread_pool_destroy(&g_pool);

    /* 遍历会话表，关闭所有活跃客户端连接并释放内存 */
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (g_sessions[i]) {
            close(g_sessions[i]->fd);      /* 关闭客户端 socket */
            free(g_sessions[i]);           /* 释放会话结构体内存 */
            g_sessions[i] = NULL;          /* 清除悬空指针 */
        }
    }
    /* 关闭 epoll 实例 */
    if (g_epfd >= 0) close(g_epfd);
}

/* ================================================================== */
/*  main 函数 —— 服务器入口点                                          */
/*                                                                      */
/*  启动流程分为 7 个步骤：                                              */
/*    1. 初始化共享内存（用于 TUI 监控和工作线程状态上报）                */
/*    2. 初始化监听 socket（socket → bind → listen）                    */
/*    3. 创建 epoll 实例并注册监听 fd                                   */
/*    4. 初始化线程池（8 个工作线程，处理文件 I/O 和协议逻辑）           */
/*    5. 注册信号处理器（SIGINT / SIGTERM）实现优雅退出                  */
/*    6. 启动 TUI 监控线程（独立线程，每秒刷新终端面板）                  */
/*    7. 进入主 Reactor 事件循环（epoll_wait → 分发事件）                */
/* ================================================================== */
int main(int argc, char *argv[])
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <ip> <port>\n", argv[0]);
        fprintf(stderr, "  e.g.: %s 0.0.0.0 8888\n", argv[0]);
        return 1;
    }

    /* --- 1. 初始化共享内存 ---
     * 共享内存用于：
     *   - TUI 线程读取各客户端状态并显示
     *   - 工作线程在处理过程中更新客户端状态（如"正在上传"、"下载完成"）
     */
    g_shm = shm_init();
    if (!g_shm) {
        fprintf(stderr, "FATAL: shared memory init failed\n");
        return 1;
    }

    /* --- 2. 初始化监听 socket ---
     * 创建 TCP socket，绑定到指定 IP 和端口，开始监听。
     * 设置 SO_REUSEADDR + SO_REUSEPORT 允许快速重启。
     */
    g_listen_fd = server_init(argv[1], argv[2]);
    if (g_listen_fd < 0) return 1;
    set_nonblock(g_listen_fd);  /* 监听 socket 也设为非阻塞 */

    /* --- 3. 创建 epoll 实例并注册监听 fd ---
     * epoll_create1(0) 等价于 epoll_create()，但参数更清晰。
     * 将监听 fd 以 EPOLLIN 事件注册，有新的客户端连接时 epoll_wait 会返回。
     */
    g_epfd = epoll_create1(0);
    if (g_epfd < 0) { perror("epoll_create1"); return 1; }

    struct epoll_event ev;
    ev.events   = EPOLLIN;       /* 监听可读事件（新连接到来） */
    ev.data.fd  = g_listen_fd;   /* 携带 fd 用于事件到来时的判断 */
    epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_listen_fd, &ev);

    /* --- 4. 初始化线程池 ---
     * 创建 8 个工作线程，它们阻塞在任务队列上等待任务。
     * 工作线程负责：登录验证、文件列表、文件上传/下载等涉及文件 I/O 的操作。
     */
    if (thread_pool_init(&g_pool, 8) < 0) {
        fprintf(stderr, "FATAL: thread pool init failed\n");
        return 1;
    }

    /* --- 5. 注册信号处理器 ---
     * SIGINT  = Ctrl+C 中断
     * SIGTERM = kill 命令发送的终止信号
     * 两者都触发相同的优雅关闭流程。
     */
    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* --- 6. 启动 TUI 监控线程 ---
     * 使用 pthread_detach 分离线程，退出时无需 pthread_join。
     * 该线程独立运行，每 1 秒刷新一次终端监控面板。
     */
    pthread_t tui_tid;
    pthread_create(&tui_tid, NULL, tui_thread_func, g_shm);
    pthread_detach(tui_tid);

    printf("[server] Reactor+ThreadPool listening on %s:%s (8 workers, epoll)\n",
           argv[1], argv[2]);

    /* ================================================================ */
    /*  7. 主 Reactor 事件循环                                           */
    /*                                                                   */
    /*  核心逻辑：                                                        */
    /*    epoll_wait 阻塞等待事件（超时 1000ms，以便定期检查 g_running）   */
    /*      → 如果是监听 fd 事件：accept 所有新连接并注册到 epoll          */
    /*      → 如果是客户端 fd 事件：                                      */
    /*          - EPOLLHUP/EPOLLERR：客户端断开，清理资源                  */
    /*          - EPOLLIN：读取命令数据，分发到线程池                      */
    /* ================================================================ */
    struct epoll_event events[MAX_EVENTS];

    while (g_running) {
        /* 超时设为 1000ms，而不是 -1（无限等待）。
         * 这样即使没有事件，也能每 1 秒检查一次 g_running 标志，
         * 确保收到 SIGINT 后能及时退出循环。 */
        int n = epoll_wait(g_epfd, events, MAX_EVENTS, 1000);
        if (n < 0) {
            if (errno == EINTR) continue;  /* 被信号中断，继续循环 */
            break;                          /* 真正的错误，退出 */
        }

        /* 遍历所有就绪的事件 */
        for (int i = 0; i < n; i++) {
            /* 判断是否为监听 fd 的事件（新客户端连接） */
            if (events[i].data.fd == g_listen_fd) {
                if (events[i].events & EPOLLIN) {
                    /* 循环 accept，直到没有更多待处理的连接。
                     * 虽然监听 fd 是水平触发（LT），这里仍需循环是因为
                     * 可能有多个连接同时到达，一次性全部接受效率更高。 */
                    while (accept_client() != NULL)
                        ;
                }
                continue;
            }

            /* 客户端 fd 事件 */
            client_session_t *sess = (client_session_t *)events[i].data.ptr;
            if (!sess) continue;  /* 防御性检查：会话指针不应为空 */

            /* 连接断开或错误事件 */
            if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                printf("[server] fd=%d hangup\n", sess->fd);
                epoll_ctl(g_epfd, EPOLL_CTL_DEL, sess->fd, NULL);
                if (sess->fd < MAX_EVENTS) g_sessions[sess->fd] = NULL;
                close(sess->fd);
                if (sess->logged_in)
                    shm_remove_client(g_shm, sess->fd); /* 从共享内存监控表中移除 */
                free(sess);
                continue;
            }

            /* 客户端有数据可读（新命令到达） */
            if (events[i].events & EPOLLIN) {
                handle_client_read(sess);
            }
        }
    }

    /* --- 清理并退出 --- */
    cleanup();
    pthread_join(tui_tid, NULL);  /* 等待 TUI 线程完全退出 */
    printf("[server] shutdown complete\n");
    return 0;
}

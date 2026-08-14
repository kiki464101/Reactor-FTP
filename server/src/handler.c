/**
 * @file handler.c
 * @brief Reactor+ThreadPool 服务器的工作线程命令处理器
 *
 * 每个命令处理器在独立的工作线程中运行，对 sess->fd 拥有独占访问权
 * （在任务提交到线程池之前，该 fd 已从主 epoll 集合中移除）。
 * 处理器发送响应后，通过 epoll_ctl 重新注册 fd，以便主 reactor 能
 * 继续读取下一条命令。
 *
 * 不使用 fork。不同会话的并发工作线程之间没有共享状态。
 * 文件 I/O（stat、open、read、write）在此处执行，即使阻塞也不会
 * 让主事件循环饥饿。
 */

#define _GNU_SOURCE
#include "handler.h"
#include "protocol.h"
#include "thread_pool.h"
#include "ipc_shm.h"
#include "sys_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/socket.h>

/* 递归创建目录，功能类似 mkdir -p
 * 例如 path="/a/b/c"，会依次创建 /a、/a/b、/a/b/c */
static void mkdir_p(const char *path)
{
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* 路径安全检查：禁止绝对路径（以 '/' 开头）和包含 ".." 的路径，防止目录穿越攻击 */
static bool is_path_safe(const char *path)
{
    if (!path) return false;
    if (path[0] == '/') return false;   /* 绝对路径 —— 拒绝 */
    if (strstr(path, "..")) return false;  /* 包含 ".." —— 可能试图穿越目录 */
    return true;
}

/* Write all n bytes to fd, handling partial writes and EINTR */
static int write_all(int fd, const void *buf, size_t n)
{
    size_t total = 0;
    const char *ptr = (const char *)buf;
    while (total < n) {
        ssize_t w = write(fd, ptr + total, n - total);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)w;
    }
    return (int)total;
}

/* ================================================================== */
/*  worker_func —— 线程池工作线程入口函数                                */
/*  从任务队列中阻塞式取出任务，根据任务类型分发给对应的处理函数          */
/* ================================================================== */
void *worker_func(void *arg)
{
    thread_pool_t *pool = (thread_pool_t *)arg;

    while (1) {
        /* 从任务队列中取出一个任务（阻塞等待，直到有任务入队） */
        pthread_mutex_lock(&pool->mutex);
        while (pool->count == 0 && !pool->shutdown)
            pthread_cond_wait(&pool->cond, &pool->mutex);
        if (pool->shutdown && pool->count == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }
        task_t task = pool->queue[pool->head];
        pool->head = (pool->head + 1) % MAX_QUEUE;
        pool->count--;
        pthread_mutex_unlock(&pool->mutex);

        client_session_t *sess = (client_session_t *)task.session;
        if (!sess) { free(task.payload); continue; }

        /* 根据任务类型分发到对应的处理函数 */
        switch (task.type) {
        case TASK_LOGIN:
            worker_handle_login(sess, task.payload, task.payload_len);
            break;
        case TASK_LS:
            worker_handle_ls(sess, task.payload, task.payload_len);
            break;
        case TASK_GET:
            worker_handle_get(sess, task.payload, task.payload_len);
            break;
        case TASK_PUT:
            worker_handle_put(sess, task.payload, task.payload_len);
            break;
        case TASK_BYE:
            worker_handle_bye(sess);
            break;
        case TASK_LISTDIR:
            worker_handle_listdir(sess, task.payload, task.payload_len);
            break;
        }

        free(task.payload);

        /* 重新将 fd 注册到 epoll，以便接收下一条命令
         * BYE 命令除外 —— 它的处理器已经关闭了 fd */
        if (task.type != TASK_BYE && sess->fd >= 0) {
            struct epoll_event ev;
            ev.events  = EPOLLIN | EPOLLET;
            ev.data.ptr = sess;
            /* 先尝试 MOD（fd 之前被移除但仍在 epoll 中），失败则用 ADD */
            if (epoll_ctl(sess->epoll_fd, EPOLL_CTL_MOD, sess->fd, &ev) < 0)
                epoll_ctl(sess->epoll_fd, EPOLL_CTL_ADD, sess->fd, &ev);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  辅助函数                                                             */
/* ------------------------------------------------------------------ */

/** 向客户端发送一个协议包，成功返回 0，失败返回 -1 */
static int tx(client_session_t *sess, int cmd, int result,
              const unsigned char *data, int dlen)
{
    return send_packet(sess->fd, cmd, result, data, dlen);
}

/** 便捷函数：发送不带数据的成功确认包 (result=1) */
static int ack_ok(client_session_t *sess, int cmd)
{
    return tx(sess, cmd, 1, NULL, 0);
}

/* ================================================================== */
/*  LOGIN —— 用户登录认证                                                */
/*  解析用户名和密码，调用验证函数，成功后生成 session_id 并记录到共享内存 */
/* ================================================================== */
void worker_handle_login(client_session_t *sess,
                         const unsigned char *payload, int plen)
{
    /* payload 格式: [4B cmd_no] [4B user_len] [user] [4B pass_len] [pass] */
    if (plen < 12) { tx(sess, FTP_CMD_LOGIN, 0, NULL, 0); return; }

    int user_len = get_le32(payload + 4);
    if (user_len < 1 || user_len > 63 || 8 + user_len + 4 > plen) {
        tx(sess, FTP_CMD_LOGIN, 0, NULL, 0);
        return;
    }

    int pass_len = get_le32(payload + 8 + user_len);
    if (pass_len < 1 || pass_len > 63 ||
        8 + user_len + 4 + pass_len > plen) {
        tx(sess, FTP_CMD_LOGIN, 0, NULL, 0);
        return;
    }

    char username[64] = {0}, password[64] = {0};
    memcpy(username, payload + 8, (size_t)(user_len < 63 ? user_len : 63));
    memcpy(password, payload + 8 + 4 + user_len,
           (size_t)(pass_len < 63 ? pass_len : 63));

    if (verify_user(username, password) == 0) {
        char session_id[32];
        snprintf(session_id, sizeof(session_id), "SID-%d", sess->fd);
        tx(sess, FTP_CMD_LOGIN, 1,
           (unsigned char *)session_id, (int)strlen(session_id));
        /* 登录成功后，将客户端信息写入共享内存
         * 使用 fd 作为客户端唯一标识（不再使用 fork/pid） */
        shm_add_client(sess->shm, sess->fd, sess->ip,
                       sess->port, "Online");
        sess->logged_in = true;
    } else {
        /* 认证失败，返回错误信息 */
        tx(sess, FTP_CMD_LOGIN, 0,
           (unsigned char *)"auth failed", 11);
    }
}

/* ================================================================== */
/*  LS —— 列出共享目录的内容（可选指定子目录）                              */
/*  遍历目录条目，目录名后加 '/' 后缀，结果用换行符分隔                     */
/* ================================================================== */
void worker_handle_ls(client_session_t *sess,
                      const unsigned char *payload, int plen)
{
    shm_update_status(sess->shm, sess->fd, "Refreshing...");

    char  buf[SIZE] = {0};
    int   off = 0;
    char  dir_to_list[512];

    if (plen >= 12) {
        /* payload 格式: [4B cmd_no] [4B path_len] [path…] */
        int path_len = get_le32(payload + 4);
        if (path_len < 0 || 8 + path_len > plen) {
            tx(sess, FTP_CMD_LS, 0, NULL, 0);
            shm_update_status(sess->shm, sess->fd, "Online");
            return;
        }
        char path[256] = {0};
        int copy_len = (path_len < 255) ? path_len : 255;
        memcpy(path, payload + 8, (size_t)copy_len);

        if (!is_path_safe(path)) {
            tx(sess, FTP_CMD_LS, 0, NULL, 0);
            shm_update_status(sess->shm, sess->fd, "Online");
            return;
        }

        snprintf(dir_to_list, sizeof(dir_to_list), "%s/%s", MY_FTP_BOOT, path);
    } else {
        snprintf(dir_to_list, sizeof(dir_to_list), "%s", MY_FTP_BOOT);
    }

    DIR *dir = opendir(dir_to_list);
    if (!dir) {
        fprintf(stderr, "[ls] opendir('%s') FAILED: errno=%d %s\n",
                dir_to_list, errno, strerror(errno));
        tx(sess, FTP_CMD_LS, 0, (unsigned char *)"opendir fail", 12);
        shm_update_status(sess->shm, sess->fd, "Online");
        return;
    }
    fprintf(stderr, "[ls] opendir('%s') OK\n", dir_to_list);

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;

        char fullpath[512];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", dir_to_list, d->d_name);

        struct stat st;
        if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                            "%s/\n", d->d_name);
        } else {
            off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                            "%s\n", d->d_name);
        }
    }
    closedir(dir);

    tx(sess, FTP_CMD_LS, 1, (unsigned char *)buf, off);
    shm_update_status(sess->shm, sess->fd, "Online");
}

/* ================================================================== */
/*  LISTDIR 递归辅助函数                                                   */
/*  从 base_path 出发，在 rel_prefix 下递归遍历所有文件和子目录              */
/*  只收集文件路径，跳过目录条目本身                                        */
/* ================================================================== */
static int listdir_recursive(const char *base_path, const char *rel_prefix,
                             char *buf, int *off, int bufsize)
{
    char full_base[512];
    snprintf(full_base, sizeof(full_base), "%s/%s", base_path, rel_prefix);
    /* 去掉搜索路径末尾可能存在的 '/'，避免路径格式问题 */
    size_t fb_len = strlen(full_base);
    if (fb_len > 0 && full_base[fb_len - 1] == '/')
        full_base[fb_len - 1] = '\0';

    DIR *dir = opendir(full_base[0] ? full_base : ".");
    if (!dir) {
        fprintf(stderr, "[listdir] opendir '%s' failed: %s\n",
                full_base[0] ? full_base : ".", strerror(errno));
        return -1;
    }
    fprintf(stderr, "[listdir] scanning: %s\n", full_base);

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;

        char item_path[512];
        snprintf(item_path, sizeof(item_path), "%s/%s", full_base, d->d_name);

        struct stat st;
        if (stat(item_path, &st) < 0) continue;

        if (S_ISDIR(st.st_mode)) {
            /* 目录：只递归进入子目录，不将目录本身添加到响应中
             * LISTDIR 命令只返回文件路径，不返回目录条目 */
            char new_prefix[512];
            snprintf(new_prefix, sizeof(new_prefix), "%s%s/",
                     rel_prefix, d->d_name);
            if (listdir_recursive(base_path, new_prefix, buf, off, bufsize) < 0) {
                closedir(dir);
                return -2;
            }
        } else {
            /* 文件条目格式：相对路径前缀 + 文件名 */
            int avail = bufsize - *off;
            if (avail <= 2) { closedir(dir); return -2; }
            int written = snprintf(buf + *off, (size_t)avail,
                                   "%s%s\n", rel_prefix, d->d_name);
            if (written < 0 || written >= avail) { closedir(dir); return -2; }
            fprintf(stderr, "[listdir]   file: %s%s\n", rel_prefix, d->d_name);
            *off += written;
        }
    }
    closedir(dir);
    return 0;
}

/* ================================================================== */
/*  LISTDIR —— 递归列出目录下的所有文件                                    */
/*  先验证目标目录存在，然后调用递归辅助函数收集所有文件路径                  */
/* ================================================================== */
void worker_handle_listdir(client_session_t *sess,
                           const unsigned char *payload, int plen)
{
    shm_update_status(sess->shm, sess->fd, "Refreshing...");

    /* payload 格式: [4B cmd_no] [4B name_len] [dirname…] */
    if (plen < 12) {
        tx(sess, FTP_CMD_LISTDIR, 0, NULL, 0);
        shm_update_status(sess->shm, sess->fd, "Online");
        return;
    }

    int name_len = get_le32(payload + 4);
    if (name_len < 0 || 8 + name_len > plen) {
        tx(sess, FTP_CMD_LISTDIR, 0, NULL, 0);
        shm_update_status(sess->shm, sess->fd, "Online");
        return;
    }

    char dirname[256] = {0};
    int copy_len = (name_len < 255) ? name_len : 255;
    memcpy(dirname, payload + 8, (size_t)copy_len);

    if (!is_path_safe(dirname)) { tx(sess, FTP_CMD_LISTDIR, 0, NULL, 0); return; }

    /* 构建目标目录的完整路径 */
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", MY_FTP_BOOT, dirname);

    /* 验证目标路径存在且确实是一个目录 */
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISDIR(st.st_mode)) {
        tx(sess, FTP_CMD_LISTDIR, 0, (unsigned char *)"not a directory", 15);
        shm_update_status(sess->shm, sess->fd, "Online");
        return;
    }

    char  buf[SIZE] = {0};
    int   off = 0;

    /* 递归列出：base_path = MY_FTP_BOOT（共享目录根），rel_prefix = dirname/ */
    char prefix[512] = {0};
    if (strcmp(dirname, ".") == 0) {
        prefix[0] = '\0';  /* 列出根目录时不加前缀 */
    } else {
        snprintf(prefix, sizeof(prefix), "%s/", dirname);
    }

    int rc = listdir_recursive(MY_FTP_BOOT, prefix, buf, &off, (int)sizeof(buf));
    if (rc == 0) {
        tx(sess, FTP_CMD_LISTDIR, 1, (unsigned char *)buf, off);
    } else {
        tx(sess, FTP_CMD_LISTDIR, 0,
            (unsigned char *)(rc == -2 ? "buffer overflow" : "readdir fail"),
            rc == -2 ? 15 : 12);
    }

    shm_update_status(sess->shm, sess->fd, "Online");
}

/* ================================================================== */
/*  GET —— 文件下载                                                       */
/*  先发送文件大小确认包，然后以裸数据流方式分块发送文件内容                   */
/* ================================================================== */
void worker_handle_get(client_session_t *sess,
                       const unsigned char *payload, int plen)
{
    /* payload 格式: [4B cmd_no] [4B name_len] [filename…] */
    if (plen < 12) { tx(sess, FTP_CMD_GET, 0, NULL, 0); return; }

    int name_len = get_le32(payload + 4);
    if (name_len < 1 || 8 + name_len > plen) {
        tx(sess, FTP_CMD_GET, 0, NULL, 0);
        return;
    }

    char filename[256] = {0};
    int copy_len = (name_len < 255) ? name_len : 255;
    memcpy(filename, payload + 8, (size_t)copy_len);

    if (!is_path_safe(filename)) { tx(sess, FTP_CMD_GET, 0, NULL, 0); return; }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", MY_FTP_BOOT, filename);

    struct stat st;
    if (stat(path, &st) < 0) {
        tx(sess, FTP_CMD_GET, 0, (unsigned char *)"not found", 9);
        return;
    }

    /* 发送带文件大小的确认包，客户端据此获知需要接收多少数据 */
    unsigned char size_le[4];
    put_le32(size_le, (int)st.st_size);
    tx(sess, FTP_CMD_GET, 1, size_le, 4);

    shm_update_status(sess->shm, sess->fd, "Downloading 0%");

    /* 以 4KB 为单位分块发送文件内容 */
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        shm_update_status(sess->shm, sess->fd, "Idle");
        return;
    }

    unsigned char buf[4096];
    int  sent     = 0;
    int  last_pct = -1;
    int  total    = (int)st.st_size;

    while (sent < total) {
        int r = (int)read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        /* 发送原始字节数据 —— 数据流不经过协议封帧 */
        int w = write_all(sess->fd, buf, (size_t)r);
        if (w < 0) break;
        sent += w;

        /* 大约每传输 10% 更新一次共享内存中的进度状态 */
        int pct = (total > 0) ? (sent * 100 / total) : 0;
        if (pct - last_pct >= 10) {
            last_pct = pct;
            char stbuf[64];
            snprintf(stbuf, sizeof(stbuf), "Downloading %d%%", pct);
            shm_update_status(sess->shm, sess->fd, stbuf);
        }
    }
    close(fd);
    shm_update_status(sess->shm, sess->fd, "Idle");

    /* 通知客户端下载传输已完成，并告知实际发送的字节数 */
    unsigned char done[4];
    put_le32(done, sent);
    tx(sess, FTP_CMD_DONE, 1, done, 4);
}

/* ================================================================== */
/*  PUT —— 文件上传                                                       */
/*  先发送确认包，然后接收客户端发来的文件数据并写入磁盘                       */
/* ================================================================== */
void worker_handle_put(client_session_t *sess,
                       const unsigned char *payload, int plen)
{
    /* payload 格式: [4B cmd_no] [4B name_len] [filename] [4B filesize] */
    if (plen < 16) { tx(sess, FTP_CMD_PUT, 0, NULL, 0); return; }

    int name_len = get_le32(payload + 4);
    if (name_len < 1 || 8 + name_len + 4 > plen) {
        tx(sess, FTP_CMD_PUT, 0, NULL, 0);
        return;
    }

    char filename[256] = {0};
    int copy_len = (name_len < 255) ? name_len : 255;
    memcpy(filename, payload + 8, (size_t)copy_len);

    int filesize = get_le32(payload + 8 + name_len);

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", MY_FTP_BOOT, filename);

    /* 如果文件名包含 '/'（即带路径），则递归创建所有父目录 */
    char *slash = strrchr(path, '/');
    if (slash) {
        char dir[512];
        size_t dlen = (size_t)(slash - path);
        if (dlen >= sizeof(dir)) dlen = sizeof(dir) - 1;
        memcpy(dir, path, dlen);
        dir[dlen] = '\0';
        mkdir_p(dir);
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        tx(sess, FTP_CMD_PUT, 0, (unsigned char *)"cannot create", 13);
        return;
    }

    /* 发送确认包，通知客户端可以开始发送文件数据 */
    ack_ok(sess, FTP_CMD_PUT);
    fprintf(stderr, "[srv put] ACK sent, waiting for %d bytes from fd=%d\n", filesize, sess->fd);

    shm_update_status(sess->shm, sess->fd, "Uploading 0%");

    /* 以 4KB 为单位分块接收文件内容 */
    unsigned char buf[4096];
    int  received  = 0;
    int  last_pct  = -1;

    while (received < filesize) {
        int remaining = filesize - received;
        int chunk = (remaining < 4096) ? remaining : 4096;
        int r = (int)read(sess->fd, buf, (size_t)chunk);
        if (r <= 0) {
            fprintf(stderr, "[srv put] read fd=%d returned %d errno=%d, breaking (received=%d/%d)\n",
                    sess->fd, r, errno, received, filesize);
            if (errno == EINTR)  continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                usleep(1000);    /* fd is non-blocking, wait for data */
                continue;
            }
            break;
        }
        int w = write_all(fd, buf, (size_t)r);
        if (w < 0) break;
        received += w;

        /* 大约每传输 10% 更新一次共享内存中的进度状态 */
        int pct = (filesize > 0) ? (received * 100 / filesize) : 0;
        if (pct - last_pct >= 10) {
            last_pct = pct;
            char stbuf[64];
            snprintf(stbuf, sizeof(stbuf), "Uploading %d%%", pct);
            shm_update_status(sess->shm, sess->fd, stbuf);
        }
    }
    close(fd);
    shm_update_status(sess->shm, sess->fd, "Idle");

    /* 通知客户端上传传输已完成，并告知实际接收的字节数
     * 如果接收到的字节数与声明的文件大小一致，则标记为成功 */
    unsigned char done[4];
    put_le32(done, received);
    tx(sess, FTP_CMD_DONE, (received == filesize) ? 1 : 0, done, 4);
}

/* ================================================================== */
/*  BYE —— 清理并断开连接                                                 */
/* ================================================================== */
void worker_handle_bye(client_session_t *sess)
{
    /* 如果已登录，从共享内存中移除客户端记录 */
    if (sess->logged_in) {
        shm_remove_client(sess->shm, sess->fd);
    }
    /* 优雅关闭连接：先 shutdown 再 close */
    shutdown(sess->fd, SHUT_RDWR);
    close(sess->fd);
    /* 不重新注册 epoll —— fd 已关闭。
     * 会话的清理工作由 main.c 中的清理路径完成：
     * 当检测到 EPOLLHUP 时（或在下次 epoll_wait 中，
     * 已关闭的 fd 会触发 EPOLLERR），会话将被释放。 */
    sess->fd = -1;
}

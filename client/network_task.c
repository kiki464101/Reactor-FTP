#define _GNU_SOURCE
/**
 * @file network_task.c
 * @brief LVGL FTP 客户端网络线程模块实现
 *
 * 本模块运行在独立的 pthread 中，负责以下核心功能：
 *
 * 1. TCP 连接管理：创建套接字、连接服务器、发送登录认证
 * 2. 协议解析：接收原始 TCP 数据，解析应用层协议数据包
 *    - 协议格式：0xC0 + pkg_len(4B 小端) + cmd_no(4B 小端) + [参数...] + 0xC0
 * 3. 文件下载流程：
 *    UI 发送 GET 命令 → 网络线程读取响应（获取文件大小）→ 进入 DOWNLOAD 状态
 *    → 从套接字读取原始字节 → 写入本地文件
 *    → 定时通过 lv_async_call() 更新进度条
 * 4. 文件上传流程：
 *    UI 发送 PUT 命令 → 网络线程读取服务器确认 → 进入 UPLOAD 状态
 *    → 读取本地文件 → 将原始字节写入套接字
 *    → 定时通过 lv_async_call() 更新进度条
 * 5. 批量传输：多文件通过传输队列和线程池并发处理
 *
 * 线程安全设计：
 *   - 所有 LVGL 组件操作必须通过 lv_async_call() 在 UI 线程中执行
 *   - 传输队列使用互斥锁 + 条件变量保护
 *   - 全局进度状态由网络线程写入，UI 线程只读
 */

#include "network_task.h"
#include "ui_manager.h"

#include "protocol.h"
#include "../lvgl/lvgl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>

/* ================================================================== */
/*  全局变量定义                                                        */
/* ================================================================== */
int          g_sockfd           = -1;       /* 与服务器的主 TCP 套接字描述符      */
pthread_t    g_net_thread       = 0;        /* 网络主线程句柄                      */
bool         g_network_running  = false;    /* 网络线程运行标志                    */
bool         g_login_ok         = false;    /* 登录认证是否成功                    */
char         g_session_info[64] = {0};      /* 服务器返回的会话标识信息            */

transfer_progress_t g_transfer_progress = {0};  /* 全局传输进度状态                */

/* 保存的登录凭据（供传输工作线程在需要时重新连接） */
char g_login_ip[64]   = {0};    /* 服务器 IP 地址                       */
char g_login_port[16] = {0};    /* 服务器端口号                         */
char g_login_user[64] = {0};    /* 登录用户名                           */
char g_login_pass[64] = {0};    /* 登录密码                             */

/* 传输队列和线程池 */
transfer_queue_t  g_tx_queue;                           /* 全局传输任务队列    */
transfer_worker_t g_tx_workers[TRANSFER_POOL_SIZE];     /* 传输工作线程池      */
volatile int      g_active_transfers = 0;               /* 当前活跃传输计数    */

/* ================================================================== */
/*  内部下载状态管理                                                     */
/*  这些变量仅在网络线程中访问，不需要加锁                                */
/* ================================================================== */
#define CHUNK_SIZE 4096             /* 每次读写的块大小（4KB）             */
#define CHUNK_DELAY_US 100000       /* 每块之间的延迟（100ms），让进度条可见 */

/* ---- 辅助函数：从目录路径中提取最后一级目录名 ---- */
/**
 * 从完整路径中提取最后一部分作为目录名
 * 例如：输入 "a/b/c/" → 输出 "c"
 * @param path    源路径字符串
 * @param out     输出缓冲区
 * @param out_sz  输出缓冲区大小
 */
static void folder_basename(const char *path, char *out, size_t out_sz)
{
    if (!path || !out || out_sz == 0) return;
    size_t len = strlen(path);
    /* 去除末尾的 '/' 字符 */
    const char *end = path + len;
    while (end > path && *(end - 1) == '/') end--;
    /* 找到最后一个 '/' */
    const char *last = end;
    while (last > path && *(last - 1) != '/') last--;
    size_t n = (size_t)(end - last);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, last, n);
    out[n] = '\0';
}

/* ---- 辅助函数：递归遍历本地目录，收集所有文件到传输任务数组 ---- */
/**
 * 递归遍历目录，将所有文件加入传输任务列表
 * 子目录会被递归展开，普通文件直接作为 PUT 任务
 *
 * @param local_base   本地基准路径（如 "./client"）
 * @param rel_prefix   当前相对路径前缀（递归时逐层追加）
 * @param out          输出任务数组
 * @param out_count    当前已收集的任务数（作为输入输出参数）
 * @param max          任务数组最大容量
 */
static void collect_files_recursive(const char *local_base,
                                     const char *rel_prefix,
                                     transfer_task_t *out,
                                     int *out_count, int max)
{
    if (*out_count >= max) return;
    char scan_dir[520];
    snprintf(scan_dir, sizeof(scan_dir), "%s/%s", local_base, rel_prefix);
    DIR *dir = opendir(scan_dir);
    if (!dir) return;                                   /* 目录打开失败则跳过 */
    struct dirent *d;
    while ((d = readdir(dir)) != NULL && *out_count < max) {
        /* 跳过 "." 和 ".." 特殊目录项 */
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;
        char item_path[520];
        snprintf(item_path, sizeof(item_path), "%s/%s", scan_dir, d->d_name);
        struct stat st;
        if (stat(item_path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            /* 子目录：递归遍历 */
            char sub[520];
            snprintf(sub, sizeof(sub), "%s%s/", rel_prefix, d->d_name);
            collect_files_recursive(local_base, sub, out, out_count, max);
        } else {
            /* 普通文件：创建上传任务 */
            memset(&out[*out_count], 0, sizeof(transfer_task_t));
            snprintf(out[*out_count].filename, sizeof(out[*out_count].filename),
                     "%s%s", rel_prefix, d->d_name);
            out[*out_count].is_upload = true;
            (*out_count)++;
        }
    }
    closedir(dir);
}

/* ---- 辅助函数：递归创建目录（类似 mkdir -p 命令） ---- */
/**
 * 递归创建路径中的所有目录
 * 例如：输入 "a/b/c" → 依次创建 a, a/b, a/b/c
 * @param path  要创建的目录路径
 */
static void mkdir_p(const char *path)
{
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (len > 0 && tmp[len - 1] == '/')
        tmp[len - 1] = '\0';                            /* 去掉末尾的 '/' */
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);                           /* 创建中间目录 */
            *p = '/';
        }
    }
    mkdir(tmp, 0755);                                   /* 创建最终目录 */
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
/*  网络线程状态机                                                      */
/*  控制网络线程的行为流程                                              */
/* ================================================================== */
typedef enum {
    ST_IDLE,                /* 空闲状态：等待新任务或服务器响应              */
    ST_LOGIN_SENT,          /* 已发送登录请求，等待服务器认证回复            */
    ST_WAIT_LS_RESP,        /* 等待 LS 命令的响应（文件列表）               */
    ST_WAIT_GET_RESP,       /* 等待 GET 命令的响应（文件大小等信息）         */
    ST_DOWNLOADING,         /* 正在下载文件（接收原始字节流）                */
    ST_WAIT_PUT_RESP,       /* 等待 PUT 命令的响应（服务器确认可上传）       */
    ST_UPLOADING,           /* 正在上传文件（发送原始字节流）                */
    ST_WAIT_LISTDIR_RESP,   /* 等待 LISTDIR 响应（目录下的文件路径列表）     */
} net_state_t;

/* 当前状态机状态及其相关数据（仅网络线程访问，无需加锁） */
static net_state_t  g_state            = ST_IDLE;      /* 当前状态         */
static int          g_dl_fd            = -1;           /* 下载时本地文件描述符 */
static int          g_dl_total         = 0;            /* 下载文件总字节数    */
static int          g_dl_received      = 0;            /* 下载已接收字节数    */
static char         g_dl_filename[256] = {0};          /* 正在下载的文件名    */
static int          g_last_progress    = -1;           /* 上次报告进度百分比（避免频繁刷新） */

static int          g_ul_fd            = -1;           /* 上传时本地文件描述符 */
static int          g_ul_total         = 0;            /* 上传文件总字节数    */
static int          g_ul_sent          = 0;            /* 上传已发送字节数    */
static char         g_ul_filename[256]      = {0};          /* 正在上传的文件名（服务器端） */
static char         g_ul_local_path[520]   = {0};          /* 上传文件的本地完整路径 */

/* ================================================================== */
/*  底层 I/O 辅助函数                                                   */
/* ================================================================== */

/**
 * 从套接字精确读取 N 个字节（阻塞模式）
 * 循环读取直到读满 N 字节或遇到 EOF/错误
 *
 * @param fd   套接字文件描述符
 * @param buf  接收缓冲区
 * @param n    需要读取的字节数
 * @return     成功返回实际读取的字节数（应等于 n），
 *             EOF 返回已读字节数（小于 n），错误返回 -1
 */
static int read_n(int fd, unsigned char *buf, int n)
{
    int offset = 0;
    while (offset < n) {
        int r = read(fd, buf + offset, (size_t)(n - offset));
        if (r <= 0) {
            if (r == 0) return offset;                  /* EOF：返回已读取的字节数 */
            if (errno == EINTR) continue;               /* 被信号中断，重试 */
            return -1;                                  /* 真正的 I/O 错误 */
        }
        offset += r;
    }
    return offset;
}

/* ================================================================== */
/*  数据包级别操作函数                                                   */
/*  协议格式：0xC0 + pkg_len(4B LE) + cmd_no(4B LE) + [参数...] + 0xC0  */
/* ================================================================== */

/**
 * 从套接字读取一个完整的协议数据包
 *
 * 读取流程：
 *   1. 寻找包头 0xC0
 *   2. 跳过连续的 0xC0 字节（可能是上一个数据包的包尾）
 *   3. 读取 4 字节的 pkg_len（小端模式）
 *   4. 根据 pkg_len 计算有效载荷长度 = pkg_len - 6（1 包头 + 4 长度 + 1 包尾）
 *   5. 使用 read_n() 精确读取有效载荷（避免载荷中 0xC0 造成误判）
 *   6. 读取并验证包尾 0xC0
 *
 * @param fd        套接字文件描述符
 * @param out_len   输出参数：有效载荷长度（不含包头/包尾/长度字段）
 * @return          成功返回 malloc 分配的有效载荷缓冲区（调用者负责 free），
 *                  失败或断开连接返回 NULL
 */
static unsigned char *read_packet(int fd, int *out_len)
{
    unsigned char ch;
    /* ----- 步骤1：寻找包头 0xC0 ----- */
    while (1) {
        int r = read(fd, &ch, 1);
        if (r <= 0) return NULL;                        /* 连接断开或读取错误 */
        if (ch == 0xC0) break;                          /* 找到包头 */
    }
    /* ----- 步骤2：跳过可能存在的连续 0xC0（上一个数据包的包尾） ----- */
    while (1) {
        int r = read(fd, &ch, 1);
        if (r <= 0) return NULL;
        if (ch != 0xC0) break;                          /* ch 现在是 pkg_len 的第一个字节 */
    }
    /* ----- 步骤3：读取 pkg_len（小端模式，共4字节） ----- */
    int pkg_len = ch;                                   /* 第一个字节已读取 */
    int i;
    for (i = 1; i < 4; i++) {
        if (read(fd, &ch, 1) <= 0) return NULL;
        pkg_len |= (ch << (8 * i));                     /* 小端拼接：低位在前 */
    }
    if (pkg_len < 10) return NULL;                      /* 最小包长校验：1+4+4+1=10 */
    /* ----- 步骤4：计算有效载荷长度 ----- */
    int payload_len = pkg_len - 6;                      /* 减去 1 包头 + 4 长度 + 1 包尾 */
    if (payload_len <= 0) return NULL;
    unsigned char *payload = (unsigned char *)malloc((size_t)payload_len);
    if (!payload) return NULL;
    /* ----- 步骤5：精确读取有效载荷（使用 read_n 避免载荷中的 0xC0 造成提前终止） ----- */
    if (read_n(fd, payload, payload_len) != payload_len) {
        free(payload);
        return NULL;
    }
    /* ----- 步骤6：读取并验证包尾 0xC0 ----- */
    if (read(fd, &ch, 1) <= 0 || ch != 0xC0) {
        free(payload);
        return NULL;
    }
    *out_len = payload_len;
    return payload;
}

/**
 * 将 32 位整数以小端模式写入缓冲区
 * @param buf  目标缓冲区（至少 4 字节）
 * @param val  要写入的值
 */
static inline void put_le32(unsigned char *buf, int val)
{
    buf[0] = (unsigned char)( val        & 0xFF);       /* 低 8 位  */
    buf[1] = (unsigned char)((val >> 8)  & 0xFF);       /* 次低 8 位 */
    buf[2] = (unsigned char)((val >> 16) & 0xFF);       /* 次高 8 位 */
    buf[3] = (unsigned char)((val >> 24) & 0xFF);       /* 高 8 位   */
}

/**
 * 从缓冲区读取 32 位小端整数
 * @param buf  源缓冲区（至少 4 字节）
 * @return     解析出的整数值
 */
static inline int get_le32(const unsigned char *buf)
{
    return buf[0] | (buf[1] << 8) | (buf[2] << 16) | (buf[3] << 24);
}

/* ================================================================== */
/*  数据包构建函数                                                       */
/* ================================================================== */

/**
 * 构建一个完整的协议命令数据包（malloc 分配，调用者负责 free）
 *
 * 数据包格式：
 *   0xC0 | pkg_len(4B LE) | cmd_no(4B LE) | [arg_data] | 0xC0
 *
 * @param cmd_no        命令号（如 FTP_CMD_LS, FTP_CMD_GET 等）
 * @param arg_data      参数数据（可为 NULL）
 * @param arg_len       参数数据长度（无参数时传 0）
 * @param out_total_len 输出参数：整个数据包的总长度
 * @return              成功返回 malloc 分配的数据包缓冲区，失败返回 NULL
 */
static unsigned char *build_cmd(int cmd_no,
                                const unsigned char *arg_data, int arg_len,
                                int *out_total_len)
{
    int pkg_len = 10 + arg_len;                         /* 包头(1) + 长度(4) + 命令号(4) + 包尾(1) */
    unsigned char *pkt = (unsigned char *)malloc((size_t)pkg_len);
    if (!pkt) return NULL;
    int i = 0;
    pkt[i++] = 0xC0;                                    /* 写入包头 */
    put_le32(pkt + i, pkg_len);  i += 4;                /* 写入 pkg_len（小端） */
    put_le32(pkt + i, cmd_no);   i += 4;                /* 写入 cmd_no（小端）  */
    if (arg_data && arg_len > 0) {
        memcpy(pkt + i, arg_data, (size_t)arg_len);     /* 写入参数数据 */
        i += arg_len;
    }
    pkt[i++] = 0xC0;                                    /* 写入包尾 */
    *out_total_len = pkg_len;
    return pkt;
}

/**
 * 构建一个带字符串参数的命令数据包
 * 参数格式：[4B 字符串长度(小端)] + [字符串内容]
 *
 * @param cmd_no   命令号
 * @param str      字符串参数
 * @param out_len  输出参数：数据包总长度
 * @return         成功返回 malloc 分配的数据包，失败返回 NULL
 */
static unsigned char *build_cmd_with_str(int cmd_no, const char *str,
                                          int *out_len)
{
    int slen = (int)strlen(str);
    int arg_len = 4 + slen;                             /* 长度字段(4) + 字符串内容 */
    unsigned char *arg = (unsigned char *)malloc((size_t)arg_len);
    if (!arg) return NULL;
    put_le32(arg, slen);                                /* 先写长度 */
    memcpy(arg + 4, str, (size_t)slen);                  /* 再写内容 */
    unsigned char *pkt = build_cmd(cmd_no, arg, arg_len, out_len);
    free(arg);
    return pkt;
}

/**
 * 构建一个 PUT 命令数据包（包含文件名和文件大小两个参数）
 * 协议格式：[4B 命令号] [4B 参数总长度] [4B 文件名长度] [文件名] [4B 文件大小]
 *
 * @param filename  要上传的文件名
 * @param filesize  文件大小（字节）
 * @param out_len   输出参数：数据包总长度
 * @return          成功返回 malloc 分配的数据包，失败返回 NULL
 */
static unsigned char *build_cmd_put(const char *filename, int filesize,
                                     int *out_len)
{
    int slen = (int)strlen(filename);
    int arg_len = 4 + slen + 4;                         /* name_len(4) + name + filesize(4) */
    unsigned char *arg = (unsigned char *)malloc((size_t)arg_len);
    if (!arg) return NULL;
    put_le32(arg,      slen);                           /* 写入文件名长度 */
    memcpy(arg + 4, filename, (size_t)slen);             /* 写入文件名内容 */
    put_le32(arg + 4 + slen, filesize);                  /* 写入文件大小（4字节小端） */
    unsigned char *pkt = build_cmd(FTP_CMD_PUT, arg, arg_len, out_len);
    free(arg);
    return pkt;
}

/* ================================================================== */
/*  响应数据包解析                                                       */
/*  响应包格式：0xC0|pkg_len(4B)|cmd_no(4B)|res_len(4B)|res_result(1B)|res_data|0xC0 */
/* ================================================================== */

/** 解析后的响应数据结构 */
typedef struct {
    int  cmd_no;                /* 命令号（应与请求一致）                  */
    int  res_result;            /* 操作结果：0=失败, 1=成功               */
    int  res_len;               /* 回复数据内容长度 = 1(result) + N(data) */
    const unsigned char *res_data;  /* 指向有效载荷中回复数据的指针       */
} resp_t;

/**
 * 解析响应数据包的载荷部分
 *
 * @param payload      有效载荷缓冲区
 * @param payload_len  有效载荷长度
 * @param rsp          输出参数：解析结果
 * @return             true=解析成功, false=数据不完整
 */
static bool parse_response(const unsigned char *payload, int payload_len,
                            resp_t *rsp)
{
    if (payload_len < 9) return false;                  /* 最小长度校验：cmd_no(4)+res_len(4)+res_result(1)=9 */
    rsp->cmd_no     = get_le32(payload + 0);             /* 字节 0-3：命令号   */
    rsp->res_len    = get_le32(payload + 4);             /* 字节 4-7：回复长度 */
    rsp->res_result = payload[8];                        /* 字节 8  ：操作结果 */
    rsp->res_data   = payload + 9;                       /* 字节 9+ ：回复数据 */
    return true;
}

/* ================================================================== */
/*  传输队列操作（线程安全）                                              */
/*  使用环形缓冲区 + 互斥锁 + 条件变量 实现                                */
/* ================================================================== */

/** 初始化传输队列 */
static void tx_queue_init(transfer_queue_t *q)
{
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mutex, NULL);                /* 初始化互斥锁 */
    pthread_cond_init(&q->cond, NULL);                  /* 初始化条件变量 */
}

/** 销毁传输队列（释放同步原语） */
static void tx_queue_destroy(transfer_queue_t *q)
{
    pthread_mutex_destroy(&q->mutex);
    pthread_cond_destroy(&q->cond);
}

/**
 * 向传输队列尾部添加一个任务（生产者）
 * 如果队列已满则返回 false
 *
 * @param q     传输队列指针
 * @param task  要添加的任务
 * @return      true=入队成功, false=队列已满
 */
static bool tx_queue_push(transfer_queue_t *q, const transfer_task_t *task)
{
    pthread_mutex_lock(&q->mutex);
    if (q->count >= MAX_QUEUE_SIZE) {                   /* 队列已满 */
        pthread_mutex_unlock(&q->mutex);
        return false;
    }
    q->tasks[q->tail] = *task;                          /* 写入环形缓冲区尾部 */
    q->tail = (q->tail + 1) % MAX_QUEUE_SIZE;           /* 尾指针前移（环）   */
    q->count++;                                          /* 计数加一            */
    pthread_cond_signal(&q->cond);                       /* 唤醒等待的消费者    */
    pthread_mutex_unlock(&q->mutex);
    return true;
}

/**
 * 从传输队列头部取出一个任务（消费者）
 * 如果队列为空且未取消，则阻塞等待直到有新任务或队列被取消
 *
 * @param q     传输队列指针
 * @param task  输出参数：取出的任务
 * @return      true=取任务成功, false=队列已取消且为空
 */
static bool tx_queue_pop(transfer_queue_t *q, transfer_task_t *task)
{
    pthread_mutex_lock(&q->mutex);
    while (q->count == 0 && !q->cancelled) {
        pthread_cond_wait(&q->cond, &q->mutex);          /* 队列空且未取消 → 阻塞等待 */
    }
    if (q->cancelled && q->count == 0) {                 /* 被取消且队列为空 */
        pthread_mutex_unlock(&q->mutex);
        return false;
    }
    *task = q->tasks[q->head];                           /* 读取环形缓冲区头部 */
    q->head = (q->head + 1) % MAX_QUEUE_SIZE;            /* 头指针前移（环）   */
    q->count--;                                           /* 计数减一            */
    pthread_mutex_unlock(&q->mutex);
    return true;
}

/** 取消队列中的所有任务并唤醒所有等待的消费者线程 */
static void tx_queue_cancel_all(transfer_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    q->cancelled = true;
    pthread_cond_broadcast(&q->cond);                    /* 广播唤醒所有等待线程 */
    q->head = q->tail = q->count = 0;                    /* 清空队列 */
    pthread_mutex_unlock(&q->mutex);
}

/** 重置传输队列（清空并恢复为非取消状态） */
static void tx_queue_reset(transfer_queue_t *q)
{
    pthread_mutex_lock(&q->mutex);
    q->cancelled = false;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_unlock(&q->mutex);
}

/* ================================================================== */
/*  异步回调包装函数                                                     */
/*  这些函数由网络线程通过 lv_async_call() 调度，在 UI 线程上下文中执行     */
/* ================================================================== */

/** 字符串数据类型，用于向 UI 线程传递文本信息 */
typedef struct {
    char text[512];
} str_data_t;

/** 在堆上分配并复制字符串数据（由回调函数负责 free） */
static str_data_t *make_str_data(const char *s)
{
    str_data_t *d = (str_data_t *)malloc(sizeof(str_data_t));
    if (d) { strncpy(d->text, s, sizeof(d->text) - 1); d->text[sizeof(d->text)-1] = '\0'; }
    return d;
}

/** 登录结果回调：登录成功切换到主界面，失败显示错误信息 */
static void cb_login_result(void *data)
{
    str_data_t *d = (str_data_t *)data;
    if (g_login_ok) {
        ui_switch_to_main();                            /* 登录成功 → 切换到文件管理主界面 */
    } else {
        /* ui_show_error 同时覆盖登录界面和主界面的错误显示
         * 当还在登录界面时，也会重新启用登录按钮 */
        ui_show_error(d ? d->text : "Login failed");
    }
    free(d);
}

/** 通用错误回调：在状态栏显示错误信息 */
static void cb_error(void *data)
{
    str_data_t *d = (str_data_t *)data;
    ui_show_error(d ? d->text : "Unknown error");
    free(d);
}

/** 断开连接回调：重置登录状态，切换回登录界面 */
static void cb_disconnected(void *data)
{
    (void)data;
    g_login_ok = false;
    ui_switch_to_login();
}

/** 下载/上传进度更新回调：更新进度条和批量传输面板 */
static void cb_dl_progress(void *data)
{
    transfer_progress_t *p = (transfer_progress_t *)data;
    ui_update_progress(p->percent, p->current_bytes, p->total_bytes,
                       p->filename, p->is_upload);
    /* 同时更新批量传输面板中的进度条 */
    ui_update_transfer_progress(p->filename, p->percent,
                                 p->current_bytes, p->total_bytes,
                                 p->is_upload);
    free(p);
}

/** 下载完成回调：隐藏进度条，显示完成状态 */
static void cb_dl_done(void *data)
{
    str_data_t *d = (str_data_t *)data;
    ui_hide_progress();
    ui_set_status(d ? d->text : "Download complete");
    ui_restore_status_after_delay();                    /* 3秒后恢复状态栏 */
    free(d);
}

/** 进度弹窗显示数据类型 */
typedef struct {
    char filename[256];
    bool is_upload;
} show_progress_data_t;

/**
 * 显示进度弹窗的回调
 * 弹窗由 UI 按钮处理函数同步创建，网络线程只负责驱动数据传输；
 * UI 层拥有所有组件的所有权。
 */
static void cb_show_progress(void *data)
{
    free(data);
}

/** 显示批量传输进度面板的回调 */
static void cb_show_progress_batch(void *data)
{
    (void)data;
    ui_show_progress_batch();
}

/** 上传完成回调：隐藏进度条，显示完成状态 */
static void cb_ul_done(void *data)
{
    str_data_t *d = (str_data_t *)data;
    ui_hide_progress();
    ui_set_status(d ? d->text : "Uploaded");
    ui_restore_status_after_delay();                    /* 3秒后恢复状态栏 */
    free(d);
}

/* ---- 多文件批量传输状态 ---- */
static int           g_batch_total  = 0;     /* 批量传输的文件总数           */
static int           g_batch_done   = 0;     /* 已完成传输的文件数           */
static bool          g_batch_active = false; /* 是否正在进行批量传输         */
static volatile bool g_transfer_cancelled = false; /* 传输取消标志（多线程访问） */

/** 单个传输完成的数据类型 */
typedef struct {
    char filename[256];     /* 文件名       */
    bool success;           /* 是否成功     */
    bool is_upload;         /* 上传/下载    */
} tx_done_data_t;

/** 单个传输完成回调：在批量面板中更新条目状态 */
static void cb_tx_done(void *data)
{
    tx_done_data_t *d = (tx_done_data_t *)data;
    ui_on_transfer_done(d->filename, d->success, d->is_upload);
    free(d);
}

/** 显示错误弹窗的回调 */
static void cb_show_error_popup(void *data)
{
    str_data_t *d = (str_data_t *)data;
    if (d) { ui_show_error_popup(d->text); free(d); }
}

/**
 * 标准化本地文件路径：将各种格式的路径统一为 "./client/..." 格式
 * 处理以下前缀：
 *   "./client/" → 保持不变
 *   "client/"   → 加上 "./"
 *   "./"        → 加上 "client/"
 *
 * @param filename  原始文件名
 * @param path      输出缓冲区
 * @param path_sz   输出缓冲区大小
 */
static void normalize_local_path(const char *filename, char *path, size_t path_sz)
{
    const char *src = filename;
    if (!src || !path || path_sz == 0) return;
    if (strncmp(src, "./client/", 9) == 0) src += 9;
    else if (strncmp(src, "client/", 7) == 0) src += 7;
    else if (strncmp(src, "./", 2) == 0) src += 2;

    snprintf(path, path_sz, "./client/%s", src[0] ? src : "");
}

/* ================================================================== */
/*  批量传输任务调度                                                     */
/* ================================================================== */

/**
 * 启动队列中的下一个传输任务
 * 在 IDLE 状态下由主循环调用
 *
 * @return  true=成功启动了一个传输, false=队列为空或启动失败
 */
static bool batch_start_next(void)
{
    if (g_tx_queue.count == 0)
        return false;

    transfer_task_t task;
    if (!tx_queue_pop(&g_tx_queue, &task))
        return false;

    bool ok;
    if (task.is_upload)
        ok = network_cmd_put(task.filename, task.local_path);   /* 启动单个文件上传 */
    else
        ok = network_cmd_get(task.filename);            /* 启动单个文件下载 */

    if (!ok) {
        /* 传输启动失败 → 标记为失败并跳过，继续处理下一个 */
        printf("[DEBUG] batch_start_next: failed to start '%s', skipping\n", task.filename);
        g_batch_done++;
        /* 立即尝试下一个任务 */
        return batch_start_next();
    }
    /* 第一个任务启动时显示进度弹窗 */
    if (g_batch_done == 0) {
        lv_async_call(cb_show_progress_batch, NULL);
    }
    return true;
}

static bool          g_batch_is_upload = false;  /* 批量传输方向（用于完成后自动刷新正确的文件列表） */

/** 刷新本地文件列表的回调（批量下载完成后） */
static void cb_refresh_local_list(void *data)
{
    (void)data;
    ui_refresh_local_files_root();
}

/** 刷新远程文件列表的回调（批量上传完成后） */
static void cb_refresh_remote_list(void *data)
{
    (void)data;
    ui_refresh_remote_list_root();
}

/**
 * 检查批量传输是否已全部完成
 * 条件：批量传输活跃 && 队列为空 && 当前状态为 IDLE
 * 完成后自动刷新对应的文件列表
 */
static void batch_check_complete(void)
{
    if (g_batch_active && g_tx_queue.count == 0 && g_state == ST_IDLE) {
        g_batch_active = false;
        str_data_t *msg = make_str_data("Transfer complete");
        if (msg) lv_async_call(cb_dl_done, msg);
        /* 根据传输方向自动刷新文件列表 */
        if (g_batch_is_upload)
            lv_async_call(cb_refresh_remote_list, NULL);    /* 上传完成 → 刷新远程列表 */
        else
            lv_async_call(cb_refresh_local_list, NULL);     /* 下载完成 → 刷新本地列表 */
    }
}

/* ================================================================== */
/*  下载状态管理（在网線线程中调用）                                      */
/* ================================================================== */

/**
 * 开始文件下载
 * 创建/截断本地文件，初始化下载状态，进入 ST_DOWNLOADING 状态
 *
 * @param filename  要保存的文件名（相对路径）
 * @param filesize  服务器告知的文件总大小
 */
static void start_download(const char *filename, int filesize)
{
    strncpy(g_dl_filename, filename, sizeof(g_dl_filename) - 1);
    g_dl_total    = filesize;
    g_dl_received = 0;
    g_last_progress = -1;
    g_active_transfers++;

    /* 创建/截断本地文件 —— 所有下载文件保存到 ./client/load/<filename> */
    char dl_path[520];
    snprintf(dl_path, sizeof(dl_path), "./client/load/%s", filename);

    /* 通过 mkdir_p 创建所需的父目录 */
    {
        char dl_dir[520];
        snprintf(dl_dir, sizeof(dl_dir), "./client/load/%s", filename);
        char *slash = strrchr(dl_dir, '/');
        if (slash) {
            *slash = '\0';
            mkdir_p(dl_dir);                            /* 递归创建目录 */
        }
    }
    g_dl_fd = open(dl_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_dl_fd < 0) {
        str_data_t *err = make_str_data("Failed to create local file");
        if (err) lv_async_call(cb_error, err);
        g_state = ST_IDLE;                              /* 创建失败 → 回到空闲 */
        return;
    }

    /* 初始化全局传输进度 */
    g_transfer_progress.active  = true;
    g_transfer_progress.is_upload = false;
    g_transfer_progress.percent = 0;
    g_transfer_progress.current_bytes = 0;
    g_transfer_progress.total_bytes = filesize;
    strncpy(g_transfer_progress.filename, filename,
            sizeof(g_transfer_progress.filename) - 1);

    g_state = ST_DOWNLOADING;                           /* 进入下载状态 */
}

/**
 * 更新下载进度
 * 计算完成百分比，每变化 1% 或到达 100% 时通过 lv_async_call 通知 UI
 */
static void update_dl_progress(void)
{
    int pct = (g_dl_total > 0) ? (g_dl_received * 100 / g_dl_total) : 0;
    if (pct - g_last_progress < 1 && pct < 100) return; /* 进度未变化且未完成 → 跳过 */
    g_last_progress = pct;

    transfer_progress_t *tp = (transfer_progress_t *)malloc(sizeof(transfer_progress_t));
    if (!tp) return;
    tp->percent       = pct;
    tp->current_bytes = g_dl_received;
    tp->total_bytes   = g_dl_total;
    tp->active        = true;
    tp->is_upload     = false;
    strncpy(tp->filename, g_dl_filename, sizeof(tp->filename) - 1);
    lv_async_call(cb_dl_progress, tp);                   /* 异步通知 UI 更新进度条 */
}

/**
 * 完成下载（无论成功或失败）
 * 关闭本地文件，重置下载状态，通知 UI
 * 如果处于批量传输模式，递增完成计数并检查是否全部完成
 *
 * @param success  true=下载成功, false=下载失败
 */
static void finish_download(bool success)
{
    if (g_dl_fd >= 0) { close(g_dl_fd); g_dl_fd = -1; }
    g_dl_received = 0;
    g_dl_total    = 0;
    g_last_progress = -1;
    g_transfer_progress.active = false;
    g_state = ST_IDLE;                                   /* 回到空闲状态 */
    g_transfer_cancelled = false;
    if (g_active_transfers > 0) g_active_transfers--;

    printf("[DEBUG] finish_download: %s success=%d batch=%d done=%d/%d queue=%d\n",
           g_dl_filename, success, g_batch_active, g_batch_done, g_batch_total, g_tx_queue.count);

    /* 向批量面板报告每个文件的传输结果 */
    {
        tx_done_data_t *d = (tx_done_data_t *)malloc(sizeof(*d));
        if (d) {
            strncpy(d->filename, g_dl_filename, sizeof(d->filename) - 1);
            d->filename[sizeof(d->filename) - 1] = '\0';
            d->success   = success;
            d->is_upload = false;
            lv_async_call(cb_tx_done, d);
        }
    }

    if (g_batch_active) {
        g_batch_done++;
        /* 批量模式下不立即显示 "Downloaded"，由 batch_check_complete 统一处理 */
    } else {
        str_data_t *msg = make_str_data(success
            ? "Downloaded"
            : "Download failed");
        if (msg) lv_async_call(cb_dl_done, msg);
    }

    /* 消费服务器的 DONE 确认包，避免污染下一次响应解析 */
    {
        int dlen;
        unsigned char *done = read_packet(g_sockfd, &dlen);
        if (done) free(done);
    }
}

/* ================================================================== */
/*  上传状态管理（在网络线程中调用）                                      */
/* ================================================================== */

/**
 * 开始文件上传
 * 打开本地文件，获取文件大小，初始化上传状态，进入 ST_UPLOADING 状态
 *
 * @param filename    要上传的文件名（服务器端名称，用于 UI 显示）
 * @param local_path  本地文件完整路径（用于 open）
 */
static void start_upload(const char *filename, const char *local_path)
{
    strncpy(g_ul_filename, filename, sizeof(g_ul_filename) - 1);

    g_ul_fd = open(local_path, O_RDONLY);
    if (g_ul_fd < 0) {
        str_data_t *err = make_str_data("Local file not found");
        if (err) lv_async_call(cb_show_error_popup, err);
        g_state = ST_IDLE;                              /* 文件不存在 → 回到空闲 */
        return;
    }
    /* 获取文件大小（seek 到末尾再回到开头） */
    off_t sz = lseek(g_ul_fd, 0, SEEK_END);
    lseek(g_ul_fd, 0, SEEK_SET);
    g_ul_total = (int)sz;
    g_ul_sent  = 0;

    fprintf(stderr, "[upload] start: %s fd=%d total=%d\n", local_path, g_ul_fd, g_ul_total);
    if (g_ul_total <= 0) {
        fprintf(stderr, "[upload] WARNING: g_ul_total=%d <= 0!\n", g_ul_total);
    }

    /* 初始化全局传输进度 */
    g_transfer_progress.active  = true;
    g_transfer_progress.is_upload = true;
    g_transfer_progress.percent = 0;
    g_transfer_progress.current_bytes = 0;
    g_transfer_progress.total_bytes = g_ul_total;
    strncpy(g_transfer_progress.filename, filename,
            sizeof(g_transfer_progress.filename) - 1);
    g_last_progress = -1;
    g_active_transfers++;

    g_state = ST_UPLOADING;                             /* 进入上传状态 */
}

/**
 * 更新上传进度
 * 每变化 1% 或到达 100% 时通知 UI
 */
static void update_ul_progress(void)
{
    int pct = (g_ul_total > 0) ? (g_ul_sent * 100 / g_ul_total) : 0;
    if (pct - g_last_progress < 1 && pct < 100) return; /* 进度未变化且未完成 → 跳过 */
    g_last_progress = pct;

    transfer_progress_t *tp = (transfer_progress_t *)malloc(sizeof(transfer_progress_t));
    if (!tp) return;
    tp->percent       = pct;
    tp->current_bytes = g_ul_sent;
    tp->total_bytes   = g_ul_total;
    tp->active        = true;
    tp->is_upload     = true;
    strncpy(tp->filename, g_ul_filename, sizeof(tp->filename) - 1);
    lv_async_call(cb_dl_progress, tp);                   /* 异步通知 UI */
}

/**
 * 完成上传（无论成功或失败）
 * 关闭本地文件，重置上传状态，通知 UI
 *
 * @param success  true=上传成功, false=上传失败
 */
static void finish_upload(bool success)
{
    if (g_ul_fd >= 0) { close(g_ul_fd); g_ul_fd = -1; }
    g_ul_sent  = 0;
    g_ul_total = 0;
    g_last_progress = -1;
    g_transfer_progress.active = false;
    g_state = ST_IDLE;                                   /* 回到空闲状态 */
    g_transfer_cancelled = false;
    if (g_active_transfers > 0) g_active_transfers--;

    printf("[DEBUG] finish_upload: %s success=%d batch=%d done=%d/%d queue=%d\n",
           g_ul_filename, success, g_batch_active, g_batch_done, g_batch_total, g_tx_queue.count);

    /* 向批量面板报告每个文件的传输结果 */
    {
        tx_done_data_t *d = (tx_done_data_t *)malloc(sizeof(*d));
        if (d) {
            strncpy(d->filename, g_ul_filename, sizeof(d->filename) - 1);
            d->filename[sizeof(d->filename) - 1] = '\0';
            d->success   = success;
            d->is_upload = true;
            lv_async_call(cb_tx_done, d);
        }
    }

    if (g_batch_active) {
        g_batch_done++;
        /* 批量模式下不立即显示 "Uploaded"，由 batch_check_complete 统一处理 */
    } else {
        str_data_t *msg = make_str_data(success
            ? "Uploaded"
            : "Upload failed");
        if (msg) lv_async_call(cb_ul_done, msg);
    }

    /* 消费服务器的 DONE 确认包，避免污染下一次响应解析 */
    {
        int dlen;
        unsigned char *done = read_packet(g_sockfd, &dlen);
        if (done) free(done);
    }
}

/* ================================================================== */
/*  下载数据块处理                                                       */
/* ================================================================== */

/**
 * 从套接字读取一块下载数据并写入本地文件
 *
 * @return   1 = 传输完成（已接收全部数据）
 *           0 = 正常（继续读取下一块）
 *          -1 = 连接错误
 */
static int handle_download_chunk(void)
{
    unsigned char buf[CHUNK_SIZE];
    int remaining = g_dl_total - g_dl_received;
    if (remaining <= 0) return 1;                       /* 传输完成 */

    int to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
    int r = (int)read(g_sockfd, buf, (size_t)to_read);
    if (r <= 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;                                   /* 可恢复错误 → 稍后重试 */
        return -1;                                      /* 连接错误 */
    }
    int w = write_all(g_dl_fd, buf, (size_t)r);        /* 写入本地文件 */
    if (w < 0) return -1;
    g_dl_received += w;
    usleep(CHUNK_DELAY_US);                              /* 100ms 延迟让进度条可见 */
    update_dl_progress();
    return 0;
}

/* ================================================================== */
/*  上传数据块处理                                                       */
/* ================================================================== */

/**
 * 从本地文件读取一块数据并写入套接字
 *
 * @return   1 = 传输完成
 *           0 = 正常
 *          -1 = 本地文件错误或网络错误
 */
static int handle_upload_chunk(void)
{
    unsigned char buf[CHUNK_SIZE];
    int remaining = g_ul_total - g_ul_sent;
    if (remaining <= 0) return 1;                       /* 传输完成 */

    int to_read = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
    int r = (int)read(g_ul_fd, buf, (size_t)to_read);
    if (r <= 0) return -1;                              /* 本地文件读取错误 */
    int w = write_all(g_sockfd, buf, (size_t)r);
    if (w < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;                                   /* 可恢复错误 → 稍后重试 */
        return -1;                                      /* 网络错误 */
    }
    g_ul_sent += w;
    fprintf(stderr, "[upload chunk] r=%d w=%d sent=%d/%d\n", r, w, g_ul_sent, g_ul_total);
    usleep(CHUNK_DELAY_US);                              /* 100ms 延迟让进度条可见 */
    update_ul_progress();
    return 0;
}

/* ================================================================== */
/*  网络线程主函数                                                       */
/*  这是整个网络模块的核心，运行在独立的 pthread 中                         */
/*                                                                      */
/*  处理流程：                                                           */
/*   1. 解析连接参数（IP/端口/用户名/密码）                                */
/*   2. 创建套接字并连接服务器                                            */
/*   3. 发送 LOGIN 命令并等待认证回复                                     */
/*   4. 进入主循环，按优先级处理：                                        */
/*      - 优先1：ST_DOWNLOADING → 接收文件数据流                          */
/*      - 优先2：ST_UPLOADING   → 发送文件数据流                          */
/*      - 优先3：ST_IDLE        → 启动队列中的下一个传输任务               */
/*      - 优先4：轮询服务器响应 → 解析协议包并分发处理                      */
/*   5. 断开时清理资源并通知 UI                                           */
/* ================================================================== */
void *network_thread_func(void *arg)
{
    char    ip[64]       = {0};
    char    port[16]     = {0};
    char    username[64] = {0};
    char    password[64] = {0};

    /* ---- 步骤1：解析连接参数 ---- */
    if (arg) {
        char **params = (char **)arg;
        if (params[0]) strncpy(ip,       params[0], sizeof(ip) - 1);
        if (params[1]) strncpy(port,     params[1], sizeof(port) - 1);
        if (params[2]) strncpy(username, params[2], sizeof(username) - 1);
        if (params[3]) strncpy(password, params[3], sizeof(password) - 1);
        /* 参数已复制到本地缓冲区，释放原始内存 */
        free(params[0]); free(params[1]); free(params[2]); free(params[3]);
        free(params);
    }

    /* 保存凭据到全局变量（供传输工作线程使用） */
    strncpy(g_login_ip,   ip,       sizeof(g_login_ip) - 1);
    strncpy(g_login_port, port,     sizeof(g_login_port) - 1);
    strncpy(g_login_user, username, sizeof(g_login_user) - 1);
    strncpy(g_login_pass, password, sizeof(g_login_pass) - 1);

    /* ---- 步骤2：创建套接字并连接服务器 ---- */
    g_sockfd = socket(AF_INET, SOCK_STREAM, 0);         /* IPv4 TCP 套接字 */
    if (g_sockfd < 0) {
        lv_async_call(cb_show_error_popup, make_str_data("socket() failed"));
        g_network_running = false;
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)atoi(port));      /* 主机字节序 → 网络字节序 */
    inet_pton(AF_INET, ip, &addr.sin_addr);              /* IP 地址转换 */

    if (connect(g_sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        lv_async_call(cb_show_error_popup, make_str_data("Connect failed"));
        close(g_sockfd);
        g_sockfd = -1;
        g_network_running = false;
        return NULL;
    }

    /* ---- 步骤3：发送 LOGIN 认证命令 ---- */
    /* 登录数据包格式：0xC0 + pkg_len + cmd_no + arg1_len + arg1 + arg2_len + arg2 + 0xC0 */
    int ulen = (int)strlen(username);
    int plen = (int)strlen(password);
    int arg_total = 4 + ulen + 4 + plen;                 /* 用户名长度(4) + 用户名 + 密码长度(4) + 密码 */
    unsigned char *login_arg = (unsigned char *)malloc((size_t)arg_total);
    if (!login_arg) { close(g_sockfd); g_sockfd = -1; g_network_running = false; return NULL; }
    put_le32(login_arg,      ulen);
    memcpy(login_arg + 4, username, (size_t)ulen);
    put_le32(login_arg + 4 + ulen, plen);
    memcpy(login_arg + 4 + ulen + 4, password, (size_t)plen);

    int pkt_len;
    unsigned char *pkt = build_cmd(FTP_CMD_LOGIN, login_arg, arg_total, &pkt_len);
    free(login_arg);
    if (!pkt) { close(g_sockfd); g_sockfd = -1; g_network_running = false; return NULL; }
    write(g_sockfd, pkt, (size_t)pkt_len);               /* 发送登录数据包 */
    free(pkt);

    /* ---- 步骤4：读取 LOGIN 认证响应 ---- */
    int rlen;
    unsigned char *rsp = read_packet(g_sockfd, &rlen);
    if (!rsp) {
        lv_async_call(cb_show_error_popup, make_str_data("No login response"));
        close(g_sockfd); g_sockfd = -1;
        g_network_running = false;
        return NULL;
    }
    resp_t resp;
    parse_response(rsp, rlen, &resp);
    g_login_ok = (resp.cmd_no == FTP_CMD_LOGIN && resp.res_result == 1);
    if (g_login_ok) {
        /* 登录成功：提取服务器返回的会话信息 */
        int info_len = resp.res_len - 1;                 /* 减去 res_result 字节 */
        if (info_len > 0 && info_len < (int)sizeof(g_session_info)) {
            memcpy(g_session_info, resp.res_data, (size_t)info_len);
            g_session_info[info_len] = '\0';
        } else {
            snprintf(g_session_info, sizeof(g_session_info), "SID-%d",
                     (int)(g_sockfd & 0xFFFF));          /* 无会话信息时使用套接字描述符生成 */
        }
        lv_async_call(cb_login_result, make_str_data("Login OK"));
    } else {
        lv_async_call(cb_login_result, make_str_data("Login rejected"));
        close(g_sockfd); g_sockfd = -1;
        g_network_running = false;
        free(rsp);
        return NULL;
    }
    free(rsp);

    /* 初始化传输队列 */
    tx_queue_init(&g_tx_queue);

    /* ================================================================ */
    /*  步骤5：主循环 —— 按优先级处理各种状态                             */
    /* ================================================================ */
    g_state = ST_IDLE;
    while (g_network_running) {

        /* ============================================================ */
        /*  优先1：ST_DOWNLOADING —— 接收原始文件数据流                    */
        /*  在此状态下紧循环读取，直到文件传输完成或出错                     */
        /* ============================================================ */
        if (g_state == ST_DOWNLOADING) {
            if (g_transfer_cancelled) {                  /* 用户取消了传输 */
                finish_download(false);
                g_transfer_cancelled = false;
                continue;
            }
            if (g_dl_received >= g_dl_total) {            /* 已收完所有数据 */
                finish_download(true);
                continue;
            }
            int rc = handle_download_chunk();
            if (rc < 0)  finish_download(false);          /* 下载出错 */
            continue;                                    /* 紧循环：持续读取直到完成/出错 */
        }

        /* ============================================================ */
        /*  优先2：ST_UPLOADING —— 发送原始文件数据流                      */
        /* ============================================================ */
        if (g_state == ST_UPLOADING) {
            if (g_transfer_cancelled) {                  /* 用户取消了传输 */
                finish_upload(false);
                g_transfer_cancelled = false;
                continue;
            }
            if (g_ul_sent >= g_ul_total) {                /* 已发送所有数据 */
                finish_upload(true);
                continue;
            }
            int rc = handle_upload_chunk();
            if (rc < 0)  finish_upload(false);            /* 上传出错 */
            continue;                                    /* 紧循环：持续写入直到完成/出错 */
        }

        /* ============================================================ */
        /*  优先3：ST_IDLE —— 启动队列中等待的传输任务                     */
        /* ============================================================ */
        if (g_state == ST_IDLE) {
            if (g_transfer_cancelled) { g_transfer_cancelled = false; }
            if (g_tx_queue.count > 0 && !g_tx_queue.cancelled) {
                printf("[DEBUG] main loop: starting next queued transfer (queue=%d)\n",
                       g_tx_queue.count);
                batch_start_next();                      /* 启动下一个传输 */
                continue;
            }
            batch_check_complete();                      /* 检查批量传输是否全部完成 */
        }

        /* ============================================================ */
        /*  优先4：轮询服务器响应 —— 在等待状态或空闲时检查是否有数据到达    */
        /* ============================================================ */
        {
            struct pollfd pfd = { .fd = g_sockfd, .events = POLLIN };
            int polltime = (g_state == ST_IDLE && g_tx_queue.count > 0) ? 0 : 500;  /* 有队列任务时立即返回 */
            int pr = poll(&pfd, 1, polltime);
            if (pr < 0) break;                           /* poll 错误 → 退出 */
            if (pr == 0) continue;                       /* 超时 → 继续循环 */

            unsigned char *payload = read_packet(g_sockfd, &rlen);
            if (!payload) break;                         /* 读取失败 → 退出 */

            resp_t rsp2;
            if (!parse_response(payload, rlen, &rsp2)) {  /* 解析响应 */
                free(payload);
                continue;
            }

            /* ---- 根据当前期望状态分发响应 ---- */

            /* 等待 GET 响应时 */
            if (g_state == ST_WAIT_GET_RESP) {
                if (rsp2.cmd_no == FTP_CMD_GET && rsp2.res_result == 1) {
                    /* 服务器确认文件存在，返回文件大小，开始下载 */
                    int filesize = get_le32(rsp2.res_data);
                    start_download(g_dl_filename, filesize);
                } else if (rsp2.cmd_no == FTP_CMD_LS) {
                    /* 下载期间收到了 LS 响应 → 路由到 LS 处理器更新文件列表 */
                    if (rsp2.res_result == 1) {
                        int dlen = rsp2.res_len - 1;
                        char *filelist = (char *)malloc((size_t)(dlen + 1));
                        if (filelist) {
                            memcpy(filelist, rsp2.res_data, (size_t)dlen);
                            filelist[dlen] = '\0';
                            lv_async_call(ui_update_file_list_cb, filelist);
                        }
                    } else {
                        str_data_t *err = make_str_data("Server: dir not found");
                        if (err) lv_async_call(cb_show_error_popup, err);
                        lv_async_call(cb_refresh_remote_list, NULL);
                    }
                    /* 不改变 g_state —— 仍在等待 GET 响应 */
                } else {
                    /* 服务器返回错误（文件不存在等） */
                    str_data_t *err = make_str_data("Server: file not found");
                    if (err) lv_async_call(cb_show_error_popup, err);
                    g_state = ST_IDLE;
                    if (g_batch_active) { g_batch_done++; }
                }
                free(payload);
                continue;
            }

            /* 等待 PUT 响应时 */
            if (g_state == ST_WAIT_PUT_RESP) {
                if (rsp2.cmd_no == FTP_CMD_PUT && rsp2.res_result == 1) {
                    /* 服务器确认可以上传，开始发送文件数据 */
                    if (g_ul_filename[0])
                        start_upload(g_ul_filename, g_ul_local_path);
                    else {
                        str_data_t *err = make_str_data("Upload target missing");
                        if (err) lv_async_call(cb_show_error_popup, err);
                        g_state = ST_IDLE;
                    }
                } else if (rsp2.cmd_no == FTP_CMD_LS) {
                    /* 上传期间收到了 LS 响应 → 路由到 LS 处理器 */
                    if (rsp2.res_result == 1) {
                        int dlen = rsp2.res_len - 1;
                        char *filelist = (char *)malloc((size_t)(dlen + 1));
                        if (filelist) {
                            memcpy(filelist, rsp2.res_data, (size_t)dlen);
                            filelist[dlen] = '\0';
                            lv_async_call(ui_update_file_list_cb, filelist);
                        }
                    }
                    /* 不改变 g_state —— 仍在等待 PUT 响应 */
                } else {
                    str_data_t *err = make_str_data("Server rejected upload");
                    if (err) lv_async_call(cb_show_error_popup, err);
                    g_state = ST_IDLE;
                    if (g_batch_active) { g_batch_done++; }
                }
                free(payload);
                continue;
            }

            /* 等待 LISTDIR 响应时（目录下载，获取目录下所有文件列表） */
            if (g_state == ST_WAIT_LISTDIR_RESP) {
                if (rsp2.cmd_no == 1033 && rsp2.res_result == 1) {
                    /* 解析换行分隔的文件路径列表，将每个文件加入下载队列 */
                    int dlen = rsp2.res_len - 1;
                    char *raw = (char *)malloc((size_t)(dlen + 1));
                    if (raw) {
                        memcpy(raw, rsp2.res_data, (size_t)dlen);
                        raw[dlen] = '\0';
                        char *save;
                        char *token = strtok_r(raw, "\n", &save);
                        int nfiles = 0;
                        while (token) {
                            if (token[0] != '\0') {
                                transfer_task_t t;
                                memset(&t, 0, sizeof(t));
                                strncpy(t.filename, token, sizeof(t.filename) - 1);
                                t.is_upload = false;
                                tx_queue_push(&g_tx_queue, &t);  /* 加入下载队列 */
                                nfiles++;
                            }
                            token = strtok_r(NULL, "\n", &save);
                        }
                        free(raw);
                        /* 修正批量计数：目录本身算 1 个任务，替换为实际文件数 */
                        g_batch_total += nfiles - 1;
                        fprintf(stderr, "[listdir resp] parsed %d files, batch_total=%d queue=%d\n",
                                nfiles, g_batch_total, g_tx_queue.count);
                    }
                    g_state = ST_IDLE;
                } else if (rsp2.cmd_no == FTP_CMD_LS) {
                    /* LISTDIR 期间收到 LS 响应 → 路由到 LS 处理器 */
                    if (rsp2.res_result == 1) {
                        int dlen = rsp2.res_len - 1;
                        char *filelist = (char *)malloc((size_t)(dlen + 1));
                        if (filelist) {
                            memcpy(filelist, rsp2.res_data, (size_t)dlen);
                            filelist[dlen] = '\0';
                            lv_async_call(ui_update_file_list_cb, filelist);
                        }
                    }
                    /* 不改变 g_state */
                } else {
                    str_data_t *err = make_str_data("Server: listdir failed");
                    if (err) lv_async_call(cb_show_error_popup, err);
                    g_state = ST_IDLE;
                }
                free(payload);
                continue;
            }

            /* ---- ST_IDLE 状态：处理异步通知（LS 响应等） ---- */
            switch (rsp2.cmd_no) {
            case FTP_CMD_LS:
                if (rsp2.res_result == 1) {
                    /* LS 成功 → 解析文件列表并更新 UI */
                    int dlen = rsp2.res_len - 1;
                    char *filelist = (char *)malloc((size_t)(dlen + 1));
                    if (filelist) {
                        memcpy(filelist, rsp2.res_data, (size_t)dlen);
                        filelist[dlen] = '\0';
                        lv_async_call(ui_update_file_list_cb, filelist);
                    }
                } else {
                    /* LS 失败（如目录不存在）→ 通知用户并回退路径 */
                    str_data_t *err = make_str_data("Server: dir not found");
                    if (err) lv_async_call(cb_show_error_popup, err);
                    lv_async_call(cb_refresh_remote_list, NULL);
                }
                break;
            case FTP_CMD_BYE:
                /* 服务器确认断开连接 */
                break;
            default:
                break;
            }
            free(payload);
        }
    }

    /* ---- 步骤6：清理资源 ---- */
    tx_queue_cancel_all(&g_tx_queue);                    /* 取消所有传输任务 */
    tx_queue_destroy(&g_tx_queue);                       /* 销毁队列同步原语 */
    if (g_dl_fd >= 0) { close(g_dl_fd); g_dl_fd = -1; }
    if (g_ul_fd >= 0) { close(g_ul_fd); g_ul_fd = -1; }
    if (g_sockfd >= 0) { close(g_sockfd); g_sockfd = -1; }

    /* 重置所有全局状态 */
    g_login_ok         = false;
    g_network_running  = false;
    g_transfer_progress.active = false;
    g_state            = ST_IDLE;

    lv_async_call(cb_disconnected, NULL);                /* 通知 UI 已断开 */
    return NULL;
}

/* ================================================================== */
/*  公开 API 实现（全部由 UI 线程调用）                                  */
/* ================================================================== */

/**
 * 启动网络连接
 * 创建网络线程并传入连接参数（IP/端口/用户名/密码）
 *
 * UI 线程的文本框缓冲区可能在界面切换时被释放，
 * 因此这里使用 strdup 复制参数字符串，由网络线程负责释放。
 */
bool network_start_connect(const char *ip, const char *port,
                           const char *username, const char *password)
{
    if (g_network_running) return false;                 /* 已有网络线程在运行 */

    /* 为网络线程分配参数内存（网络线程会负责释放） */
    char **params = (char **)malloc(4 * sizeof(char *));
    if (!params) return false;
    params[0] = strdup(ip       ? ip       : "");
    params[1] = strdup(port     ? port     : "");
    params[2] = strdup(username ? username : "");
    params[3] = strdup(password ? password : "");

    g_login_ok        = false;
    g_network_running = true;
    g_session_info[0] = '\0';

    if (pthread_create(&g_net_thread, NULL, network_thread_func,
                       (void *)params) != 0) {
        g_network_running = false;
        free(params[0]); free(params[1]); free(params[2]); free(params[3]);
        free(params);
        return false;
    }
    pthread_detach(g_net_thread);                        /* 分离线程，自动回收资源 */
    return true;
}

/**
 * 断开与服务器的连接
 * 取消所有传输，发送 BYE 命令，关闭套接字
 */
void network_disconnect(void)
{
    tx_queue_cancel_all(&g_tx_queue);                    /* 取消所有待处理的传输 */
    g_batch_active = false;

    if (!g_network_running || g_sockfd < 0) return;

    /* 发送 BYE 命令通知服务器 */
    int len;
    unsigned char *pkt = build_cmd(FTP_CMD_BYE, NULL, 0, &len);
    if (pkt) {
        write(g_sockfd, pkt, (size_t)len);
        free(pkt);
    }
    usleep(100000);                                      /* 等待 100ms 确保 BYE 发送完成 */
    g_network_running = false;

    /* 关闭套接字读写，打断网络线程中阻塞的 read() 调用 */
    shutdown(g_sockfd, SHUT_RDWR);
}

/**
 * 发送 LS 命令获取服务器文件列表
 * 如果不指定路径则列出根目录，否则列出指定子目录
 */
bool network_cmd_ls(const char *path)
{
    if (!g_network_running || g_sockfd < 0) return false;
    int len;
    unsigned char *pkt;
    if (!path || path[0] == '\0') {
        /* 无路径参数 → 列出根目录 */
        pkt = build_cmd(FTP_CMD_LS, NULL, 0, &len);
    } else {
        /* 带路径参数 → 列出子目录 */
        pkt = build_cmd_with_str(FTP_CMD_LS, path, &len);
    }
    if (!pkt) return false;
    write(g_sockfd, pkt, (size_t)len);
    free(pkt);
    return true;
}

/* ---- 单文件传输核心函数 ---- */

/**
 * 发送 LISTDIR 命令获取目录内容
 * 这是内部函数，当 GET 的目标是目录时自动调用
 */
static bool network_cmd_listdir(const char *dirname)
{
    if (!g_network_running || g_sockfd < 0 || !dirname) return false;
    /* 去除末尾的 '/'（协议要求） */
    char clean[256];
    strncpy(clean, dirname, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    size_t l = strlen(clean);
    if (l > 0 && clean[l - 1] == '/') clean[l - 1] = '\0';

    int len;
    unsigned char *pkt = build_cmd_with_str(1033, clean, &len);  /* FTP_CMD_LISTDIR = 1033 */
    if (!pkt) return false;
    write(g_sockfd, pkt, (size_t)len);
    free(pkt);
    g_state = ST_WAIT_LISTDIR_RESP;                      /* 进入等待 LISTDIR 响应状态 */
    return true;
}

/**
 * 发送 GET 命令下载单个文件
 * 如果 filename 以 '/' 结尾则视为目录，改用 LISTDIR 协议
 */
bool network_cmd_get(const char *filename)
{
    if (!g_network_running || g_sockfd < 0 || !filename) return false;

    /* 如果文件名以 '/' 结尾，说明是目录 → 使用 LISTDIR 协议 */
    size_t flen = strlen(filename);
    if (flen > 0 && filename[flen - 1] == '/')
        return network_cmd_listdir(filename);

    g_state = ST_WAIT_GET_RESP;                          /* 进入等待 GET 响应状态 */
    g_dl_filename[0] = '\0';
    strncpy(g_dl_filename, filename, sizeof(g_dl_filename) - 1);

    int len;
    unsigned char *pkt = build_cmd_with_str(FTP_CMD_GET, filename, &len);
    if (!pkt) return false;
    write(g_sockfd, pkt, (size_t)len);
    free(pkt);
    return true;
}

/**
 * 发送 PUT 命令上传单个文件
 * 在发送前检查：
 *   1. 本地文件是否存在
 *   2. 是否为普通文件（跳过目录）
 *   3. 服务器端是否已有同名文件（防止重复上传）
 */
bool network_cmd_put(const char *filename, const char *local_path)
{
    if (!g_network_running || g_sockfd < 0 || !filename || !local_path) return false;

    /* 检查本地文件是否存在 */
    struct stat st;
    if (stat(local_path, &st) != 0) {
        return false;                                    /* 文件不存在 */
    }
    /* 只上传普通文件，跳过目录 */
    if (!S_ISREG(st.st_mode)) return false;
    int filesize = (int)st.st_size;

    /* 提取文件名（仅基本名称），本地读取仍使用完整路径 */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;

    /* 检查服务器端是否已有同名文件 */
    if (ui_remote_list_has_entry(base)) {
        str_data_t *err = make_str_data("repeat file");
        if (err) lv_async_call(cb_show_error_popup, err);
        return false;                                    /* 重复文件，拒绝上传 */
    }

    g_ul_filename[0] = '\0';
    strncpy(g_ul_filename, filename, sizeof(g_ul_filename) - 1);
    g_ul_local_path[0] = '\0';
    strncpy(g_ul_local_path, local_path, sizeof(g_ul_local_path) - 1);
    g_state = ST_WAIT_PUT_RESP;                          /* 进入等待 PUT 响应状态 */

    int len;
    unsigned char *pkt = build_cmd_put(filename, filesize, &len);
    if (!pkt) return false;
    if (write_all(g_sockfd, pkt, (size_t)len) < 0) { free(pkt); return false; }
    free(pkt);
    return true;
}

/* ---- 多文件批量传输：将所有文件加入队列，逐个启动 ---- */

/**
 * 批量下载多个文件
 * 将所有选中的文件加入传输队列
 * 如果文件名以 '/' 结尾则视为目录下载
 */
bool network_cmd_get_multi(const char **filenames, int count)
{
    if (!g_network_running || !filenames || count <= 0) return false;
    if (count > MAX_SELECTED_FILES) count = MAX_SELECTED_FILES;

    /* 防止批量传输进行中重复调用 */
    if (g_batch_active && g_tx_queue.count > 0) {
        str_data_t *err = make_str_data("transfer in progress");
        if (err) lv_async_call(cb_show_error_popup, err);
        return false;
    }

    /* 重置取消标志，使队列在之前取消后可重新使用 */
    tx_queue_reset(&g_tx_queue);

    /* 将所有文件加入队列 */
    int actual = 0;
    for (int i = 0; i < count; i++) {
        if (!filenames[i] || strlen(filenames[i]) == 0) continue;

        size_t flen = strlen(filenames[i]);
        /* 目录：检查重复后按原样入队（保留 "/" 后缀） */
        if (flen > 0 && filenames[i][flen - 1] == '/') {
            char base[256];
            folder_basename(filenames[i], base, sizeof(base));
            if (ui_local_list_has_entry(base)) {
                str_data_t *err = make_str_data("Dirent has exist");
                if (err) lv_async_call(cb_show_error_popup, err);
                continue;                                /* 本地已存在同名目录，跳过 */
            }
            transfer_task_t task;
            memset(&task, 0, sizeof(task));
            strncpy(task.filename, filenames[i], sizeof(task.filename) - 1);
            task.is_upload = false;
            if (tx_queue_push(&g_tx_queue, &task))
                actual++;
            continue;
        }

        /* 普通文件 */
        transfer_task_t task;
        memset(&task, 0, sizeof(task));
        strncpy(task.filename, filenames[i], sizeof(task.filename) - 1);
        task.is_upload = false;
        if (tx_queue_push(&g_tx_queue, &task))
            actual++;
    }

    if (actual > 0) {
        g_batch_total  = actual;
        g_batch_done   = 0;
        g_batch_active = true;
        g_batch_is_upload = false;
        printf("[DEBUG] get_multi: enqueued %d files\n", actual);
    }
    return actual > 0;
}

/**
 * 批量上传多个文件
 * 目录会被递归展开为单个文件任务
 * 在入队前校验每个文件的有效性
 */
bool network_cmd_put_multi(const char **filenames, int count)
{
    if (!g_network_running || !filenames || count <= 0) return false;
    if (count > MAX_SELECTED_FILES) count = MAX_SELECTED_FILES;

    /* 防止批量传输进行中重复调用 */
    if (g_batch_active && g_tx_queue.count > 0) {
        str_data_t *err = make_str_data("transfer in progress");
        if (err) lv_async_call(cb_show_error_popup, err);
        return false;
    }

    /* 重置取消标志 */
    tx_queue_reset(&g_tx_queue);

    struct stat st;
    int valid_count = 0;

    /* 验证并加入队列 */
    for (int i = 0; i < count; i++) {
        if (!filenames[i] || strlen(filenames[i]) == 0) continue;

        size_t flen = strlen(filenames[i]);

        /* 目录：递归收集所有文件，然后为每个文件创建独立的 PUT 任务 */
        if (flen > 0 && filenames[i][flen - 1] == '/') {
            char local_base[520];
            normalize_local_path(filenames[i], local_base, sizeof(local_base));
            /* 去掉末尾的 '/' */
            size_t bl = strlen(local_base);
            if (bl > 0 && local_base[bl - 1] == '/') local_base[bl - 1] = '\0';

            char base[256];
            folder_basename(filenames[i], base, sizeof(base));
            if (ui_remote_list_has_entry(base)) {
                str_data_t *err = make_str_data("Dirent has exist");
                if (err) lv_async_call(cb_show_error_popup, err);
                continue;                                /* 远程已存在同名目录，跳过 */
            }

            /* 收集目录下所有文件 */
            transfer_task_t tasks[MAX_SELECTED_FILES];
            int task_count = 0;
            collect_files_recursive(local_base, "", tasks, &task_count, MAX_SELECTED_FILES);

            char sub_prefix[256];
            folder_basename(filenames[i], sub_prefix, sizeof(sub_prefix));
            /* 为每个文件添加目录前缀后入队 */
            for (int j = 0; j < task_count; j++) {
                char full_fn[300];
                snprintf(full_fn, sizeof(full_fn), "%s/%s", sub_prefix, tasks[j].filename);
                transfer_task_t task;
                memset(&task, 0, sizeof(task));
                strncpy(task.filename, full_fn, sizeof(task.filename) - 1);
                snprintf(task.local_path, sizeof(task.local_path), "%s/%s", local_base, tasks[j].filename);
                task.is_upload = true;
                tx_queue_push(&g_tx_queue, &task);
                valid_count++;
            }
            continue;
        }

        /* 普通文件：验证文件存在且为普通文件 */
        char fpath[520];
        normalize_local_path(filenames[i], fpath, sizeof(fpath));
        if (stat(fpath, &st) != 0) {
            str_data_t *err = make_str_data("file unexist");
            if (err) lv_async_call(cb_show_error_popup, err);
            continue;                                    /* 文件不存在，跳过 */
        }
        /* 跳过目录，只上传普通文件 */
        if (!S_ISREG(st.st_mode)) continue;

        /* 检查服务器端是否已有同名文件 */
        {
            const char *base = strrchr(filenames[i], '/');
            base = base ? base + 1 : filenames[i];
            if (ui_remote_list_has_entry(base)) {
                str_data_t *err = make_str_data("repeat file");
                if (err) lv_async_call(cb_show_error_popup, err);
                continue;                                /* 远程已存在，跳过 */
            }
        }

        transfer_task_t task;
        memset(&task, 0, sizeof(task));
        strncpy(task.filename, filenames[i], sizeof(task.filename) - 1);
        snprintf(task.local_path, sizeof(task.local_path), "%s", fpath);
        task.is_upload = true;
        tx_queue_push(&g_tx_queue, &task);
        valid_count++;
    }

    if (valid_count > 0) {
        g_batch_total  = valid_count;
        g_batch_done   = 0;
        g_batch_active = true;
        g_batch_is_upload = true;
        printf("[DEBUG] put_multi: enqueued %d files\n", valid_count);
    }
    return valid_count > 0;
}

/**
 * 取消所有正在进行的传输
 * 设置取消标志，清空传输队列
 */
void network_cancel_transfer(void)
{
    g_transfer_cancelled = true;
    tx_queue_cancel_all(&g_tx_queue);                    /* 清空队列并唤醒等待线程 */
    g_batch_active = false;
    g_active_transfers = 0;
}

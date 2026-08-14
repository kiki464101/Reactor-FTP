/**
 * @file handler.h
 * @brief 服务器工作线程的命令处理器接口定义
 *
 * 这些函数由线程池里的工作线程（worker_func）调用，负责具体执行
 * 客户端发来的每条命令：登录、列目录、下载、上传、断开、递归列目录。
 * 每个处理器完成工作后，会把客户端连接重新注册回 epoll，好让主线程
 * 继续接收下一条命令。
 */
#ifndef SERVER_HANDLER_H
#define SERVER_HANDLER_H

#include "thread_pool.h"
#include "ipc_shm.h"
#include <sys/epoll.h>
#include <netinet/in.h>   /* struct sockaddr_in */

/* 客户端会话 —— 在同一个 fd 的多次任务之间持续存在。
 * 相当于给"每一个连进来的客户端"建立一份档案，记录它的连接信息
 * 和当前的上传状态。 */
typedef struct client_session {
    int                fd;            /* 客户端连接的 socket 文件描述符 */
    struct sockaddr_in addr;          /* 客户端网络地址（IP + 端口） */
    client_info_t     *shm;           /* 指向共享内存，用于上报状态 */
    int                epoll_fd;      /* epoll 实例，工作线程完成后用它重新注册 fd */
    char               ip[32];        /* 客户端 IP 字符串 */
    int                port;          /* 客户端端口 */
    bool               logged_in;     /* 是否已登录 */
    /* ---- 上传状态 ---- */
    bool               uploading;     /* 是否正在上传 */
    int                upload_fd;     /* 上传时打开的目标文件描述符 */
    int                upload_total;  /* 上传文件总大小 */
    int                upload_received; /* 已接收的字节数 */
    char               upload_filename[256]; /* 上传文件名 */
} client_session_t;

/* 工作线程命令处理器（由 handler.c 里的 worker_func 调用） */

void worker_handle_login(client_session_t *sess,
                         const unsigned char *payload, int plen);

void worker_handle_ls(client_session_t *sess,
                     const unsigned char *payload, int plen);

void worker_handle_get(client_session_t *sess,
                       const unsigned char *payload, int plen);

void worker_handle_put(client_session_t *sess,
                       const unsigned char *payload, int plen);

void worker_handle_bye(client_session_t *sess);

void worker_handle_listdir(client_session_t *sess,
                           const unsigned char *payload, int plen);

#endif

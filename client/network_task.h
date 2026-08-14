/**
 * @file network_task.h
 * @brief LVGL FTP 客户端网络任务模块头文件
 *
 * 本模块声明了网络线程相关的所有接口，包括：
 * - 网络线程的启动/停止
 * - 全局套接字状态
 * - 应用层协议命令（LS/GET/PUT/BYE/LOGIN）
 * - 多文件传输队列和线程池
 * - 传输进度状态（线程安全，网络线程写入，UI 线程读取）
 *
 * 架构说明：
 *   网络线程运行在独立的 pthread 中，接收服务器 TCP 数据，
 *   解析应用层协议数据包，通过 lv_async_call() 将结果推送给 UI 线程。
 */

#ifndef NETWORK_TASK_H
#define NETWORK_TASK_H

#include <pthread.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/*  协议命令号定义                                                      */
/*  与服务器端 protocol.h 中的 CMD_NO 枚举保持一致                       */
/* ------------------------------------------------------------------ */
#define FTP_CMD_LOGIN   1028    /* 登录认证命令号 */

/* ------------------------------------------------------------------ */
/*  传输线程池相关常量                                                   */
/* ------------------------------------------------------------------ */
#define TRANSFER_POOL_SIZE  3       /* 并发传输工作线程数               */
#define MAX_QUEUE_SIZE      256     /* 传输任务队列最大容量              */
#define MAX_SELECTED_FILES  128     /* 最大可选择文件数                  */

/* ------------------------------------------------------------------ */
/*  传输任务结构体                                                       */
/*  描述单个待传输的文件（上传或下载）                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    char filename[256];             /* 服务器端路径（含相对路径）         */
    char local_path[520];           /* 本地完整路径                       */
    bool is_upload;                 /* true=上传, false=下载             */
} transfer_task_t;

/* ------------------------------------------------------------------ */
/*  线程安全的传输任务队列                                               */
/*  使用互斥锁 + 条件变量实现生产者-消费者模型                             */
/* ------------------------------------------------------------------ */
typedef struct {
    transfer_task_t tasks[MAX_QUEUE_SIZE];  /* 环形缓冲区存储任务        */
    int             head;                   /* 队列头索引                */
    int             tail;                   /* 队列尾索引                */
    int             count;                  /* 当前队列中的任务数         */
    pthread_mutex_t mutex;                  /* 互斥锁，保护队列操作       */
    pthread_cond_t  cond;                   /* 条件变量，用于阻塞等待     */
    bool            cancelled;              /* 取消标志，通知工作线程退出  */
} transfer_queue_t;

/* ------------------------------------------------------------------ */
/*  传输工作线程状态                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    pthread_t thread;               /* 线程句柄                         */
    int       id;                   /* 工作线程编号（0-based）           */
    bool      running;              /* 线程是否正在运行                  */
} transfer_worker_t;

/* ------------------------------------------------------------------ */
/*  全局状态变量（extern 声明）                                          */
/* ------------------------------------------------------------------ */
extern int          g_sockfd;           /* 与服务器的主 TCP 套接字       */
extern pthread_t    g_net_thread;       /* 网络主线程句柄                 */
extern bool         g_network_running;  /* 网络线程是否运行中            */
extern bool         g_login_ok;        /* 登录是否成功                   */
extern char         g_session_info[64]; /* 会话信息（登录后服务器返回）   */

/* 保存的登录凭据（供传输工作线程重新登录使用） */
extern char g_login_ip[64];             /* 服务器 IP 地址                */
extern char g_login_port[16];           /* 服务器端口号                  */
extern char g_login_user[64];           /* 登录用户名                    */
extern char g_login_pass[64];           /* 登录密码                      */

/* 传输队列和线程池 */
extern transfer_queue_t  g_tx_queue;                    /* 全局传输任务队列 */
extern transfer_worker_t g_tx_workers[TRANSFER_POOL_SIZE]; /* 工作线程池   */
extern volatile int      g_active_transfers;            /* 当前活跃传输数  */

/* ------------------------------------------------------------------ */
/*  传输进度结构体（线程安全：网络线程写入，UI 线程读取）                   */
/* ------------------------------------------------------------------ */
typedef struct {
    int     percent;           /* 当前进度百分比 (0-100)                */
    int     current_bytes;     /* 已传输字节数                          */
    int     total_bytes;       /* 总字节数                              */
    char    filename[256];     /* 正在传输的文件名                       */
    bool    active;            /* 是否有活跃的传输                       */
    bool    is_upload;         /* true=上传, false=下载                 */
} transfer_progress_t;

extern transfer_progress_t g_transfer_progress;  /* 全局传输进度状态    */

/* ================================================================== */
/*  公开 API（全部由 UI 线程调用）                                       */
/* ================================================================== */

/* ---- 连接管理 ---- */

/**
 * 启动网络连接线程
 * @param ip        服务器 IP 地址
 * @param port      服务器端口号
 * @param username  登录用户名
 * @param password  登录密码
 * @return          true=连接线程启动成功, false=失败
 */
bool network_start_connect(const char *ip, const char *port,
                           const char *username, const char *password);

/**
 * 断开与服务器的连接
 * 发送 BYE 命令，关闭套接字，停止网络线程
 */
void network_disconnect(void);

/* ---- 单条命令（主线程调用） ---- */

/**
 * 发送 LS 命令，获取服务器文件列表
 * @param path  要列出的目录路径（NULL 表示根目录）
 * @return      true=命令发送成功, false=失败
 */
bool network_cmd_ls(const char *path);

/* ---- 单文件传输（兼容接口） ---- */

/**
 * 发送 GET 命令下载单个文件
 * 如果 filename 以 '/' 结尾，则视为目录下载（使用 LISTDIR 协议）
 * @param filename  要下载的文件名/目录名
 * @return          true=命令发送成功, false=失败
 */
bool network_cmd_get(const char *filename);

/**
 * 发送 PUT 命令上传单个文件
 * @param filename    要上传的文件名（发送给服务器）
 * @param local_path  本地文件完整路径（用于 stat/open）
 * @return            true=命令发送成功, false=失败
 */
bool network_cmd_put(const char *filename, const char *local_path);

/* ---- 多文件批量传输（将任务加入队列，由线程池处理） ---- */

/**
 * 批量下载多个文件
 * 将所有文件加入传输队列，逐个处理
 * @param filenames  文件名数组
 * @param count      文件数量
 * @return           true=至少有一个文件成功入队, false=全部失败
 */
bool network_cmd_get_multi(const char **filenames, int count);

/**
 * 批量上传多个文件
 * 将文件加入传输队列，目录会被递归展开为单个文件任务
 * @param filenames  文件名数组
 * @param count      文件数量
 * @return           true=至少有一个文件成功入队, false=全部失败
 */
bool network_cmd_put_multi(const char **filenames, int count);

/* ---- 取消所有传输 ---- */

/**
 * 取消当前所有传输任务
 * 清空传输队列，设置取消标志，停止所有正在进行的传输
 */
void network_cancel_transfer(void);

/* ---- 传输线程池生命周期 ---- */
void transfer_pool_init(void);      /* 初始化传输线程池                 */
void transfer_pool_stop(void);      /* 停止并清理传输线程池              */

/* ---- 网络线程入口函数（不要直接调用，由 pthread_create 使用） ---- */
void *network_thread_func(void *arg);

#endif /* NETWORK_TASK_H */

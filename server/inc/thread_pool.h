/**
 * @file thread_pool.h
 * @brief 线程池数据结构和接口定义
 *
 * 【小白速懂：什么是"线程池"？】
 *   线程(thread)是程序里"干活的最小单位"。如果每来一个任务就新建一个
 *   线程、用完就销毁，反复创建/销毁很浪费。线程池的思路是：提前创建
 *   固定数量的线程（比如 8 个），让它们排队领取任务。有任务来就丢进
 *   "任务队列"，空闲的线程会自己取走任务去执行。既快又省资源。
 *
 *  本文件定义了：
 *    1. task_t         —— 一个"任务"长什么样
 *    2. thread_pool_t  —— 线程池本身（线程数组 + 任务队列 + 锁 + 条件变量）
 *    3. 三个接口函数     —— 初始化 / 提交任务 / 销毁
 */
#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include <pthread.h>
#include <stdbool.h>

#define MAX_QUEUE 128   /* 任务队列最大容量 */

/* 任务类型（数字与协议命令号对应，方便映射） */
enum {
    TASK_LOGIN   = 0,   /* 登录认证 */
    TASK_LS      = 1,   /* 列出目录 */
    TASK_GET     = 2,   /* 下载文件 */
    TASK_PUT     = 3,   /* 上传文件 */
    TASK_BYE     = 4,   /* 断开连接 */
    TASK_LISTDIR = 5,   /* 递归列出目录 */
};

/* 一个任务：描述"谁(fd)在什么会话(session)上要做什么(type)" */
typedef struct {
    int    type;           /* 任务类型：TASK_LOGIN / LS / GET / PUT / BYE */
    int    fd;             /* 客户端连接的 socket 文件描述符 */
    void  *session;        /* 指向 client_session_t* 的不透明指针 */
    unsigned char *payload;     /* 命令数据（malloc 分配，由工作线程 free） */
    int    payload_len;    /* 命令数据长度 */
} task_t;

/* 线程池主体 */
typedef struct {
    task_t queue[MAX_QUEUE];   /* 环形任务队列（固定数组实现） */
    int head, tail, count;     /* 队头下标 / 队尾下标 / 当前任务数 */
    pthread_mutex_t mutex;     /* 互斥锁：保护队列的并发访问 */
    pthread_cond_t  cond;      /* 条件变量：线程在此等待新任务 */
    pthread_t threads[8];      /* 工作线程句柄数组（最多 8 个） */
    int num_threads;           /* 实际创建的线程数 */
    bool shutdown;             /* 关闭标志：true 表示线程池要退出 */
} thread_pool_t;

int thread_pool_init(thread_pool_t *pool, int n);        /* 初始化并创建 n 个工作线程 */
void thread_pool_submit(thread_pool_t *pool, task_t *t); /* 提交一个任务到队列 */
void thread_pool_destroy(thread_pool_t *pool);           /* 优雅关闭并销毁线程池 */

/* 工作线程入口函数（定义在 handler.c，由 pthread_create 调用） */
void *worker_func(void *arg);
#endif

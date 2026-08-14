/**
 * thread_pool.c - 线程池实现
 *
 * 本文件实现了一个基于生产者-消费者模型的固定大小线程池。
 *
 * 【架构概述】
 * - 线程池维护一个固定数量的工作线程（最多8个）和一个环形任务队列（容量128）
 * - 主线程（生产者）通过 thread_pool_submit() 将任务放入队列
 * - 工作线程（消费者）在 worker_func() 中循环等待并取出任务执行
 * - 使用互斥锁保护队列的并发访问，使用条件变量实现线程的等待/唤醒
 *
 * 【环形队列（Circular Queue）】
 * - 队列使用数组 pool->queue[MAX_QUEUE] 实现
 * - head: 指向队首（下一个要取出的任务位置）
 * - tail: 指向队尾（下一个要放入的空位）
 * - count: 当前队列中的任务数量
 * - 入队: queue[tail] = task; tail = (tail + 1) % MAX_QUEUE; count++
 * - 出队: task = queue[head]; head = (head + 1) % MAX_QUEUE; count--
 * - 取模运算使索引在数组末尾回绕到开头，实现循环利用
 *
 * 【生产者-消费者同步】
 * - 生产者（提交任务）:
 *   1. 加锁
 *   2. 检查 shutdown 标志和队列是否已满
 *   3. 将任务放入队列尾部
 *   4. 发送条件信号唤醒一个等待的消费者线程
 *   5. 解锁
 *
 * - 消费者（worker_func，定义在 handler.c 中）:
 *   1. 加锁
 *   2. 循环等待条件变量（队列非空 或 shutdown 时被唤醒）
 *   3. 如果 shutdown 且队列为空，退出循环
 *   4. 从队列头部取出任务
 *   5. 解锁
 *   6. 执行任务
 *   7. 回到步骤1
 *
 * 【互斥锁与条件变量】
 * - mutex: 保护所有对线程池结构体（特别是队列）的并发访问
 *          所有对 pool 成员的读写都必须在持有 mutex 的情况下进行
 * - cond:  条件变量，工作线程在队列为空时在此等待
 *          pthread_cond_signal:  唤醒一个等待的线程（提交任务时使用）
 *          pthread_cond_broadcast: 唤醒所有等待的线程（销毁线程池时使用）
 */

#include "thread_pool.h"
#include "handler.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/**
 * thread_pool_init - 初始化线程池
 * @pool: 指向线程池结构体的指针（调用者分配内存）
 * @n:    要创建的工作线程数量
 *
 * 功能:
 * 1. 清零线程池结构体，初始化 head/tail/count 为 0
 * 2. 限制线程数量在合理范围（最小1个，最大8个）
 * 3. 初始化互斥锁和条件变量
 * 4. 创建 n 个工作线程，每个线程执行 worker_func
 *
 * 返回值: 成功返回 0，失败返回 -1
 *
 * 错误处理:
 * - 如果某个线程创建失败，设置 shutdown 标志，广播唤醒所有已创建的线程
 *   然后等待它们全部结束（pthread_join），最后销毁锁和条件变量
 */
int thread_pool_init(thread_pool_t *pool, int n)
{
    /* 参数校验：pool 不能为空指针 */
    if (!pool) return -1;

    /* 将整个线程池结构体清零 —— head/tail/count 自动归零，shutdown 为 false */
    memset(pool, 0, sizeof(*pool));

    /* 限制线程数在合理范围内 */
    if (n <= 0) n = 4;   /* 默认创建 4 个工作线程 */
    if (n > 8)  n = 8;   /* 最多 8 个线程（threads 数组成员只有 8 个槽位） */

    pool->num_threads = n;
    pool->shutdown    = false;

    /* 初始化互斥锁 —— 用于保护线程池结构体（尤其是任务队列）的并发访问 */
    pthread_mutex_init(&pool->mutex, NULL);
    /* 初始化条件变量 —— 工作线程在此等待新任务的到来 */
    pthread_cond_init(&pool->cond, NULL);

    /* 循环创建 n 个工作线程 */
    for (int i = 0; i < n; i++) {
        /*
         * 创建线程，入口函数为 worker_func（定义在 handler.c 中），
         * 将线程池指针 (void *)pool 作为参数传递给工作线程，
         * 这样每个工作线程都能访问同一个线程池的任务队列。
         */
        int rc = pthread_create(&pool->threads[i], NULL,
                                worker_func, (void *)pool);
        if (rc != 0) {
            /* 线程创建失败，打印错误信息到 stderr */
            fprintf(stderr, "thread_pool: pthread_create %d failed (err=%d)\n", i, rc);

            /*
             * 错误恢复流程：
             * 1. 设置 shutdown 标志，通知前面已成功创建的线程退出
             * 2. 广播唤醒所有阻塞在条件变量上的工作线程
             * 3. 等待它们全部结束后，清理同步原语
             */
            pthread_mutex_lock(&pool->mutex);
            pool->shutdown = true;
            /* 广播唤醒所有等待任务的工作线程，使它们检查 shutdown 并退出循环 */
            pthread_cond_broadcast(&pool->cond);
            pthread_mutex_unlock(&pool->mutex);

            /* 等待前面已成功创建的所有线程正常结束（pthread_join 阻塞直到线程退出） */
            for (int j = 0; j < i; j++)
                pthread_join(pool->threads[j], NULL);

            /* 清理同步资源：销毁互斥锁和条件变量 */
            pthread_mutex_destroy(&pool->mutex);
            pthread_cond_destroy(&pool->cond);
            pool->num_threads = 0;
            return -1;
        }
    }
    return 0;
}

/**
 * thread_pool_submit - 向线程池提交一个任务（生产者操作）
 * @pool: 线程池指针
 * @t:    要提交的任务指针
 *
 * 功能:
 * 将任务复制到环形队列的尾部，然后通过条件变量唤醒一个等待的工作线程。
 *
 * 注意:
 * - 任务通过值拷贝存入队列（pool->queue[pool->tail] = *t），
 *   因此调用者可以在函数返回后立即释放 task_t 的内存，不会产生悬空指针
 * - 如果线程池已关闭（shutdown == true）或队列已满（count >= MAX_QUEUE），
 *   任务会被静默丢弃 —— 这是一种背压（back-pressure）策略
 * - 使用 pthread_cond_signal 而非 broadcast：
 *   每次只唤醒一个线程处理一个任务，避免惊群效应（thundering herd）
 */
void thread_pool_submit(thread_pool_t *pool, task_t *t)
{
    /* 参数校验：pool 和 t 都不能为空 */
    if (!pool || !t) return;

    /* 加锁，保护对任务队列的并发写入 —— 多线程环境下的互斥访问 */
    pthread_mutex_lock(&pool->mutex);

    /*
     * 检查两个条件：
     * 1. shutdown: 线程池正在关闭，不再接受新任务
     * 2. count >= MAX_QUEUE: 环形队列已满（MAX_QUEUE = 128），无法容纳更多任务
     * 任一条件成立则丢弃任务并返回
     */
    if (pool->shutdown || pool->count >= MAX_QUEUE) {
        pthread_mutex_unlock(&pool->mutex);
        return;  /* 静默丢弃任务 —— 上层调用者可据此实现重试或限流 */
    }

    /*
     * 环形队列入队操作：
     * 1. 将任务按值拷贝到 tail 指向的空位
     * 2. 尾指针前移，取模 MAX_QUEUE 实现回绕（到达数组末尾后回到0）
     * 3. 队列元素计数加一
     */
    pool->queue[pool->tail] = *t;
    pool->tail = (pool->tail + 1) % MAX_QUEUE;
    pool->count++;

    /*
     * 发送条件信号，唤醒一个正在 cond 上等待的工作线程。
     * 被唤醒的线程会从队列头部取出任务并执行。
     * 使用 signal（唤醒一个）而非 broadcast（唤醒全部），
     * 因为放入的只有一个任务，只需一个线程来处理。
     */
    pthread_cond_signal(&pool->cond);

    pthread_mutex_unlock(&pool->mutex);
}

/**
 * thread_pool_destroy - 销毁线程池，优雅地等待所有工作线程结束
 * @pool: 线程池指针
 *
 * 功能:
 * 1. 设置 shutdown 标志，通知所有工作线程停止等待新任务
 * 2. 广播唤醒所有阻塞在条件变量上的工作线程
 * 3. 等待每个工作线程执行完当前任务后退出（pthread_join）
 * 4. 销毁互斥锁和条件变量，释放系统资源
 *
 * 优雅关闭（Graceful Shutdown）机制:
 * - 工作线程在 worker_func 中检测到 shutdown == true 后，
 *   会处理完队列中剩余的所有任务，然后退出事件循环
 * - pthread_join 确保所有线程完全退出后才返回，
 *   防止线程访问已销毁的锁或条件变量导致的未定义行为
 */
void thread_pool_destroy(thread_pool_t *pool)
{
    /* 参数校验 */
    if (!pool) return;

    /*
     * 第一步：设置 shutdown 标志并通知所有工作线程
     * 加锁是必需的 —— shutdown 的修改需要与工作线程的读取保持同步
     */
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = true;
    /*
     * 使用 broadcast（而非 signal）唤醒所有等待的线程。
     * 因为可能有多个工作线程正在 cond 上阻塞等待任务，
     * 必须全部唤醒，让它们各自检测到 shutdown 后退出。
     */
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    /*
     * 第二步：等待所有工作线程退出
     * pthread_join 会阻塞调用线程，直到目标线程的入口函数（worker_func）返回。
     * 这一步确保所有线程持有的资源（如栈空间、TLS 等）被完全释放。
     */
    for (int i = 0; i < pool->num_threads; i++)
        pthread_join(pool->threads[i], NULL);

    /*
     * 第三步：清理同步原语
     * 在所有线程退出后，安全地销毁互斥锁和条件变量。
     * 销毁顺序无关紧要，因为此时已无其他线程可能访问这些对象。
     */
    pthread_mutex_destroy(&pool->mutex);
    pthread_cond_destroy(&pool->cond);
    pool->num_threads = 0;
}

/*
 * ipc_shm.c — 共享内存IPC模块
 *
 * 本模块使用System V共享内存(shm)在多个进程间共享客户端状态信息。
 * 主要用途：服务器主进程将每个已连接客户端的信息（PID、IP、端口、状态）
 * 写入共享内存表，TUI监控线程/进程可读取此表来实时显示所有客户端状态。
 *
 * 共享内存表结构：一个固定大小的 client_info_t 数组（MAX_CLIENTS个槽位），
 * 每个槽位对应一个可能的客户端连接，通过 active 标志位区分已用/空闲槽位。
 *
 * 提供四个核心接口：
 *   shm_init()          - 创建或附加共享内存段
 *   shm_add_client()    - 添加客户端信息到表中
 *   shm_update_status() - 更新客户端状态
 *   shm_remove_client() - 从表中移除客户端（逻辑删除）
 */
#include "ipc_shm.h"

#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

/* 初始化共享内存段
 * 使用ftok生成键值，尝试创建或附加到共享内存段。
 * 首次调用本函数的进程会创建新段，后续进程则附加到已存在的段上。
 * 返回值：成功返回共享内存映射指针，失败返回NULL。
 * 注意：返回的指针在所有进程中指向同一块物理内存，写入操作对所有进程立即可见 */
client_info_t *shm_init(void)
{
    /* 使用ftok生成System V IPC键值：以/tmp路径为基准，0x77为项目标识符（可任意指定，但所有进程必须一致） */
    key_t key = ftok("/tmp", 0x77);
    if (key < 0) { perror("ftok"); return NULL; }

    /* 尝试以独占方式(IPC_EXCL)创建共享内存段，确保是新创建的 */
    int shmid = shmget(key, sizeof(client_info_t) * MAX_CLIENTS,
                        IPC_CREAT | IPC_EXCL | 0666);
    if (shmid < 0) {
        /* 创建失败说明共享内存段可能已存在（由其他进程先创建），改用普通方式获取已有段的id */
        shmid = shmget(key, sizeof(client_info_t) * MAX_CLIENTS, 0666);
        if (shmid < 0) { perror("shmget"); return NULL; }
    }

    /* 将共享内存段附加(attach)到当前进程的地址空间，NULL表示让系统选择地址 */
    client_info_t *p = (client_info_t *)shmat(shmid, NULL, 0);
    if (p == (void *)-1) { perror("shmat"); return NULL; }

    /* 清零整个共享内存区域（防御性初始化）：
       - 新创建的内存段：Linux内核虽然会清零，但显式memset确保跨平台行为一致；
       - 附加到已有段时：防止读到之前进程遗留的脏数据；
       - 同时将所有client_info_t的active字段设为false，确保槽位状态的正确初始值 */
    memset(p, 0, sizeof(client_info_t) * MAX_CLIENTS);
    return p;
}

/* 向共享内存表中添加一个新客户端
 * 参数：
 *   shm    - 共享内存指针
 *   pid    - 客户端进程ID
 *   ip     - 客户端IP地址
 *   port   - 客户端端口号
 *   status - 客户端初始状态字符串
 * 说明：遍历表中所有槽位（slot），找到第一个未激活的位置填入客户端信息，
 *       如果表已满（MAX_CLIENTS个槽位全部被占用）则输出错误提示 */
void shm_add_client(client_info_t *shm, int pid,
                    const char *ip, int port, const char *status)
{
    if (!shm) return;
    /* 线性扫描查找空闲槽位（active == false 的位置） */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!shm[i].active) {
            shm[i].pid    = pid;
            shm[i].port   = port;
            /* strncpy使用size-1确保预留一个字节给'\0'结尾符，防止越界 */
            strncpy(shm[i].ip,     ip,     sizeof(shm[i].ip) - 1);
            strncpy(shm[i].status, status, sizeof(shm[i].status) - 1);
            shm[i].active = true;  /* 标记该槽位为已占用 */
            return;
        }
    }
    fprintf(stderr, "shm_add_client: table full\n");
}

/* 更新指定客户端的状态字符串
 * 参数：
 *   shm    - 共享内存指针
 *   pid    - 要更新状态的客户端进程ID
 *   status - 新的状态字符串（如 "online", "offline", "transferring" 等）
 * 说明：通过pid定位客户端槽位，更新其状态字段。
 *       如果找不到匹配的活跃客户端，则静默返回（不做任何操作） */
void shm_update_status(client_info_t *shm, int pid, const char *status)
{
    if (!shm) return;
    /* 遍历所有活跃槽位，通过pid匹配定位目标客户端 */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (shm[i].active && shm[i].pid == pid) {
            strncpy(shm[i].status, status, sizeof(shm[i].status) - 1);
            return;
        }
    }
}

/* 从共享内存表中移除（逻辑删除）一个客户端
 * 参数：
 *   shm - 共享内存指针
 *   pid - 要移除的客户端进程ID
 * 说明：不实际清除内存数据，而是将active标记设为false，
 *       这样该槽位就可以被shm_add_client重新使用。
 *       这是一种轻量级的"逻辑删除"策略，避免了memmove等开销 */
void shm_remove_client(client_info_t *shm, int pid)
{
    if (!shm) return;
    /* 通过pid找到对应的活跃槽位，将其标记为非活跃即可释放该槽位 */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (shm[i].active && shm[i].pid == pid) {
            shm[i].active = false;  /* 逻辑删除：仅移除活跃标记，数据可被后续写入覆盖 */
            return;
        }
    }
}

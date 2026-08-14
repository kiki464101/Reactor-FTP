#include "sys_auth.h"

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>   /* 提供 flock 文件锁功能，用于并发读取控制 */

/* 配置文件路径：首先尝试当前目录；如果失败则回退到 ../server/ 目录（适配从 bin/ 子目录运行的情况） */
#define AUTH_FILE_CWD  "./users.conf"
#define AUTH_FILE_ALT  "../server/users.conf"
#define LINE_MAX 256

/**
 * 验证用户凭证
 * @param username 待验证的用户名
 * @param password 待验证的密码
 * @return 0 表示验证成功（用户名和密码匹配），-1 表示验证失败或文件错误
 *
 * 从 users.conf 文件中逐行读取用户信息，使用共享文件锁确保多进程并发安全。
 * 该函数首先尝试从当前目录打开配置，失败后尝试上级目录。
 */
int verify_user(const char *username, const char *password)
{
    /* 两级路径回退机制：先尝试当前目录，再尝试 ../server/ 目录
     * 这样无论程序从哪个目录启动都能找到配置文件 */
    int fd = open(AUTH_FILE_CWD, O_RDONLY);
    if (fd < 0) {
        fd = open(AUTH_FILE_ALT, O_RDONLY);
    }
    if (fd < 0) { perror("open users.conf"); return -1; }

    /* 获取共享锁（LOCK_SH）：允许多个子进程同时读取文件，但不允许写入
     * 这种方式在保证读一致性的同时，避免了不必要的读阻塞 */
    flock(fd, LOCK_SH);

    char line[LINE_MAX];
    int  found = -1;    /* 默认值 -1 表示"未找到/验证失败"，找到匹配后设为 0 */
    /* 逐字符遍历文件内容：每次读取一个字符，手动构建每一行 */
    while (read(fd, line, 1) == 1) {
        /* 逐字符读取一行：先读第一个字符，然后循环读取直到遇到换行符或缓冲区满 */
        int off = 0;
        while (off < LINE_MAX - 1 && line[off] != '\n') {
            if (read(fd, &line[++off], 1) <= 0) break;
        }
        /* 去除行尾的换行符(\n)和回车符(\r)，确保字符串比较时不受行尾字符干扰 */
        while (off >= 0 && (line[off] == '\n' || line[off] == '\r'))
            line[off--] = '\0';
        line[off + 1] = '\0';
        if (off <= 0) continue;           /* 跳过空行（没有任何有效字符的行） */

        /* 将原始行复制到独立缓冲区 saved，避免直接修改 line 缓冲区 */
        char saved[LINE_MAX];
        strncpy(saved, line, LINE_MAX - 1);
        saved[LINE_MAX - 1] = '\0';

        /* 用 strchr 查找冒号分隔符，将一行分为用户名部分和密码部分 */
        char *sep = strchr(saved, ':');
        if (!sep) continue;     /* 无效行：没有冒号分隔符，跳过 */
        *sep = '\0';            /* 在冒号处截断，saved 变为用户名字符串 */

        /* 同时比较用户名和密码，两者都匹配才算验证成功 */

        if (strcmp(saved, username) == 0 && strcmp(sep + 1, password) == 0) {
            found = 0;
            break;
        }
    }

    /* 释放共享锁并关闭文件描述符，无论验证成功与否都要执行 */
    flock(fd, LOCK_UN);
    close(fd);
    return found;   /* 返回验证结果：0 成功，-1 失败 */
}

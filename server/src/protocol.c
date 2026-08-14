#define _GNU_SOURCE
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/*  read_packet – 从文件描述符读取一个完整的数据包                      */
/*  协议格式: 0xC0 + pkg_len(4字节小端) + payload + 0xC0              */
/*  该函数与客户端的解析逻辑完全一致                                    */
/* ------------------------------------------------------------------ */
unsigned char *read_packet(int fd, int *payload_len)
{
    unsigned char ch;
    /* 寻找帧头 0xC0，作为数据包的起始标记 */
    while (1) {
        if (read(fd, &ch, 1) <= 0) return NULL;
        if (ch == 0xC0) break;
    }
    /* 跳过重复的 0xC0 字节（可能是上一个包的尾帧，确保定位到真正的帧头之后的有效数据） */
    while (1) {
        if (read(fd, &ch, 1) <= 0) return NULL;
        if (ch != 0xC0) break;
    }
    /* ch 当前存放的是 pkg_len 的最低字节（小端序，即低地址存低字节） */
    int pkg_len = ch;
    for (int i = 1; i < 4; i++) {
        if (read(fd, &ch, 1) <= 0) return NULL;
        pkg_len |= (ch << (8 * i));
    }
    if (pkg_len < 10) return NULL;  /* 最小有效包长度: 帧头(1) + 长度字段(4) + 预留/命令字(4) + 帧尾(1) = 10字节 */

    int plen = pkg_len - 6;          /* 有效载荷长度 = 包总长 - 帧头(1) - 长度字段(4) - 帧尾(1) */
    if (plen <= 0) return NULL;

    unsigned char *payload = (unsigned char *)malloc((size_t)plen);
    if (!payload) return NULL;

    /* 精确读取 plen 字节的有效载荷，不做 0xC0 扫描。
     * 这样设计是为了支持二进制载荷（如文件内容），
     * 其中可能包含值为 0xC0 的字节，不能将其误判为帧边界。 */
    {
        int total = 0;
        while (total < plen) {
            int r = (int)read(fd, payload + total, (size_t)(plen - total));
            if (r <= 0) { free(payload); return NULL; }
            total += r;
        }
    }
    /* 验证帧尾 0xC0，确保数据包完整且格式正确 */
    if (read(fd, &ch, 1) <= 0 || ch != 0xC0) {
        free(payload);
        return NULL;
    }

    *payload_len = plen;
    return payload;
}

/* ------------------------------------------------------------------ */
/*  write_all – 循环写入直到所有数据写完或出错                          */
/*  处理 write() 可能产生的部分写和 EINTR 中断                          */
/* ------------------------------------------------------------------ */
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

/* ------------------------------------------------------------------ */
/*  send_packet – 构建并发送一个响应数据包                              */
/*  将命令号、结果码和数据组装成协议格式后写入文件描述符                 */
/* ------------------------------------------------------------------ */
int send_packet(int fd, int cmd_no, int res_result,
                const unsigned char *data, int data_len)
{
    /*
     * 线上字节流格式（小端序）:
     *   0xC0 + pkg_len(4字节) + cmd_no(4字节) + res_len(4字节) + res_result(1字节) + data + 0xC0
     * 帧头 0xC0 和帧尾 0xC0 用于界定包的边界
     */
    int res_len = 1 + data_len;       /* 响应数据总长 = 结果码(1字节) + 实际数据长度 */
    int pkg_len = 15 + data_len;      /* 包总长 = 帧头(1) + pkg_len字段(4) + cmd_no(4) + res_len(4) + res_result(1) + data + 帧尾(1) = 15 + data_len */

    unsigned char *pkt = (unsigned char *)malloc((size_t)pkg_len);
    if (!pkt) return -1;

    int i = 0;
    pkt[i++] = 0xC0;
    put_le32(pkt + i, pkg_len); i += 4;
    put_le32(pkt + i, cmd_no);  i += 4;
    put_le32(pkt + i, res_len); i += 4;
    pkt[i++] = (unsigned char)res_result;
    if (data && data_len > 0) {
        memcpy(pkt + i, data, (size_t)data_len);
        i += data_len;
    }
    pkt[i++] = 0xC0;

    int ret = write_all(fd, pkt, (size_t)pkg_len);
    free(pkt);
    return (ret == pkg_len) ? 0 : -1;
}

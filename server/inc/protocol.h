/**
 * @file protocol.h
 * @brief 服务器端通信协议定义 —— 客户端和服务器"怎么对话"的规则说明书
 *
 * =====================================================================
 *  这个头文件定义了网络通信的"协议"，也就是客户端和服务器之间
 *  收发数据的统一格式。它包含三部分：
 *
 *   1. 常量：
 *        SIZE        —— 收发缓冲区大小（4096 字节）
 *        MY_FTP_BOOT —— 服务器对外开放的共享目录（"./copy"）
 *
 *   2. 命令号枚举 cmd_no_t：
 *        客户端发"命令"，服务器执行并"回复"。每个命令有一个编号。
 *
 *   3. 底层收发函数：
 *        read_packet  —— 从网络连接读一个完整的数据包
 *        send_packet  —— 打包并发送一个响应数据包
 *        put_le32 / get_le32 —— 小端序整数与字节之间的转换
 *
 *  【小白速懂：什么叫"协议"？】
 *    两个人打电话约定：先说"你好"（起始标记），再报数字（长度），
 *    再讲内容，最后说"再见"（结束标记）。网络通信也一样，双方必须
 *    用同一套格式，否则一方发的内容另一方看不懂。
 *
 *  【小白速懂：什么叫"小端序(LE, Little Endian)"？】
 *    一个整数在内存里占 4 个字节。小端序就是"低位字节放在前面"。
 *    比如整数 0x01020304，小端序存成字节序列：04 03 02 01。
 *    网络两端必须约定用同一种顺序，否则数字会被读错。
 * =====================================================================
 */

#ifndef SERVER_PROTOCOL_H
#define SERVER_PROTOCOL_H

#define SIZE 4096                /* 收发缓冲区大小（字节） */
#define MY_FTP_BOOT "./copy"     /* 服务器对外共享目录的路径 */

/**
 * 命令号枚举：客户端发请求时携带的命令编号。
 * 服务器收到后，根据 cmd_no 判断客户端想做什么。
 */
typedef enum {
    FTP_CMD_LS       = 1024,   /* 列出目录内容（查看服务器上有什么文件） */
    FTP_CMD_GET      = 1025,   /* 下载文件（从服务器取回本地） */
    FTP_CMD_PUT      = 1026,   /* 上传文件（从本地发到服务器） */
    FTP_CMD_BYE      = 1027,   /* 断开连接 */
    FTP_CMD_LOGIN    = 1028,   /* 登录认证（校验用户名密码） */
    FTP_CMD_CANCEL   = 1029,   /* 取消传输 */
    FTP_CMD_GET_DATA = 1030,   /* 数据连接握手：下载 */
    FTP_CMD_PUT_DATA = 1031,   /* 数据连接握手：上传 */
    FTP_CMD_DONE     = 1032,   /* 传输完成通知（服务器发完/收完文件后告知客户端） */
    FTP_CMD_LISTDIR  = 1033,   /* 递归列出目录下所有文件（下载整个文件夹用） */
} cmd_no_t;

/* 从 fd 读取一个完整数据包。
 * 精确读取帧头(8字节)后按 pkg_len 读载荷，不扫描 0xC0 标记。
 * 返回的缓冲区由调用者负责 free。 */
unsigned char *read_packet(int fd, int *payload_len);

/* 构建并发送一个响应数据包。
 * 参数：fd 目标连接、cmd_no 命令号、res_result 结果码(0失败/1成功)、data 数据。 */
int send_packet(int fd, int cmd_no, int res_result,
                const unsigned char *data, int data_len);

/* 把一个 32 位整数 v 以小端序写入字节数组 b 的前 4 字节 */
static inline void put_le32(unsigned char *b, int v) {
    b[0] = (unsigned char)( v        & 0xFF);
    b[1] = (unsigned char)((v >> 8)  & 0xFF);
    b[2] = (unsigned char)((v >> 16) & 0xFF);
    b[3] = (unsigned char)((v >> 24) & 0xFF);
}
/* 从字节数组 b 的前 4 字节按小端序还原出一个 32 位整数 */
static inline int get_le32(const unsigned char *b) {
    return b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
}

#endif

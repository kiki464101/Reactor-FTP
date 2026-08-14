#ifndef __HANDLER_H__
#define __HANDLER_H__


/*
    Handler:    进行通信,相关请求操作
    @confd:     连接套接字描述符
*/
void Handler(int confd);

/*
    Handler_ls:     处理客户端发来的 LS 请求回复对应文件名
    @confd:     连接套接字描述符
*/
void Handler_ls(int confd);


/*
    Handler_ls: 处理客户端发来的 GET 请求回复对应文件名
    @confd:     连接套接字描述符
    @filename:  待 GET 的文件名
*/
void Handler_get(int confd,char *filename);

/*
    Handler_bye:    接收客户端发来的 BYE
    @confd:         连接套接字描述符
*/
void Handler_bye(int confd);

#endif
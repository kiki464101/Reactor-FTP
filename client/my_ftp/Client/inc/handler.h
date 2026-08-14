#ifndef __HANDLER_H__
#define __HANDLER_H__


/*
    Handler:    进行通信,相关请求操作
    @sockfd:    待通信套接字描述符
*/
void Handler(int sockfd);

/*  
    CMD_ls:     发送 LS 请求并接受回复处理
    @sockfd:    套接字描述符
*/
void CMD_ls(int sockfd);

/*  
    CMD_get:    封装 GET 请求并发送给服务器,接收服务器回复并解析
    @sockfd:    套接字描述符
*/
void CMD_get(int sockfd,char *filename);

/*  
    CMD_bye:    封装 BYE 请求并发送给服务器
    @sockfd:    套接字描述符
*/
void CMD_bye(int sockfd);

#endif
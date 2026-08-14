#ifndef __CLIENT_H__
#define __CLIENT_H__


/*
	tcp_client_init:	TCP client 初始化(创建+复用+绑定+监听)
	返回值:				成功返回监听套接字,失败返回 -1
*/
int tcp_client_init();

/*
	tcp_client_uninit:	TCP client 扫尾
	@sockfd:			待关闭的套接字描述符
*/
void tcp_client_uninit(int sockfd);


/*
	tcp_client_connect:	客户端连接服务器
    @ip:				server IP
	@port:				server port
	@sockfd:			待等待套接字描述符
    retval:             成功返回 0,失败返回 -1
*/
int tcp_client_connect(int sockfd,char *ip,char *port);

#endif
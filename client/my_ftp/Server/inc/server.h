#ifndef __SERVER_H__
#define __SERVER_H__


/*
	tcp_server_init:	TCP Server 初始化(创建+复用+绑定+监听)
	@ip:				Server IP
	@port:				Server port
	返回值:				成功返回监听套接字,失败返回 -1
*/
int tcp_server_init(char *ip,char *port);

/*
	tcp_server_uninit:	TCP Server 扫尾
	@sockfd:			待关闭的套接字描述符
*/
void tcp_server_uninit(int sockfd);


/*
	tcp_server_accept_handler:	等客户端连接并新建线程处理
	@sockfd:			        待等待套接字描述符
*/
void tcp_server_accept_handler(int sockfd);

/*
	handler_client:	处理客户端的连接请求
	@confd:			连接套接字描述符
	@ip:			客户端的 ip
	@port:			客户端的 port
*/
void handler_client(int confd,char *ip,unsigned short port);

#endif
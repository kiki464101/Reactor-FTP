#include <stdio.h>
#include <stdlib.h>				//	atoi 依赖头文件
#include <string.h>				//	memset 依赖头文件
#include <unistd.h>				//	close 依赖头文件
#include <sys/poll.h>           //	poll 依赖头文件
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>			//	IP 地址转换函数 依赖头文件
#include <arpa/inet.h>			//	主机字节序 与 网络字节序 转换函数
#include "client.h"


/*
	tcp_client_init:	TCP client 初始化(创建)
	返回值:				成功返回监听套接字,失败返回 -1
*/
int tcp_client_init()
{
	//	1、创建一个IPv4的流式套接字
	int sockfd = socket(AF_INET,SOCK_STREAM,0);
	if(sockfd == -1)
	{
		perror("socket error");
		return -1;
	}
	return sockfd;
}

/*
	tcp_client_uninit:	TCP client 扫尾
	@sockfd:			待关闭的套接字描述符
*/
void tcp_client_uninit(int sockfd)
{
    close(sockfd);
}

/*
	tcp_client_connect:	客户端连接服务器
    @ip:				server IP
	@port:				server port
	@sockfd:			待连接套接字描述符
    retval:             成功返回 0,失败返回 -1
*/
int tcp_client_connect(int sockfd,char *ip,char *port)
{
    //  连接服务器
	//  1、准备好 Server 套接字地址
	struct sockaddr_in addr;					//	保存 IPv4 套接字地址
	memset(&addr,0,sizeof(addr));				//	将 addr 的sizeof(addr)设置为0
	addr.sin_family = AF_INET;					//	指定协议族
	addr.sin_port = htons(atoi(port));		    //	指定 Server PORT
	addr.sin_addr.s_addr = inet_addr(ip);	    //	指定 Server IP	

	//	2.2	连接
	if(connect(sockfd,(struct sockaddr *)&addr,sizeof(addr)) == -1)
   	{
		perror("connect error");
		return -1;
   	}  
    return 0;
}
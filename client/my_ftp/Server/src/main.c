#include <stdio.h>
#include "server.h"

int main(int argc,char *argv[])
{
	if(argc != 3)
	{
		printf("please input <excute> <ser_ip> <ser_port>\n");
		return -1;
	}
	
	//	1、初始化
	int sockfd = tcp_server_init(argv[1],argv[2]);
	if(sockfd == -1)
	{
		printf("tcp_server_init error\n");
		return -1;
	}

    //  2、等待客户端连接 并创建一个新任务进行处理
    tcp_server_accept_handler(sockfd);
	
   	//	3、关闭套接字
   	tcp_server_uninit(sockfd);
	
	return 0;
}
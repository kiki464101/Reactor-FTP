#include <stdio.h>
#include "client.h"
#include "handler.h"

int main(int argc,char *argv[])
{
	if(argc != 3)
	{
		printf("please input <excute> <ser_ip> <ser_port>\n");
		return -1;
	}
	
	//	1、初始化
	int sockfd = tcp_client_init();
	if(sockfd == -1)
	{
		printf("tcp_client_init error\n");
		return -1;
	}

    //  2、连接服务器
    tcp_client_connect(sockfd,argv[1],argv[2]);
	
	//	3、进行通信,处理相关操作
	Handler(sockfd);

   	//	4、关闭套接字
   	tcp_client_uninit(sockfd);
	return 0;
}
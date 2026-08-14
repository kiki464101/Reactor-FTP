#define _GNU_SOURCE             //  POLLRDHUP 依赖宏定义
#include <stdio.h>
#include <stdlib.h>				//	atoi 依赖头文件
#include <string.h>				//	memset 依赖头文件
#include <unistd.h>				//	close 依赖头文件
#include <sys/poll.h>           //	poll 依赖头文件
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>			//	IP 地址转换函数 依赖头文件
#include <arpa/inet.h>			//	主机字节序 与 网络字节序 转换函数
#include "server.h"
#include "handler.h"

/*
	tcp_server_init:	TCP Server 初始化(创建+复用+绑定+监听)
	@ip:				Server IP
	@port:				Server port
	返回值:				成功返回监听套接字,失败返回 -1
*/
int tcp_server_init(char *ip,char *port)
{
	//	1、创建一个IPv4的流式套接字
	int sockfd = socket(AF_INET,SOCK_STREAM,0);
	if(sockfd == -1)
	{
		perror("socket error");
		return -1;
	}

    //  2、复用 IP+PORT
    int optval = 1;
   	setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&optval,sizeof(optval)); 	//	复用IP
	setsockopt(sockfd,SOL_SOCKET,SO_REUSEPORT,&optval,sizeof(optval)); 	//	复用PORT

	//	3、绑定套接字地址
	//	3.1 准备好套接字地址
	struct sockaddr_in addr;				//	保存 IPv4 套接字地址
	memset(&addr,0,sizeof(addr));			//	将 addr 的sizeof(addr)设置为0
	addr.sin_family = AF_INET;				//	指定协议族
	addr.sin_port = htons(atoi(port));		//	指定 自身 PORT
	addr.sin_addr.s_addr = inet_addr(ip);	//	指定 自身 IP	

	//	3.2 绑定
	if(bind(sockfd,(struct sockaddr *)&addr,sizeof(addr)) == -1)
	{
		perror("bind error");
		return -1;
	}

	//	4、监听套接字
	if(listen(sockfd,5) == -1)
	{
		perror("listen error");
		return -1;
	}
	return sockfd;
}

/*
	tcp_server_uninit:	TCP Server 扫尾
	@sockfd:			待关闭的套接字描述符
*/
void tcp_server_uninit(int sockfd)
{
    close(sockfd);
}

/*
	tcp_server_accept_handler:	等客户端连接并新建线程处理
	@sockfd:			        待等待套接字描述符
*/
void tcp_server_accept_handler(int sockfd)
{
    //	1、等待客户端的连接,进行并发通信
	struct sockaddr_in cli; 			            //	保存请求连接的客户端信息
	socklen_t addrlen = sizeof(cli);
    //  2、加入 poll 多路复用
    struct pollfd fds[1] = {sockfd,POLLIN,0};       //  对套接字描述符进行监听 可读事件
	while(1)
	{
        int r = poll(fds,1,2000);    //  等待 2s
        if(r == -1)
        {
            perror("poll()");
            break;
        }
        else if(r == 0)            //  超时
        {
            //printf("accept timeout\n");
        }
        else                        
        {
            if(fds[0].revents & POLLIN) //  有客户端进行连接
            {
                int confd = accept(sockfd,(struct sockaddr*)&cli,&addrlen);
                printf("%s:%d gets online\n",inet_ntoa(cli.sin_addr),ntohs(cli.sin_port));
                
                //	3、使用 fork 创建父子进程
                if(fork() == 0)		//	子进程用于通信
                {
                    handler_client(confd,inet_ntoa(cli.sin_addr),ntohs(cli.sin_port));
                }
                close(confd);       //  父进程不需要该文件描述符,非阻塞等待子进程结束
                //  自己加上 信号处理
            }
        }
        //	自行完善线程,非阻塞的方式等待新任务死亡
	}	
}


/*
	handler_client:	处理客户端的连接请求
	@confd:			连接套接字描述符
	@ip:			客户端的 ip
	@port:			客户端的 port
*/
void handler_client(int confd,char *ip,unsigned short port)		//	应用层协议
{
	//  1、加入 poll 多路复用
    struct pollfd fds[1] = {confd,POLLIN | POLLRDHUP,0};     //  对连接套接字描述符进行监听 可读事件 和 对方关闭/关闭写
	while(1)
	{
        int r = poll(fds,1,2000);    //  等待 2s
        if(r == -1)
        {
            perror("poll()");
            break;
        }
        else if(r == 0)            //  超时
        {
            //printf("client timeout\n");
        }
        else                                //  confd 已经可读
        {
            if(fds[0].revents & POLLRDHUP)  //  client 断开连接
            {
                printf("%s:%d detch\n",ip,port);
                break;
            }
            else if(fds[0].revents & POLLIN)  //  confd 有数据可读
            {
                Handler(confd);
            }
        }
	}
	shutdown(confd,SHUT_RDWR);
	printf("%s:%d gets offline\n",ip,port);
}
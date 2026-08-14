#include <stdio.h>
#include <stdlib.h>				//	atoi 依赖头文件
#include <string.h>				//	memset 依赖头文件
#include <unistd.h>				//	close 依赖头文件
#include <sys/poll.h>           //	poll 依赖头文件
#include <sys/types.h>          /* See NOTES */
#include <sys/stat.h>           //	open 依赖头文件
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>			//	IP 地址转换函数 依赖头文件
#include <arpa/inet.h>			//	主机字节序 与 网络字节序 转换函数
#include <sys/types.h>
#include "handler.h"
#include "protocol.h"

/*
    Handler:    进行通信,相关请求操作
    @sockfd:    待通信套接字描述符
*/
void Handler(int sockfd)
{
    //  1、等待终端的输入 加入 poll 多路复用
    struct pollfd fds[1] = {STDIN_FILENO,POLLIN,0};     //  对标准输入进行监听 可读事件
	while(1)
	{
        int r = poll(fds,1,2000);    //  等待 2s
        if(r == -1)
        {
            perror("poll()");       //  建议加上 信号处理
            break;
        }
        else if(r == 0)            //  超时
        {
            //printf("client timeout\n");
        }
        else                                
        {
            if(fds[0].revents & POLLIN)     //  标准输入已经输入完
            {
                char buf[30] = {0};         //  保存标准输入缓冲区数据
                fgets(buf,30,stdin);        //  输入可能有空白字符,不适合使用 scanf
                //  根据输入的不同执行对应的操作
                if(strncmp(buf,"ls",2) == 0)        //  ls 请求
                {
                    CMD_ls(sockfd);
                }
                else if(strncmp(buf,"get",3) == 0)  //  get 请求,并获取到待 get 的文件名
                {
                    char filename[30] = {0};        //  保存实际文件名
                    int i = 3;                      //  get 已经占用 3B
                    int j = 0;                      //  文件名长度
                    while(buf[i] == ' ')             //  消除空格
                    {
                        i++;
                    }
                    while(buf[i] != '\n')           //  获取文件名,直到遇到 换行
                    {
                        filename[j++] = buf[i++];
                    }
                    CMD_get(sockfd,filename);
                }
                //  同理
                else if(strncmp(buf,"bye",3) == 0)  //  断开连接
                {
                    CMD_bye(sockfd);
                }
                else
                {
                    printf("重新输入!!!\n");
                }
            }
        }
	}
}

/*  
    CMD_ls:     封装 LS 请求并发送给服务器,接收服务器回复并解析
    @sockfd:    套接字描述符
*/
void CMD_ls(int sockfd)
{
    //  1、封装 LS 请求并发送给服务器
    //	封装数据包
    int pkg_len = 10;				//	数据包长度,包括头尾
	unsigned char cmd[10] = {0};	//	保存具体数据包
	int i = 0;						//	数据包下标
	//	1.1 封装包头 
	cmd[i++] = 0XC0;

	//	1.2 封装 pkg_len 以小端模式保存
	cmd[i++] = pkg_len & 0XFF;			//	低8位数据
	cmd[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	1.3 封装 cmd_no 以小端模式保存
	cmd[i++] = FTP_CMD_LS & 0XFF;			//	低8位数据
	cmd[i++] = (FTP_CMD_LS >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (FTP_CMD_LS >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (FTP_CMD_LS >> 24) & 0XFF;	//	高8位数据
	
    //	1.4 封装包尾
	cmd[i++] = 0XC0;

    //  1.5 发送封装好的数据包
    write(sockfd,cmd,10);

    //  2、接收服务器回复并解析
    /*
        CMD:	ls(FTP_CMD_LS)
        数据包格式:	包头 pkg_len cmd_no res_len res_result res_data 包尾
        数据包大小:	 1	   4	  4		  4			1		 不定长   1
    */
    //	2.1 先找数据包的包头	0XC0
    unsigned char ch;
    do
    {
        if(read(sockfd,&ch,1) == -1)
        {
            printf("有问题\n");
            return;
        }
    }while(ch != 0XC0);		//	ch == 0XC0
    //	上述 do-while 结束;ch == 0XC0 可能是上一数据包的包尾
    while(ch == 0XC0)
    {
        if(read(sockfd,&ch,1) == -1)
        {
            printf("有问题\n");
            return;
        } 
    }
	//	while 结束;ch != 0XC0,理论上是数据包的 第二字节
	//	ch 保存 pkg_len 的第一个字节
	//	此时有两种做法:
	//		①直接一次全部读取完毕,直到读取到包尾的 0XC0(如果数据中有 0XC0 崩掉)
	//		②先读取包长,再根据包长;读取剩余内容	使用的!!!
	//	2.2 解析 pkg_len
	i = 0;
	pkg_len = ch;		//	数据包格式为 小端模式
	while(i++ < 3)
    {
    	read(sockfd,&ch,1);
        pkg_len = ((ch << (8*i)) | pkg_len);	//	得到数据包长度
    }
	printf("pkg_len = %d\n",pkg_len);
	//	2.3 一次性读取剩余内容(不读取包尾,已经读取了包头和包长)
	i = 0;			//	标识读取长度
	unsigned char res[pkg_len-6];
	while(1)
    {
       	read(sockfd,&ch,1);
        if(ch == 0XC0)//	读取到包尾
        {
            break;
        }
     	res[i++] = ch;		//	读取到的内容保存到数组中
    }
	//	2.4 验证数据是否正确
	if(pkg_len-6 != i)		//	中间数据出现了 0XC0
    {
       	printf("有问题\n");
        return;
    }
	//	2.5 根据对应的命令执行对应操作
	//	先获取数据包的命令号
	CMD_NO cmd_no = res[0] | res[1] << 8 | res[2] << 16 | res[3] << 24;
	if(cmd_no == FTP_CMD_LS && res[8] == 1) //  验证服务器操作结果是否正确
    {
        printf("%s\n",res+9);
    }
}

/*  
    CMD_get:    封装 GET 请求并发送给服务器,接收服务器回复并解析
    @sockfd:    套接字描述符
*/
void CMD_get(int sockfd,char *filename)
{
    /*
    CMD:	get filename(FTP_CMD_GET)
	数据包格式:	包头 pkg_len cmd_no arg_len arg_data 包尾
	数据包大小:	 1	   4	  4		 4		arg_len  1  
    */
    //  1、封装 LS 请求并发送给服务器
    //	封装数据包
    int arg_len = strlen(filename);
    int pkg_len = 14+arg_len;		//	数据包长度,包括头尾
	unsigned char cmd[pkg_len];		//	保存具体数据包
	int i = 0;						//	数据包下标
	//	1.1 封装包头 
	cmd[i++] = 0XC0;

	//	1.2 封装 pkg_len 以小端模式保存
	cmd[i++] = pkg_len & 0XFF;			//	低8位数据
	cmd[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	1.3 封装 cmd_no 以小端模式保存
	cmd[i++] = FTP_CMD_GET & 0XFF;			//	低8位数据
	cmd[i++] = (FTP_CMD_GET >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (FTP_CMD_GET >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (FTP_CMD_GET >> 24) & 0XFF;	//	高8位数据
	
	//	1.4 封装参数
	//	1.4.1 封装参数内容长度
	cmd[i++] = arg_len & 0XFF;			//	低8位数据
	cmd[i++] = (arg_len >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (arg_len >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (arg_len >> 24) & 0XFF;	//	高8位数据
	//	1.4.2 封装参数实际内容
	strncpy(cmd+i,filename,arg_len);
	i += arg_len;
	//	1.5 封装包尾
	cmd[i++] = 0XC0;

    //  1.6 发送封装好的数据包
    write(sockfd,cmd,pkg_len);

    //  2、接收服务器回复并解析
    /*
        CMD:	get		回复文件的大小(4B),后续不再使用该格式发送,单纯发送文件内容
        数据包格式:	包头 pkg_len cmd_no res_len res_result res_data 包尾
        数据包大小:	 1	   4	  4		  4			1		 4   	1
    */
    //	2.1 先找数据包的包头	0XC0
    unsigned char ch;
    do
    {
        if(read(sockfd,&ch,1) == -1)
        {
            printf("有问题\n");
            return;
        }
    }while(ch != 0XC0);		//	ch == 0XC0
    //	上述 do-while 结束;ch == 0XC0 可能是上一数据包的包尾
    while(ch == 0XC0)
    {
        if(read(sockfd,&ch,1) == -1)
        {
            printf("有问题\n");
            return;
        } 
    }
	//	while 结束;ch != 0XC0,理论上是数据包的 第二字节
	//	ch 保存 pkg_len 的第一个字节
	//	此时有两种做法:
	//		①直接一次全部读取完毕,直到读取到包尾的 0XC0(如果数据中有 0XC0 崩掉)
	//		②先读取包长,再根据包长;读取剩余内容	使用的!!!
	//	2.2 解析 pkg_len
	i = 0;
	pkg_len = ch;		//	数据包格式为 小端模式
	while(i++ < 3)
    {
    	read(sockfd,&ch,1);
        pkg_len = ((ch << (8*i)) | pkg_len);	//	得到数据包长度
    }
	printf("pkg_len = %d\n",pkg_len);
	//	2.3 一次性读取剩余内容(不读取包尾,已经读取了包头和包长)
	i = 0;			//	标识读取长度
	unsigned char res[pkg_len-6];
	while(1)
    {
       	read(sockfd,&ch,1);
        if(ch == 0XC0)//	读取到包尾
        {
            break;
        }
     	res[i++] = ch;		//	读取到的内容保存到数组中
    }
	//	2.4 验证数据是否正确
	if(pkg_len-6 != i)		//	中间数据出现了 0XC0
    {
       	printf("有问题\n");
        return;
    }
	//	2.5 根据服务器回复操作是否成功决定是否继续
	//	先获取数据包的命令号
	CMD_NO cmd_no = res[0] | res[1] << 8 | res[2] << 16 | res[3] << 24;
    if(cmd_no != FTP_CMD_GET || res[8] != 1)    //  操作结果已是错误
    {
        printf("NO Found File\n");
        return;
    }
    //  2.6 确认无误先获取文件大小
    int filesize = res[9] | res[10] << 8 | res[11] << 16 | res[12] << 24;
    printf("filesize = %d\n",filesize);

    //  2.7 创建本地文件
    int fd = open(filename,O_RDWR|O_CREAT|O_TRUNC,0777);    //  不存在就创建,存在就清空
    //  判断!!!
    //  2.8 接收
    unsigned char buf[SIZE] = {0};      //  保存获取到的数据
    int recv_sum = 0;                   //  接收到的字节数目
    while(recv_sum < filesize)
    {
        int r = read(sockfd,buf,SIZE);
        if(r > 0)
        {
            recv_sum += write(fd,buf,r); 
        }
    }
    //  2.9 关闭文件
    close(fd);
    printf("CP Over\n");
}


/*  
    CMD_bye:    封装 BYE 请求并发送给服务器
    @sockfd:    套接字描述符
*/
void CMD_bye(int sockfd)
{
    /*
        CMD:	ls(FTP_CMD_BYE)
        数据包格式:	包头 pkg_len cmd_no 包尾
        数据包大小:	 1	   4	  4		1  
        */
    //  1、封装 LS 请求并发送给服务器
    //	封装数据包
    int pkg_len = 10;				//	数据包长度,包括头尾
	unsigned char cmd[10] = {0};	//	保存具体数据包
	int i = 0;						//	数据包下标
	//	1.1 封装包头 
	cmd[i++] = 0XC0;

	//	1.2 封装 pkg_len 以小端模式保存
	cmd[i++] = pkg_len & 0XFF;			//	低8位数据
	cmd[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	1.3 封装 cmd_no 以小端模式保存
	cmd[i++] = FTP_CMD_BYE & 0XFF;			//	低8位数据
	cmd[i++] = (FTP_CMD_BYE >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (FTP_CMD_BYE >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (FTP_CMD_BYE >> 24) & 0XFF;	//	高8位数据
	
    //	1.4 封装包尾
	cmd[i++] = 0XC0;

    //  1.5 发送封装好的数据包
    write(sockfd,cmd,10);

    sleep(2);
    close(sockfd);      //  等待 2s 后关闭套接字
    exit(0);            
}
	
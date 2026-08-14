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
#include <dirent.h>         //  目录操作 依赖头文件
#include "handler.h"
#include "protocol.h"

/*
    Handler:    进行通信,相关请求操作
    @confd:     连接套接字描述符
*/
void Handler(int confd)
{
    //  1、接收客户端的请求并解析
    /*
        请求数据包格式:
        组成	包头 pkg_len cmd_no arg1 arg2 ... 包尾
        大小	 1	   4	  4		4+n	 4+n ...   1
    */
    //	1.1 先找数据包的包头	0XC0
    unsigned char ch;
    do
    {
        if(read(confd,&ch,1) == -1)
        {
            printf("有问题\n");
            return;
        }
    }while(ch != 0XC0);		//	ch == 0XC0
    //	上述 do-while 结束;ch == 0XC0 可能是上一数据包的包尾
    while(ch == 0XC0)
    {
        if(read(confd,&ch,1) == -1)
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
	//	1.2 解析 pkg_len
	int i = 0;
	int pkg_len = ch;		//	数据包格式为 小端模式
	while(i++ < 3)
    {
    	read(confd,&ch,1);
        pkg_len = ((ch << (8*i)) | pkg_len);	//	得到数据包长度
    }
	printf("pkg_len = %d\n",pkg_len);
	//	1.3 一次性读取剩余内容(不读取包尾,已经读取了包头和包长)
	i = 0;			//	标识读取长度
	unsigned char res[pkg_len-6];
	while(1)
    {
       	read(confd,&ch,1);
        if(ch == 0XC0)//	读取到包尾
        {
            break;
        }
     	res[i++] = ch;		//	读取到的内容保存到数组中
    }
	//	1.4 验证数据是否正确
	if(pkg_len-6 != i)		//	中间数据出现了 0XC0
    {
       	printf("有问题\n");
        return;
    }
	//	2.5 根据对应的命令执行对应操作
	//	先获取数据包的命令号
	CMD_NO cmd_no = res[0] | res[1] << 8 | res[2] << 16 | res[3] << 24;
	switch (cmd_no)
    {
        case FTP_CMD_LS:	Handler_ls(confd);      break;  //	获取服务器信息
        case FTP_CMD_GET:
                        {
                            //  获取文件名长度
                            int arg_len = res[4] | res[5] << 8 | res[6] << 16 | res[7] << 24;     	
                            //  获取文件名
                            unsigned char filename[arg_len+1];      //  '\0'
                            strncpy(filename,res+8,arg_len);
                            filename[arg_len] = '\0';
                            Handler_get(confd,filename);	
                        }    
                    break;  //	下载文件
        case FTP_CMD_PUT:		break;  //	上传文件
        //  ...
        case FTP_CMD_BYE:	Handler_bye(confd); 	break;  //	断开连接
    }
}

/*
    Handler_ls:     处理客户端发来的 LS 请求回复对应文件名
    @confd:     连接套接字描述符
*/
void Handler_ls(int confd)
{  
    int res_result = 1;         //  默认成功
   	//	1、利用 文件IO 知识将服务器的指定的网络共享目录下所有文件名都保存
    unsigned char filename[SIZE] = {0};
	//  1.1 打开目录
    DIR *dir = opendir(MY_FTP_BOOT);    //  打开 指定目录
    if(dir == NULL)                     //  打开错误
    {
        perror("opendir error");        //  解析错误
        res_result = 0;
    }
    //  1.2 读取目录项
    struct dirent *p = NULL;        //  指向读取到的目录项
    int r = 0;                      //  读取到名字的总长度,换行进行间隔
    while(p = readdir(dir))         //  将名字保存
    {
        r += snprintf(filename+r,SIZE-1-r,"%s\n",p->d_name);
        //  filename+r         获取到文件名应该放到 file_name 数组哪个位置
        //  SIZE-1-r            最多写入的字节数目,留个位置给 '\0';防止数组撑爆
        //  "%s\n",p->d_name    写入文件名,后面间隔一个 \n
        //  r+=                 返回值是实际写入的字节数目
    }
    filename[r] = '\0';             //  最后加一个 \0
    r++;
    //  1.3 关闭目录
    closedir(dir);
    //  2、封装回复数据包
    /*
    CMD:	ls(FTP_CMD_LS)
	数据包格式:	包头 pkg_len cmd_no res_len res_result res_data 包尾
	数据包大小:	 1	   4	  4		  4			1		 不定长   1
    */          
    int res_len = 1 + 4;				//	默认认为错误
    if(res_result == 1)
    {
        res_len = 1 + strlen(filename);	//	没错
    }
    int pkg_len = 14+res_len;			//	数据包长度,包括头尾
	unsigned char res[pkg_len];			//	保存具体数据包
	int i = 0;							//	数据包下标
	//	2.1 封装包头 
	res[i++] = 0XC0;

	//	2.2 封装 pkg_len 以小端模式保存
	res[i++] = pkg_len & 0XFF;			//	低8位数据
	res[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	2.3 封装 cmd_no 以小端模式保存
	res[i++] = FTP_CMD_LS & 0XFF;			//	低8位数据
	res[i++] = (FTP_CMD_LS >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (FTP_CMD_LS >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (FTP_CMD_LS >> 24) & 0XFF;	//	高8位数据
	
	//	2.4 封装 res_len 以小端模式保存
	res[i++] = res_len & 0XFF;			//	低8位数据
	res[i++] = (res_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (res_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (res_len >> 24) & 0XFF;	//	高8位数据
	//	2.5 封装 res_result
	res[i++] = res_result;
	//	2.6 封装 res_data
	if(res_result == 0)		//	错误,封装错误码
    {
     	// res[i++] = 错误码 & 0XFF;			//	低8位数据
        // res[i++] = (错误码 >> 8) & 0XFF;	//	次低8位数据
        // res[i++] = (错误码 >> 16) & 0XFF;	//	次高8位数据
        // res[i++] = (错误码 >> 24) & 0XFF;	//	高8位  
    }
	else		//	没错,封装所有的文件名
    {
        strncpy(res+i,filename,res_len-1);
		i += res_len-1;
    }

	//	2.7 封装包尾
	res[i++] = 0XC0;   
    
    //  2、将封装好的数据包回复给客户端
    write(confd,res,i);
}


/*
    Handler_get:    处理客户端发来的 GET 请求回复对应文件名
    @confd:         连接套接字描述符
    @filename:      待 GET 的文件名
*/
void Handler_get(int confd,char *filename)
{  
    int res_result = 1;         //  默认成功
   	//	1、利用 文件IO 知识判断对应文件是否存在;文件大小(存在)/错误码(不存在)
    unsigned char pathname[128] = {0};      //  保存绝对路径名
    sprintf(pathname,"%s/%s",MY_FTP_BOOT,filename);     
    struct stat st;             //  保存文件属性
    int flag = stat(pathname,&st);
    if(flag == -1)
    {
        perror("stat error");
        res_result = 0;         //  操作失败
    }
    
    //  2、封装回复数据包
    /*
    CMD:	get		回复文件的大小(4B),后续不再使用该格式发送,单纯发送文件内容
	数据包格式:	包头 pkg_len cmd_no res_len res_result res_data 包尾
	数据包大小:	 1	   4	  4		  4			1		 4   	1
    */          
    int res_len = 5;				    //	成功/失败 都是 5B
    int pkg_len = 14+res_len;			//	数据包长度,包括头尾
	unsigned char res[pkg_len];			//	保存具体数据包
	int i = 0;							//	数据包下标
	//	2.1 封装包头 
	res[i++] = 0XC0;

	//	2.2 封装 pkg_len 以小端模式保存
	res[i++] = pkg_len & 0XFF;			//	低8位数据
	res[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	2.3 封装 cmd_no 以小端模式保存
	res[i++] = FTP_CMD_GET & 0XFF;			//	低8位数据
	res[i++] = (FTP_CMD_GET >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (FTP_CMD_GET >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (FTP_CMD_GET >> 24) & 0XFF;	//	高8位数据
	
	//	2.4 封装 res_len 以小端模式保存
	res[i++] = res_len & 0XFF;			//	低8位数据
	res[i++] = (res_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (res_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (res_len >> 24) & 0XFF;	//	高8位数据
	//	2.5 封装 res_result
	res[i++] = res_result;
	//	2.6 封装 res_data
	if(res_result == 0)		//	错误,封装错误码
    {
     	// res[i++] = 错误码 & 0XFF;			//	低8位数据
        // res[i++] = (错误码 >> 8) & 0XFF;	//	次低8位数据
        // res[i++] = (错误码 >> 16) & 0XFF;	//	次高8位数据
        // res[i++] = (错误码 >> 24) & 0XFF;	//	高8位  
    }
	else		//	没错,封装所有的文件名
    {
        res[i++] = st.st_size & 0XFF;			//	低8位数据
        res[i++] = (st.st_size >> 8) & 0XFF;	//	次低8位数据
        res[i++] = (st.st_size >> 16) & 0XFF;	//	次高8位数据
        res[i++] = (st.st_size >> 24) & 0XFF;	//	高8位  
    }

	//	2.7 封装包尾
	res[i++] = 0XC0;   
    
    //  3、将封装好的数据包回复给客户端
    write(confd,res,i);

    //  4、如果文件存在,单纯发数据内容
    if(res_result)
    {
        //  4.1 打开网络本地文件
        int fd = open(pathname,O_RDWR);     //  只读打开网络共享目录下对应文件
        //  判断!!!
        //  4.2 发送文件内容给客户端
        unsigned char buf[SIZE] = {0};      //  保存获取到的数据
        int recv_sum = 0;                   //  接收到的字节数目
        while(recv_sum < st.st_size)
        {
            int r = read(fd,buf,SIZE);
            if(r > 0)
            {
                recv_sum += write(confd,buf,r); 
            }
        }
        //  4.3 关闭文件
        close(fd);
        printf("Get Over\n");
    }
}


/*
    Handler_bye:    接收客户端发来的 BYE
    @confd:         连接套接字描述符
*/
void Handler_bye(int confd)
{
    close(confd);
    exit(0);
}
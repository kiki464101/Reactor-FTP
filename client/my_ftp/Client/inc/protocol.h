#ifndef __PROCOTOL_H__
#define __PROCOTOL_H__
//  应用层协议共用的东西
#define SIZE 4096
#define MY_FTP_BOOT "/home/china/my_boot"	//	服务器的文件目录

typedef enum
{
    FTP_CMD_LS = 1024,	//	获取服务器信息
    FTP_CMD_GET,		//	下载文件
    FTP_CMD_PUT,		//	上传文件
    FTP_CMD_BYE			//	断开连接
    //	按需增加...
}CMD_NO;

//  错误码 等信息
/*
请求数据包格式:
	组成	包头 pkg_len cmd_no arg1 arg2 ... 包尾
  	大小	 1	   4	  4		4+n	 4+n ...   1
解析:
	包头 		   //  1byte  数据包中的第一个字节,每个数据包都是以 0XC0 开头
             	//	实际开发过程中,为了保证包头唯一性,通常设置多个字节为包头
        
    pkg_len		//	4bytes	保存数据包的总长度,以小端模式保存;包括头和尾
        
    cmd_no 		//	4bytes	保存数据包的命令号(要做什么),以小端模式保存
    
    arg1 		//	4+n bytes	命令执行所需要的参数,由两部分组成 参数内容长度+参数实际内容
        		//	arg_len		4bytes			参数长度,以小端模式保存
        		//	arg_data	arg_len bytes	参数内容,字符保存
        
    arg2 		//	同上
        
    ... 
        
    包尾		   //  1byte  数据包中的第一个字节,每个数据包都是以 0XC0 结尾
             	//	实际开发过程中,为了保证包头唯一性,通常设置多个字节为包头
一般来说:
  	FTP_CMD_LS FTP_CMD_BYE 不需要参数
    FTP_CMD_GET FTP_CMD_PUT 需要参数(文件名...)
举个例子:
	CMD:	ls(FTP_CMD_LS)
	数据包格式:	包头 pkg_len cmd_no 包尾
	数据包大小:	 1	   4	  4		1  
        
    //	封装数据包
    int pkg_len = 10;				//	数据包长度,包括头尾
	unsigned char cmd[10] = {0};	//	保存具体数据包
	int i = 0;						//	数据包下标
	//	1 封装包头 
	cmd[i++] = 0XC0;

	//	2 封装 pkg_len 以小端模式保存
	cmd[i++] = pkg_len & 0XFF;			//	低8位数据
	cmd[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	3 封装 cmd_no 以小端模式保存
	cmd[i++] = FTP_CMD_LS & 0XFF;			//	低8位数据
	cmd[i++] = (FTP_CMD_LS >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (FTP_CMD_LS >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (FTP_CMD_LS >> 24) & 0XFF;	//	高8位数据
	//	4 封装包尾
	cmd[i++] = 0XC0;

	为了避免实际数据中有 0XC0,可以在发送时将数据包中的:
		0XC0	--->	0XDD 0XDB
        0XDD	--->	0XDD 0XDC  
   	接收时:
		0XDD 0XDB	--->	0XC0	
        0XDD 0XDC 	--->	0XDD
            
再举个例子:
	CMD:	get filename(FTP_CMD_GET)
	数据包格式:	包头 pkg_len cmd_no arg_len arg_data 包尾
	数据包大小:	 1	   4	  4		 4		arg_len  1  
        
    //	封装数据包
    int arg_len = strlen(filename);
    int pkg_len = 14+arg_len;		//	数据包长度,包括头尾
	unsigned char cmd[pkg_len];		//	保存具体数据包
	int i = 0;						//	数据包下标
	//	1 封装包头 
	cmd[i++] = 0XC0;

	//	2 封装 pkg_len 以小端模式保存
	cmd[i++] = pkg_len & 0XFF;			//	低8位数据
	cmd[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	3 封装 cmd_no 以小端模式保存
	cmd[i++] = FTP_CMD_GET & 0XFF;			//	低8位数据
	cmd[i++] = (FTP_CMD_GET >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (FTP_CMD_GET >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (FTP_CMD_GET >> 24) & 0XFF;	//	高8位数据
	
	//	4 封装参数
	//	4.1 封装参数内容长度
	cmd[i++] = arg_len & 0XFF;			//	低8位数据
	cmd[i++] = (arg_len >> 8) & 0XFF;	//	次低8位数据
    cmd[i++] = (arg_len >> 16) & 0XFF;	//	次高8位数据
    cmd[i++] = (arg_len >> 24) & 0XFF;	//	高8位数据
	//	4.2 封装参数实际内容
	strncpy(cmd+i,filename,arg_len);
	i += arg_len;
	//	5 封装包尾
	cmd[i++] = 0XC0;
    其它同理!!!
*/

/*
回复数据包格式:
	组成	包头 pkg_len cmd_no res_len res_result res_data 包尾
  	大小	 1	   4	  4		   4		1		不定长	   1
解析:
	包头 		   //  1byte  数据包中的第一个字节,每个数据包都是以 0XC0 开头
             	//	实际开发过程中,为了保证包头唯一性,通常设置多个字节为包头
        
    pkg_len		//	4bytes	保存数据包的总长度,以小端模式保存;包括头和尾
        
    cmd_no 		//	4bytes	保存数据包的命令号(和请求包一致),以小端模式保存
    
    res_len 	//	4bytes	回复数据内容的长度 = 回复结果长度 + 回复内容长度
    
   	res_result	//	1byte	表示收到请求后操作的结果,0 代表失败,1 代表成功
        		
    res_data	//	不定长,需要根据 cmd_no 和 res_result 决定
        		
        			成功:		不同的命令回复内容也不一样
        				LS	所有文件的文件名(长度未知),各个文件名之间需要间隔
        				GET	回复文件的大小(4B),后续不再使用该格式发送,单纯发送文件内容
        				...
        			失败:		借鉴 errno
        				错误码(4B),自定义错误信息(枚举类型)
        		
        
    包尾		   //  1byte  数据包中的第一个字节,每个数据包都是以 0XC0 结尾
             	//	实际开发过程中,为了保证包头唯一性,通常设置多个字节为包头
举个例子:
	CMD:	ls(FTP_CMD_LS)
	数据包格式:	包头 pkg_len cmd_no res_len res_result res_data 包尾
	数据包大小:	 1	   4	  4		  4			1		 不定长   1
        
   	//	利用 文件IO 知识将服务器的指定的网络共享目录下所有文件名都保存
    unsigned char filename[4096] = {0};
	... 读取目录项
    如果错误 res_result = 0
    如果成功 res_result = 1
        
    //	封装数据包
    int res_len = 1 + 4;				//	默认认为错误
    if(res_result == 1)
    {
        res_len = 1 + strlen(filename);	//	没错
    }
    int pkg_len = 14+res_len;			//	数据包长度,包括头尾
	unsigned char res[pkg_len];			//	保存具体数据包
	int i = 0;							//	数据包下标
	//	1 封装包头 
	res[i++] = 0XC0;

	//	2 封装 pkg_len 以小端模式保存
	res[i++] = pkg_len & 0XFF;			//	低8位数据
	res[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	3 封装 cmd_no 以小端模式保存
	res[i++] = FTP_CMD_LS & 0XFF;			//	低8位数据
	res[i++] = (FTP_CMD_LS >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (FTP_CMD_LS >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (FTP_CMD_LS >> 24) & 0XFF;	//	高8位数据
	
	//	4 封装 res_len 以小端模式保存
	res[i++] = res_len & 0XFF;			//	低8位数据
	res[i++] = (res_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (res_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (res_len >> 24) & 0XFF;	//	高8位数据
	//	5 封装 res_result
	res[i++] = res_result;
	//	6 封装 res_data
	if(res_result == 0)		//	错误,封装错误码
    {
     	res[i++] = 错误码 & 0XFF;			//	低8位数据
        res[i++] = (错误码 >> 8) & 0XFF;	//	次低8位数据
        res[i++] = (错误码 >> 16) & 0XFF;	//	次高8位数据
        res[i++] = (错误码 >> 24) & 0XFF;	//	高8位  
    }
	else		//	没错,封装所有的文件名
    {
        strncpy(res+i,filename,res_len-1);
		i += res_len-1;
    }

	//	7 封装包尾
	cmd[i++] = 0XC0;
            
再举个例子:
	CMD:	get		回复文件的大小(4B),后续不再使用该格式发送,单纯发送文件内容
	数据包格式:	包头 pkg_len cmd_no res_len res_result res_data 包尾
	数据包大小:	 1	   4	  4		  4			1		 4   	1
        
   	//	利用 文件IO 知识判断对应文件是否存在;文件大小(存在)/错误码(不存在)
    int res_data;
	... 读取目录项
    如果不存在 res_result = 0 res_data = 错误码
    如果成功 res_result = 1 res_data = filesize
        
    //	封装数据包
    int res_len = 5;				//	错误码/文件大小 都是4B
    int pkg_len = 19;				//	数据包长度,包括头尾
	unsigned char res[19] = {0};	//	保存具体数据包
	int i = 0;						//	数据包下标
	//	1 封装包头 
	res[i++] = 0XC0;

	//	2 封装 pkg_len 以小端模式保存
	res[i++] = pkg_len & 0XFF;			//	低8位数据
	res[i++] = (pkg_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (pkg_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (pkg_len >> 24) & 0XFF;	//	高8位数据

	//	3 封装 cmd_no 以小端模式保存
	res[i++] = FTP_CMD_GET & 0XFF;			//	低8位数据
	res[i++] = (FTP_CMD_GET >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (FTP_CMD_GET >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (FTP_CMD_GET >> 24) & 0XFF;	//	高8位数据
	
	//	4 封装 res_len 以小端模式保存
	res[i++] = res_len & 0XFF;			//	低8位数据
	res[i++] = (res_len >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (res_len >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (res_len >> 24) & 0XFF;	//	高8位数据
	//	5 封装 res_result
	res[i++] = res_result;
	//	6 封装 res_data
    res[i++] = res_data & 0XFF;			//	低8位数据
    res[i++] = (res_data >> 8) & 0XFF;	//	次低8位数据
    res[i++] = (res_data >> 16) & 0XFF;	//	次高8位数据
    res[i++] = (res_data >> 24) & 0XFF;	//	高8位  
   
	//	7 封装包尾
	cmd[i++] = 0XC0;

    if(res_result == 1)
    {
        sum = 0;
        while(sum < filesize)
        {
			r = read(本地)
        	sum += write(网络,r);  
        }
    }
其它同理!!!
*/

/*
    //	1 先找数据包的包头	0XC0
    unsigned char ch;
    do
    {
        if(read(sockfd,&ch,1) == -1)
        {
            有问题
        }
    }while(ch != 0XC0);		//	ch == 0XC0
    //	上述 do-while 结束;ch == 0XC0 可能是上一数据包的包尾
    while(ch == 0XC0)
    {
        if(read(sockfd,&ch,1) == -1)
        {
            有问题
        } 
    }
	//	while 结束;ch != 0XC0,理论上是数据包的 第二字节
	//	ch 保存 pkg_len 的第一个字节
	//	此时有两种做法:
	//		①直接一次全部读取完毕,直到读取到包尾的 0XC0(如果数据中有 0XC0 崩掉)
	//		②先读取包长,再根据包长;读取剩余内容	使用的!!!
	//	2 解析 pkg_len
	int i = 0;
	int pkg_len = ch;		//	数据包格式为 小端模式
	while(i++ < 3)
    {
    	read(sockfd,&ch,1);
        pkg_len = ((ch << (8*i)) | pkg_len);	//	得到数据包长度
    }
	printf("pkg_len = %d\n",pkg_len);
	//	3 一次性读取剩余内容(不读取包尾,已经读取了包头和包长)
	i = 0;			//	标识读取长度
	unsigned char buf[pkg_len-6];
	while(1)
    {
       	read(sockfd,&ch,1);
        if(ch == 0XC0)//	读取到包尾
        {
            break;
        }
     	buf[i++] = ch;		//	读取到的内容保存到数组中
    }
	//	4 验证数据是否正确
	if(pkg_len-6 != i)		//	中间数据出现了 0XC0
    {
       	有问题
    }
	//	5 根据对应的命令执行对应操作
	//	先获取数据包的命令号
	CMD_NO cmd_no = buf[0] | buf[1] << 8 | buf[2] << 16 | buf[3] << 24;
	......
*/
#endif

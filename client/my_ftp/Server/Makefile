#目标名称
TARGET:=./bin/server

#指定编译器
CC:=gcc

#头文件路径
INC:=-I ./inc

#tftp 服务器目录
#TFTP:=~/tftpboot

#CSRC 保存了指定目录下所有以 .c 结尾的文件名且空格隔开
CSRC:=$(wildcard ./src/*.c)	

#OJBS 保存 将 CSRC 中所有 .c 结尾是文件名替换成了 .o 结尾
OJBS:=$(patsubst %.c,%.o,$(CSRC))

$(TARGET):$(OJBS)
	$(CC) $^ $(INC) -o $@
	rm ./src/*.o

%.o:%.c
	$(CC) -c $< $(INC) $(LIB) $(LIB_NAME) -o $@

clean:
	rm ./src/*.o
	rm $(TARGET)

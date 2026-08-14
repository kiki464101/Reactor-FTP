# =====================================================================
#  服务器构建配置
# =====================================================================
#  把 server/src/ 下的 6 个 .c 文件编译链接成可执行程序 bin/ftp_server。
#  服务器不需要图形界面，所以只链接 m（数学库）和 pthread（线程库）。
# =====================================================================

cmake_minimum_required(VERSION 3.10)
project(ftp_server C)              # 项目名，纯 C 项目
set(CMAKE_C_STANDARD 99)           # 使用 C99 标准
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/../bin)   # 输出到上一级 bin/ 目录
include_directories(inc)           # 头文件在 inc/ 目录
add_executable(ftp_server          # 生成可执行文件 ftp_server
    src/main.c                     # 主 Reactor 事件循环 + 连接管理
    src/handler.c                  # 命令处理器（登录/列表/下载/上传等）
    src/protocol.c                 # 协议封包/解包
    src/sys_auth.c                 # 用户认证（读 users.conf）
    src/ipc_shm.c                  # 共享内存（客户端状态监控）
    src/thread_pool.c              # 线程池
)
target_link_libraries(ftp_server m pthread)   # 链接数学库 + 线程库
add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/ftp_server DEPENDS ftp_server)

# =====================================================================
#  客户端构建配置（CMakeLists.txt）
# =====================================================================
#  【小白速懂：CMake 是什么？】
#    CMake 是一个"构建工具"。它读取本文件，生成 Makefile，再调用编译器
#    (gcc) 把 .c 源文件编译链接成可执行程序 bin/main。
#
#  本文件做的事：
#    1. 指定项目名、C 语言标准
#    2. 找到依赖库（SDL2，用来显示图形窗口）
#    3. 指定头文件搜索路径
#    4. 把 LVGL、FreeType 作为子项目一起编译
#    5. 把我们的 main.c、mouse_cursor_icon.c、client/*.c 编译成 bin/main
#    6. 链接所有库
# =====================================================================

cmake_minimum_required(VERSION 3.10)   # 要求 CMake 最低版本

project(lvgl)                          # 项目名

set(CMAKE_C_STANDARD 99)               # 使用 C99 标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)   # 可执行文件输出到 bin/ 目录

link_directories(./)                   # 库文件搜索路径设为当前目录

# ---- 外部依赖 ----
find_package(PkgConfig QUIET)          # 查找 pkg-config 工具

# SDL2（图形显示库，在电脑上模拟屏幕，必需）
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 REQUIRED sdl2)
else()
    find_package(SDL2 REQUIRED)
endif()

# ---- 头文件搜索路径（编译时去哪找 .h 文件）----
include_directories(
    ${PROJECT_SOURCE_DIR}                      # 项目根目录（找 lvgl/lvgl.h 等）
    client   # 找 ui_manager.h、network_task.h
    client/my_ftp/Client/inc  # 找 protocol.h
    freetype-2.13.3/include/   # FreeType 字体库头文件
    ${SDL2_INCLUDE_DIRS}        # SDL2 头文件
)

# ---- 子项目（把第三方库作为子项目一起编译）----
add_subdirectory(freetype-2.13.3)   # FreeType：负责字体渲染
add_subdirectory(lvgl)              # LVGL：负责图形界面
target_include_directories(lvgl PUBLIC ${PROJECT_SOURCE_DIR}
                           PRIVATE freetype-2.13.3/include/freetype)

# ---- 我们的源文件 ----
aux_source_directory(client SRC_FILE)          # 自动收集 client/ 目录下所有 .c 文件
add_executable(main main.c mouse_cursor_icon.c ${SRC_FILE})  # 生成可执行文件 main

# ---- 链接库 ----
target_link_libraries(main
    lvgl              # LVGL 图形库
    lvgl::examples    # LVGL 示例
    lvgl::demos       # LVGL 演示
    lvgl::thorvg      # LVGL 矢量图渲染
    ${SDL2_LIBRARIES} # SDL2（窗口显示）
    m                 # 数学库（三角函数等）
    pthread           # POSIX 线程库（多线程）
    freetype          # FreeType 字体库
)

add_custom_target(run COMMAND ${EXECUTABLE_OUTPUT_PATH}/main DEPENDS main)  # 执行 make run 直接运行
# ---- 服务器（可选，独立二进制）----
# 取消下面这行注释可以把服务器也一起编译：
# add_subdirectory(server)

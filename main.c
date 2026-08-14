/**
 * @file main.c
 * @brief 客户端程序入口 —— 嵌入式远程文件管理器（图形界面）的启动起点
 *
 * =====================================================================
 *  本文件是整个"客户端"程序的 main() 函数所在地，它只做三件事：
 *
 *    1. lv_init()             —— 初始化 LVGL 图形库
 *    2. lv_linux_disp_init()  —— 初始化"显示设备"（在电脑上用 SDL2 模拟屏幕）
 *    3. while(1) 死循环        —— 不断驱动 LVGL 刷新界面、处理事件
 *
 *  【小白速懂：LVGL 是什么？】
 *    LVGL 是一个开源的嵌入式图形界面（GUI）库。你可以把它理解成一个
 *    "会画按钮、文本框、进度条的引擎"。它自己不直接操作显示器，而是
 *    通过"驱动(driver)"来画图。真实的嵌入式设备用 LCD 驱动，而本项目
 *    在电脑上开发调试，所以用 SDL2 在桌面窗口里模拟一个屏幕。
 *
 *  【小白速懂：图形程序为什么是个"死循环"？】
 *    一个带界面的程序本质上是一个不停运行的循环。每循环一次：
 *       - lv_timer_handler() 让 LVGL 处理到期的定时任务和积压的事件
 *         （比如你点击了按钮、敲了键盘、进度条需要前进一格）
 *       - LVGL 据此重绘界面
 *    循环中间的 usleep(5000) 是"睡 5 毫秒"，避免 CPU 空转烧到 100%。
 * =====================================================================
 */

#include "lvgl/lvgl.h"                                    /* LVGL 核心头文件          */
#include "lvgl/demos/lv_demos.h"                          /* LVGL 官方演示            */
#include "lvgl/src/drivers/sdl/lv_sdl_keyboard.h"         /* SDL 键盘驱动             */
#include "lvgl/src/drivers/sdl/lv_sdl_mousewheel.h"       /* SDL 滚轮驱动             */
#include <unistd.h>                                       /* usleep() 等系统调用      */
#include <pthread.h>                                      /* 多线程（网络线程会用到）  */
#include <time.h>
#include "lvgl/examples/lv_examples.h"                    /* LVGL 官方示例            */
#include <stdio.h>
#include "ui_manager.h"                                   /* 我们自己的界面管理模块    */

/**
 * 从环境变量读取一个配置值，读不到就用默认值。
 * 例如 getenv_default("LV_SDL_VIDEO_WIDTH", "1024")
 *   如果用户设了 LV_SDL_VIDEO_WIDTH 环境变量就用它，否则用 "1024"。
 * 注意：`?:` 是 GNU C 的扩展语法（相当于"如果为空就取后面的值"）。
 */
static const char *getenv_default(const char *name, const char *dflt)
{
    return getenv(name) ?: dflt;
}

/* =====================================================================
 *  下面是"显示设备初始化"函数 lv_linux_disp_init() 的三种实现。
 *  通过 #if / #elif / #else 条件编译，只保留其中一种（由 lv_conf.h 决定）：
 *    - LV_USE_LINUX_FBDEV：使用 Linux 帧缓冲(/dev/fb0)，真实嵌入式板卡用
 *    - LV_USE_LINUX_DRM  ：使用 DRM 显卡驱动
 *    - LV_USE_SDL        ：使用 SDL2 桌面窗口（本项目默认用这个，方便调试）
 * ===================================================================== */

#if LV_USE_LINUX_FBDEV
static void lv_linux_disp_init(void)
{
    const char *device = getenv_default("LV_LINUX_FBDEV_DEVICE", "/dev/fb0");
    lv_display_t *disp = lv_linux_fbdev_create();

    lv_linux_fbdev_set_file(disp, device);

    lv_indev_t * indev = lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event6");
}
#elif LV_USE_LINUX_DRM
static void lv_linux_disp_init(void)
{
    const char *device = getenv_default("LV_LINUX_DRM_CARD", "/dev/dri/card0");
    lv_display_t *disp = lv_linux_drm_create();

    lv_linux_drm_set_file(disp, device, -1);
}
#elif LV_USE_SDL
static void lv_linux_disp_init(void)
{
    /* 从环境变量读取窗口宽高（默认 1024x600） */
    const int width = atoi(getenv("LV_SDL_VIDEO_WIDTH") ?: "1024");
    const int height = atoi(getenv("LV_SDL_VIDEO_HEIGHT") ?: "600");

    /* 创建一个 SDL 窗口当作"屏幕" */
    lv_sdl_window_create(width, height);

    /* SDL window only auto-creates a mouse — we must explicitly
     * create keyboard and mousewheel input devices, otherwise
     * textareas won't receive any keystrokes. */
    /* 说明：SDL 窗口默认只自动创建鼠标，必须手动创建键盘和滚轮输入设备，
     * 否则文本框（输入 IP、密码的地方）收不到键盘按键。 */
    lv_sdl_keyboard_create();
    lv_sdl_mousewheel_create();
}
#else
#error Unsupported configuration
#endif



/**
 * 程序入口 main()。
 * 执行顺序：
 *   1. lv_init()            —— 初始化 LVGL
 *   2. lv_linux_disp_init() —— 初始化显示（打开 SDL 窗口）
 *   3. ui_login_init()      —— 创建并显示"登录界面"（IP/端口/用户名/密码）
 *   4. while(1)             —— 事件循环，持续刷新界面直到程序被关闭
 *
 *  注意：真正的网络连接不在这里，而是在点击"登录"按钮后，
 *        由 ui_manager.c 调用 network_task.c 里的 network_start_connect()
 *        开启一个独立的"网络线程"去连服务器。这个 main 线程只负责"画界面"。
 */
int main(void) {
    lv_init();                     /* 初始化 LVGL 图形库 */
    lv_linux_disp_init();          /* 初始化显示设备（打开窗口） */
    ui_login_init();               /* 显示登录界面（见 ui_manager.c） */

    while(1) {
        lv_timer_handler();        /* 让 LVGL 处理定时任务和事件（核心驱动） */
        usleep(5000);              /* 睡 5 毫秒，降低 CPU 占用 */
    }
    return 0;
}

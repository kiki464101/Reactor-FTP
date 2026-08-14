/**
 * @file ui_manager.c
 * @brief LVGL FTP 客户端 UI 管理器模块实现
 *
 * 本模块实现了 FTP 客户端的所有用户界面，包括：
 * - 登录界面：IP/端口/用户名/密码输入，连接按钮
 * - 主界面（文件管理器）：远程和本地文件列表，支持多选、下载、上传、删除
 * - 进度弹窗：单文件和批量传输进度显示
 *
 * 线程安全设计：
 *   所有 LVGL 组件的创建、修改、销毁操作必须在 UI 线程中执行。
 *   网络线程通过 lv_async_call() 将数据推送到 UI 线程的上下文中处理。
 *   UI 线程不能直接访问网络线程的内部状态。
 *
 * 界面布局（主界面）：
 *   ┌─────────────────────────────┐
 *   │  状态栏（用户信息/连接状态）   │
 *   ├─────────────────────────────┤
 *   │  远程文件列表（服务器文件）    │
 *   │  - 支持点击导航子目录         │
 *   │  - 支持长按多选              │
 *   ├─────────────────────────────┤
 *   │  本地文件列表（客户端文件）    │
 *   │  - 支持点击导航子目录         │
 *   │  - 支持长按多选              │
 *   ├─────────────────────────────┤
 *   │ [刷新][下载][上传][删除][断开] │← 操作按钮行
 *   └─────────────────────────────┘
 */

#include "ui_manager.h"
#include "network_task.h"
#include <dirent.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define SIZE 4096                   /* 文件列表扫描缓冲区大小 */

/* ================================================================== */
/*  多选状态                                                           */
/* ================================================================== */
char g_selected_remote[MAX_SELECTED_FILES][256] = {{0}};   /* 远程选中的文件路径 */
int  g_remote_sel_count = 0;                                /* 远程选中数量      */
char g_selected_local[MAX_SELECTED_FILES][256]  = {{0}};    /* 本地选中的文件路径 */
int  g_local_sel_count  = 0;                                /* 本地选中数量      */

/* ================================================================== */
/*  文件列表镜像数组                                                    */
/*  存储当前 LVGL 列表组件中显示的文件名                                */
/*                                                                      */
/*  用途：让网络层可以在不访问 LVGL 组件内部状态的情况下                  */
/*        查询"某个文件名是否已在列表中显示"                              */
/*                                                                      */
/*  线程安全：仅由 UI 线程写入；网络/工作线程只读（单写者模式）           */
/* ================================================================== */
char g_remote_displayed_files[MAX_SELECTED_FILES][256] = {{0}};
int  g_remote_displayed_count = 0;
char g_local_displayed_files[MAX_SELECTED_FILES][256]  = {{0}};
int  g_local_displayed_count  = 0;

/* ================================================================== */
/*  界面对象引用                                                        */
/* ================================================================== */
lv_obj_t *login_screen = NULL;      /* 登录界面    */
lv_obj_t *main_screen  = NULL;      /* 主界面      */

/* ---- 登录界面组件 ---- */
static lv_obj_t *login_ip_ta;       /* IP 地址输入框      */
static lv_obj_t *login_port_ta;     /* 端口号输入框       */
static lv_obj_t *login_user_ta;     /* 用户名输入框       */
static lv_obj_t *login_pass_ta;     /* 密码输入框         */
static lv_obj_t *login_btn;         /* 连接/登录按钮      */
static lv_obj_t *login_status;      /* 登录状态标签       */
static lv_obj_t *login_title;       /* 登录界面标题       */
static lv_obj_t *login_spinner;     /* 加载动画（备用）   */

/* ---- 主界面组件 ---- */
static lv_obj_t *main_status_bar;   /* 顶部状态栏         */
static lv_obj_t *main_file_list;    /* 远程（服务器）文件列表 */
static lv_obj_t *main_local_list;   /* 本地文件列表        */

/* ---- 选中高亮样式 ---- */
static lv_style_t style_selected_remote;  /* 远程文件选中样式 */
static lv_style_t style_selected_local;   /* 本地文件选中样式 */
static lv_obj_t *main_selected_label;     /* 选中计数标签     */

/* ---- 进度弹窗（基于活动界面创建） ---- */
static lv_obj_t *prog_panel   = NULL;     /* 进度弹窗面板 */
static lv_obj_t *prog_label   = NULL;     /* 进度标题     */
static lv_obj_t *prog_bar     = NULL;     /* 进度条       */
static lv_obj_t *prog_info    = NULL;     /* 进度详情     */

/* ---- 屏幕键盘（多输入框共享） ---- */
static lv_obj_t *kb = NULL;

/**
 * 获取当前可用的父容器
 * 优先使用顶层图层，回退到活动界面
 */
static lv_obj_t *ui_get_window_parent(void)
{
    lv_obj_t *parent = lv_layer_top();
    if (!parent) parent = lv_scr_act();
    return parent;
}

/* ================================================================== */
/*  批量传输进度跟踪                                                     */
/* ================================================================== */
#define MAX_PROGRESS_BARS 10        /* 批量面板中最多显示的进度条数 */

/** 单个进度条槽位 */
typedef struct {
    lv_obj_t *bar;                  /* 进度条组件       */
    lv_obj_t *label;                /* 标签组件         */
    char      filename[256];        /* 关联的文件名     */
    bool      is_upload;            /* 上传/下载        */
    bool      active;               /* 是否活跃         */
    bool      done;                 /* 是否已完成       */
} prog_slot_t;

static prog_slot_t prog_slots[MAX_PROGRESS_BARS];  /* 进度条槽位数组    */
static lv_obj_t   *batch_prog_panel = NULL;         /* 批量进度面板      */
static int          batch_prog_count = 0;            /* 当前活跃槽位数    */

/* 当前浏览路径 */
static char g_local_cur_path[256] = {0};    /* 本地文件浏览的当前路径   */
static char g_remote_cur_path[256] = {0};   /* 远程文件浏览的当前路径   */

/* 错误/确认弹窗单例 */
static lv_obj_t *err_popup      = NULL;      /* 错误提示弹窗          */
static lv_obj_t *err_label      = NULL;      /* 错误弹窗中的文字标签   */
static lv_obj_t *confirm_popup = NULL;       /* 确认删除弹窗          */

/* 长按状态标记（抑制长按后的点击事件） */
static bool g_local_long_pressed  = false;   /* 本地列表刚发生长按     */
static bool g_remote_long_pressed = false;   /* 远程列表刚发生长按     */

/* ================================================================== */
/*  事件处理函数的前向声明                                               */
/* ================================================================== */
static void on_login_btn_clicked(lv_event_t *e);
static void on_file_item_clicked(lv_event_t *e);
static void on_local_file_item_clicked(lv_event_t *e);
static void on_local_file_item_long_pressed(lv_event_t *e);
static void on_remote_file_item_long_pressed(lv_event_t *e);
static void on_refresh_btn_clicked(lv_event_t *e);
static void on_download_btn_clicked(lv_event_t *e);
static void on_upload_btn_clicked(lv_event_t *e);
static void on_disconnect_btn_clicked(lv_event_t *e);
static void on_delete_btn_clicked(lv_event_t *e);
static void on_delete_confirm_yes_btn_clicked(lv_event_t *e);
static void on_delete_confirm_no_btn_clicked(lv_event_t *e);
static void execute_local_delete(void);
static void ui_show_confirm_popup(void);
static void on_cancel_btn_clicked(lv_event_t *e);
static void on_close_progress_btn_clicked(lv_event_t *e);
static void on_ta_focused(lv_event_t *e);
static void on_keyboard_event(lv_event_t *e);

/* ================================================================== */
/*  辅助函数：创建带文本的按钮                                           */
/* ================================================================== */

/**
 * 在指定父容器上创建一个带文本标签的按钮
 *
 * @param parent  父容器
 * @param text    按钮文本
 * @param w       按钮宽度
 * @param h       按钮高度
 * @param cb       点击事件回调函数（可为 NULL）
 * @return        创建的按钮对象
 */
static lv_obj_t *create_btn(lv_obj_t *parent, const char *text,
                             lv_coord_t w, lv_coord_t h,
                             lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_center(lbl);                                 /* 文字居中 */
    if (cb) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ================================================================== */
/*  登录界面                                                            */
/*  布局：标题 → IP输入 → 端口输入 → 用户名输入 → 密码输入 → 连接按钮 → 状态 */
/* ================================================================== */

/* 组件尺寸常量 */
#define BTN_W  160      /* 按钮宽度   */
#define BTN_H   40      /* 按钮高度   */
#define TA_W   280      /* 文本框宽度 */
#define TA_H    36      /* 文本框高度 */
#define LBL_H   22      /* 标签高度   */

/**
 * 初始化并显示登录界面
 * 如果已存在则清空并重新加载
 */
void ui_login_init(void)
{
    if (login_screen) {
        lv_obj_clean(login_screen);                     /* 清空已有界面 */
        lv_screen_load(login_screen);
        return;
    }

    login_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(login_screen, lv_color_hex(0x1a1a2e), 0);  /* 深蓝背景 */

    /* ---- 标题 ---- */
    login_title = lv_label_create(login_screen);
    lv_label_set_text(login_title,
        "Embedded Remote File Manager\nLVGL FTP Client");
    lv_obj_set_style_text_align(login_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(login_title, lv_color_hex(0x00d2ff), 0);  /* 青色标题 */
    lv_obj_set_style_text_font(login_title, &lv_font_montserrat_20, 0);
    lv_obj_align(login_title, LV_ALIGN_TOP_MID, 0, 20);

    /* ---- 表单容器（使用 Flex 布局） ---- */
    lv_obj_t *cont = lv_obj_create(login_screen);
    lv_obj_set_size(cont, 380, 420);
    lv_obj_center(cont);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);   /* 透明背景 */
    lv_obj_set_style_border_width(cont, 0, 0);           /* 无边框   */
    lv_obj_set_style_pad_all(cont, 8, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);     /* 列方向排列 */
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);

    /* ---- IP 地址输入 ---- */
    lv_obj_t *lbl1 = lv_label_create(cont);
    lv_label_set_text(lbl1, "Server IP");
    lv_obj_set_style_text_color(lbl1, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_margin_top(lbl1, 6, 0);

    login_ip_ta = lv_textarea_create(cont);
    lv_obj_set_size(login_ip_ta, TA_W, TA_H);
    lv_textarea_set_one_line(login_ip_ta, true);
    lv_textarea_set_cursor_click_pos(login_ip_ta, true);
    lv_textarea_set_max_length(login_ip_ta, 32);
    lv_textarea_set_placeholder_text(login_ip_ta, "127.0.0.1");
    lv_textarea_set_text(login_ip_ta, "127.0.0.1");      /* 默认值 */
    lv_obj_add_event_cb(login_ip_ta, on_ta_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(login_ip_ta, on_ta_focused, LV_EVENT_CLICKED, NULL);

    /* ---- 端口号输入 ---- */
    lv_obj_t *lbl2 = lv_label_create(cont);
    lv_label_set_text(lbl2, "Port");
    lv_obj_set_style_text_color(lbl2, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_margin_top(lbl2, 6, 0);

    login_port_ta = lv_textarea_create(cont);
    lv_obj_set_size(login_port_ta, TA_W, TA_H);
    lv_textarea_set_one_line(login_port_ta, true);
    lv_textarea_set_cursor_click_pos(login_port_ta, true);
    lv_textarea_set_max_length(login_port_ta, 8);
    lv_textarea_set_placeholder_text(login_port_ta, "8888");
    lv_textarea_set_text(login_port_ta, "8888");         /* 默认值 */
    lv_obj_add_event_cb(login_port_ta, on_ta_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(login_port_ta, on_ta_focused, LV_EVENT_CLICKED, NULL);

    /* ---- 用户名输入 ---- */
    lv_obj_t *lbl3 = lv_label_create(cont);
    lv_label_set_text(lbl3, "Username");
    lv_obj_set_style_text_color(lbl3, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_margin_top(lbl3, 6, 0);

    login_user_ta = lv_textarea_create(cont);
    lv_obj_set_size(login_user_ta, TA_W, TA_H);
    lv_textarea_set_one_line(login_user_ta, true);
    lv_textarea_set_cursor_click_pos(login_user_ta, true);
    lv_textarea_set_max_length(login_user_ta, 32);
    lv_textarea_set_placeholder_text(login_user_ta, "admin");
    lv_textarea_set_text(login_user_ta, "admin");        /* 默认值 */
    lv_obj_add_event_cb(login_user_ta, on_ta_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(login_user_ta, on_ta_focused, LV_EVENT_CLICKED, NULL);

    /* ---- 密码输入 ---- */
    lv_obj_t *lbl4 = lv_label_create(cont);
    lv_label_set_text(lbl4, "Password");
    lv_obj_set_style_text_color(lbl4, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_margin_top(lbl4, 6, 0);

    login_pass_ta = lv_textarea_create(cont);
    lv_obj_set_size(login_pass_ta, TA_W, TA_H);
    lv_textarea_set_one_line(login_pass_ta, true);
    lv_textarea_set_cursor_click_pos(login_pass_ta, true);
    lv_textarea_set_max_length(login_pass_ta, 32);
    lv_textarea_set_password_mode(login_pass_ta, true);   /* 密码模式（显示为 *） */
    lv_textarea_set_placeholder_text(login_pass_ta, "password");
    lv_textarea_set_text(login_pass_ta, "123456");        /* 默认值 */
    lv_obj_add_event_cb(login_pass_ta, on_ta_focused, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(login_pass_ta, on_ta_focused, LV_EVENT_CLICKED, NULL);

    /* ---- 间距 ---- */
    lv_obj_t *sp = lv_obj_create(cont);
    lv_obj_set_size(sp, 1, 10);
    lv_obj_set_style_bg_opa(sp, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sp, 0, 0);

    /* ---- 连接按钮 ---- */
    login_btn = lv_button_create(cont);
    lv_obj_set_size(login_btn, BTN_W, BTN_H);
    lv_obj_set_style_bg_color(login_btn, lv_color_hex(0x007bff), 0);  /* 蓝色按钮 */
    lv_obj_set_style_radius(login_btn, 6, 0);
    lv_obj_t *btn_lbl = lv_label_create(login_btn);
    lv_label_set_text(btn_lbl, "Connect / Login");
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(login_btn, on_login_btn_clicked, LV_EVENT_CLICKED,
                         NULL);

    /* ---- 状态文字 ---- */
    login_status = lv_label_create(cont);
    lv_label_set_text(login_status, "Ready");
    lv_obj_set_style_text_color(login_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_margin_top(login_status, 6, 0);

    lv_screen_load(login_screen);
}

/* ================================================================== */
/*  登录按钮事件处理                                                     */
/* ================================================================== */

/**
 * 登录按钮点击处理
 * 从输入框读取连接参数，调用 network_start_connect()，
 * 禁用按钮防止重复点击，状态反馈通过异步回调完成
 */
static void on_login_btn_clicked(lv_event_t *e)
{
    (void)e;
    const char *ip   = lv_textarea_get_text(login_ip_ta);
    const char *port = lv_textarea_get_text(login_port_ta);
    const char *user = lv_textarea_get_text(login_user_ta);
    const char *pass = lv_textarea_get_text(login_pass_ta);

    if (strlen(ip) < 7 || strlen(port) < 1) {            /* 基本校验 */
        lv_label_set_text(login_status, "Please enter IP and Port");
        return;
    }

    lv_label_set_text(login_status, "Connecting...");
    lv_obj_set_style_text_color(login_status, lv_color_hex(0xffaa00), 0);  /* 橙色状态 */

    /* 禁用按钮防止重复点击 */
    lv_obj_add_state(login_btn, LV_STATE_DISABLED);

    if (!network_start_connect(ip, port, user, pass)) {   /* 启动连接（异步） */
        lv_label_set_text(login_status, "Failed to start connection thread");
        lv_obj_clear_state(login_btn, LV_STATE_DISABLED);  /* 失败则恢复按钮 */
    }
}

/* ================================================================== */
/*  主界面（文件管理器）                                                  */
/*                                                                      */
/*  布局结构：                                                          */
/*    - 顶部状态栏                                                       */
/*    - 远程文件列表（上半屏）                                            */
/*    - 本地文件列表（下半屏）                                            */
/*    - 底部操作按钮行                                                    */
/* ================================================================== */

/**
 * 初始化主界面
 * 创建文件列表、操作按钮等所有组件
 * 不会自动切换到此界面（由 ui_switch_to_main() 调用 lv_screen_load）
 */
void ui_main_init(void)
{
    if (main_screen) {
        lv_obj_clean(main_screen);                       /* 已存在则清空重建 */
        lv_screen_load(main_screen);
        return;
    }

    main_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(main_screen, lv_color_hex(0x16213e), 0);  /* 深蓝背景 */

    /* ======== 顶部状态栏 ======== */
    main_status_bar = lv_label_create(main_screen);
    lv_obj_set_size(main_status_bar, LV_PCT(100), 28);
    lv_obj_set_style_bg_color(main_status_bar, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_bg_opa(main_status_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_left(main_status_bar, 10, 0);
    lv_obj_set_style_text_color(main_status_bar, lv_color_hex(0x00d2ff), 0);
    lv_obj_align(main_status_bar, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_text(main_status_bar, "Connecting...");

    /* 初始化选中高亮样式 */
    lv_style_init(&style_selected_remote);
    lv_style_set_bg_color(&style_selected_remote, lv_color_hex(0x228B22));  /* 绿色高亮 */
    lv_style_set_bg_opa(&style_selected_remote, LV_OPA_COVER);
    lv_style_init(&style_selected_local);
    lv_style_set_bg_color(&style_selected_local, lv_color_hex(0x228B22));
    lv_style_set_bg_opa(&style_selected_local, LV_OPA_COVER);

    /* ======== 路径标签（隐藏，调试用） ======== */
    lv_obj_t *path_lbl = lv_label_create(main_screen);
    lv_obj_set_style_text_color(path_lbl, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(path_lbl, LV_ALIGN_TOP_LEFT, 10, 32);
    lv_label_set_text(path_lbl, "Directory: /remote_share/");
    lv_obj_add_flag(path_lbl, LV_OBJ_FLAG_HIDDEN);

    /* ======== 远程文件列表（上半屏） ======== */
    lv_obj_t *remote_header = lv_label_create(main_screen);
    lv_label_set_text(remote_header, "Remote Files (server):");  /* 服务器文件 */
    lv_obj_set_style_text_color(remote_header, lv_color_hex(0x00d2ff), 0);
    lv_obj_align(remote_header, LV_ALIGN_TOP_LEFT, 10, 32);

    lv_obj_t *remote_cont = lv_obj_create(main_screen);
    lv_obj_set_size(remote_cont, LV_PCT(96), 200);       /* 宽 96%，高 200px */
    lv_obj_align(remote_cont, LV_ALIGN_TOP_MID, 0, 54);
    lv_obj_set_style_bg_color(remote_cont, lv_color_hex(0x1a1a3e), 0);
    lv_obj_set_style_border_color(remote_cont, lv_color_hex(0x334466), 0);
    lv_obj_set_style_radius(remote_cont, 6, 0);
    lv_obj_set_scrollbar_mode(remote_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(remote_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(remote_cont, LV_DIR_VER);       /* 仅垂直滚动 */

    main_file_list = lv_list_create(remote_cont);
    lv_obj_set_size(main_file_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(main_file_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_file_list, 0, 0);
    lv_obj_set_scrollbar_mode(main_file_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(main_file_list, LV_DIR_VER);
    lv_list_add_text(main_file_list, "Click Refresh");    /* 初始提示 */

    /* ======== 本地文件列表（下半屏，与远程列表等大） ======== */
    lv_obj_t *local_header = lv_label_create(main_screen);
    lv_label_set_text(local_header, "Local Files (client):");  /* 本地文件 */
    lv_obj_set_style_text_color(local_header, lv_color_hex(0x00d2ff), 0);
    lv_obj_align(local_header, LV_ALIGN_TOP_LEFT, 10, 258);

    lv_obj_t *local_cont = lv_obj_create(main_screen);
    lv_obj_set_size(local_cont, LV_PCT(96), 200);
    lv_obj_align(local_cont, LV_ALIGN_TOP_MID, 0, 280);
    lv_obj_set_style_bg_color(local_cont, lv_color_hex(0x1a1a3e), 0);
    lv_obj_set_style_border_color(local_cont, lv_color_hex(0x334466), 0);
    lv_obj_set_style_radius(local_cont, 6, 0);
    lv_obj_set_scrollbar_mode(local_cont, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_add_flag(local_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(local_cont, LV_DIR_VER);

    main_local_list = lv_list_create(local_cont);
    lv_obj_set_size(main_local_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(main_local_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_local_list, 0, 0);
    lv_obj_set_scrollbar_mode(main_local_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(main_local_list, LV_DIR_VER);
    lv_list_add_text(main_local_list, "Click Refresh");   /* 初始提示 */

    /* ======== 选中计数显示 ======== */
    main_selected_label = lv_label_create(main_screen);
    lv_obj_align(main_selected_label, LV_ALIGN_BOTTOM_LEFT, 10, -48);
    lv_obj_set_style_text_color(main_selected_label, lv_color_hex(0x88ccff), 0);
    lv_label_set_text(main_selected_label, "Remote: 0 | Local: 0");

    /* ======== 底部操作按钮行 ======== */
    lv_obj_t *btn_row = lv_obj_create(main_screen);
    lv_obj_set_size(btn_row, LV_PCT(100), 46);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(btn_row, lv_color_hex(0x0f3460), 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_left(btn_row, 4, 0);
    lv_obj_set_style_pad_right(btn_row, 4, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);      /* 水平排列 */
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 创建五个操作按钮 */
    create_btn(btn_row, "Refresh",    90, 34, on_refresh_btn_clicked);
    create_btn(btn_row, "Download",   90, 34, on_download_btn_clicked);
    create_btn(btn_row, "Upload",     90, 34, on_upload_btn_clicked);
    create_btn(btn_row, "Delete",     90, 34, on_delete_btn_clicked);
    create_btn(btn_row, "Disconnect", 100, 34, on_disconnect_btn_clicked);
}

/* ================================================================== */
/*  界面切换                                                            */
/* ================================================================== */

/** 切换到登录界面（断开连接时调用） */
void ui_switch_to_login(void)
{
    if (!login_screen) ui_login_init();                  /* 延迟初始化 */
    lv_screen_load(login_screen);
    if (login_status) {
        lv_label_set_text(login_status, "Disconnected");
        lv_obj_set_style_text_color(login_status, lv_color_hex(0xff6666), 0);  /* 红色提示 */
    }
    if (login_btn) lv_obj_clear_state(login_btn, LV_STATE_DISABLED);  /* 恢复按钮 */
}

/** 切换到主界面（登录成功后调用） */
void ui_switch_to_main(void)
{
    if (!main_screen) ui_main_init();                    /* 延迟初始化 */

    /* 更新状态栏信息 */
    char sb[256];
    snprintf(sb, sizeof(sb), "User: %s  |  %s  |  Connected",
             g_login_user[0] ? g_login_user : "admin",
             g_session_info[0] ? g_session_info : "N/A");
    if (main_status_bar) lv_label_set_text(main_status_bar, sb);

    /* 切换到主界面并请求初始文件列表 */
    lv_screen_load(main_screen);
    network_cmd_ls(NULL);                               /* 请求远程文件列表 */
    ui_refresh_local_files();                            /* 扫描本地文件    */
}

/* ================================================================== */
/*  进度弹窗                                                            */
/* ================================================================== */

/**
 * 显示批量传输进度面板
 * 包含：标题（文件名+百分比）、红色进度条、文件大小信息、关闭/取消按钮
 */
void ui_show_progress_batch(void)
{
    lv_obj_t *parent = ui_get_window_parent();
    if (!parent) return;

    /* 如果旧面板被隐藏了，先销毁再创建新的 */
    if (batch_prog_panel && lv_obj_has_flag(batch_prog_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_del(batch_prog_panel);
        batch_prog_panel = NULL;
        memset(prog_slots, 0, sizeof(prog_slots));
        batch_prog_count = 0;
    }

    /* 防止重复创建 */
    if (batch_prog_panel || prog_panel) return;

    memset(prog_slots, 0, sizeof(prog_slots));
    batch_prog_count = 0;

    batch_prog_panel = lv_obj_create(parent);
    lv_obj_set_size(batch_prog_panel, 340, 160);
    lv_obj_center(batch_prog_panel);
    lv_obj_set_style_bg_color(batch_prog_panel, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_color(batch_prog_panel, lv_color_hex(0xCC0000), 0);  /* 红色边框 */
    lv_obj_set_style_border_width(batch_prog_panel, 2, 0);
    lv_obj_set_style_radius(batch_prog_panel, 10, 0);
    lv_obj_set_style_pad_all(batch_prog_panel, 12, 0);

    /* 槽位0：标题（文件名 + 百分比） */
    prog_slots[0].label = lv_label_create(batch_prog_panel);
    lv_label_set_text(prog_slots[0].label, "Preparing transfer...");
    lv_obj_set_style_text_color(prog_slots[0].label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(prog_slots[0].label, &lv_font_montserrat_16, 0);
    lv_obj_align(prog_slots[0].label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 槽位0：红色进度条（高大醒目） */
    prog_slots[0].bar = lv_bar_create(batch_prog_panel);
    lv_obj_set_size(prog_slots[0].bar, 300, 30);
    lv_obj_align(prog_slots[0].bar, LV_ALIGN_CENTER, 0, 4);
    lv_bar_set_range(prog_slots[0].bar, 0, 100);
    lv_bar_set_value(prog_slots[0].bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(prog_slots[0].bar, lv_color_hex(0x331111), 0);       /* 暗红背景 */
    lv_obj_set_style_bg_color(prog_slots[0].bar, lv_color_hex(0xCC0000), LV_PART_INDICATOR);  /* 亮红指示 */
    lv_obj_set_style_radius(prog_slots[0].bar, 6, 0);
    lv_obj_set_style_anim_time(prog_slots[0].bar, 100, 0);  /* 100ms 动画 */

    /* 槽位1：文件大小信息 */
    prog_slots[1].label = lv_label_create(batch_prog_panel);
    lv_label_set_text(prog_slots[1].label, "0 B / 0 B");
    lv_obj_set_style_text_color(prog_slots[1].label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_align(prog_slots[1].label, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* 关闭按钮（仅隐藏面板，传输继续） */
    lv_obj_t *close_btn = lv_button_create(batch_prog_panel);
    lv_obj_set_size(close_btn, 70, 26);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_LEFT, 0, -4);
    lv_obj_t *cbl = lv_label_create(close_btn);
    lv_label_set_text(cbl, "Close");
    lv_obj_center(cbl);
    lv_obj_add_event_cb(close_btn, on_close_progress_btn_clicked, LV_EVENT_CLICKED, NULL);

    /* 取消按钮（停止传输 + 隐藏面板） */
    lv_obj_t *cancel_btn = lv_button_create(batch_prog_panel);
    lv_obj_set_size(cancel_btn, 80, 26);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, 0, -4);
    lv_obj_t *cbl2 = lv_label_create(cancel_btn);
    lv_label_set_text(cbl2, "Cancel");
    lv_obj_center(cbl2);
    lv_obj_add_event_cb(cancel_btn, on_cancel_btn_clicked, LV_EVENT_CLICKED, NULL);

    batch_prog_count = 0;
}

/**
 * 更新批量传输面板中当前文件的进度
 * 由网络线程通过 lv_async_call 在 UI 线程中调用
 */
void ui_update_transfer_progress(const char *filename, int percent,
                                  int current_bytes, int total_bytes,
                                  bool is_upload)
{
    if (!batch_prog_panel) return;

    /* 更新槽位0：标题文字 */
    if (prog_slots[0].label) {
        char title[300];
        snprintf(title, sizeof(title), "%s: %s  %d%%",
                 is_upload ? "Uploading" : "Downloading", filename, percent);
        lv_label_set_text(prog_slots[0].label, title);
    }

    /* 更新槽位0：进度条 */
    if (prog_slots[0].bar) {
        lv_bar_set_value(prog_slots[0].bar, percent, LV_ANIM_ON);
    }

    /* 更新槽位1：文件大小信息（自动选择 B/KB/MB 单位） */
    if (prog_slots[1].label) {
        char buf[128];
        if (total_bytes >= 1048576)                      /* >= 1 MB */
            snprintf(buf, sizeof(buf), "%.1f / %.1f MB",
                     current_bytes / 1048576.0f, total_bytes / 1048576.0f);
        else if (total_bytes >= 1024)                    /* >= 1 KB */
            snprintf(buf, sizeof(buf), "%.1f / %.1f KB",
                     current_bytes / 1024.0f, total_bytes / 1024.0f);
        else                                             /* < 1 KB */
            snprintf(buf, sizeof(buf), "%d / %d B", current_bytes, total_bytes);
        lv_label_set_text(prog_slots[1].label, buf);
    }
}

/**
 * 单个传输完成回调
 * 更新批量面板中的标题和进度条颜色（成功=绿色，失败=红色）
 */
void ui_on_transfer_done(const char *filename, bool success, bool is_upload)
{
    if (batch_prog_panel && prog_slots[0].label) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[%s] %s - %s",
                 is_upload ? "UP" : "DL", filename,
                 success ? "DONE" : "FAILED");
        lv_label_set_text(prog_slots[0].label, buf);
        lv_bar_set_value(prog_slots[0].bar, success ? 100 : 0, LV_ANIM_OFF);
        /* 成功绿色，失败红色 */
        lv_obj_set_style_bg_color(prog_slots[0].bar,
            success ? lv_color_hex(0x00CC00) : lv_color_hex(0xFF0000), LV_PART_INDICATOR);
    }
}

/**
 * 显示单文件传输进度弹窗
 * 包含：标题、进度条、文件大小、关闭/取消按钮
 */
void ui_show_progress(const char *filename, bool is_upload)
{
    lv_obj_t *parent = ui_get_window_parent();
    if (!parent) return;

    /* 清理已有弹窗 */
    if (prog_panel) { lv_obj_del(prog_panel); prog_panel = NULL; }

    prog_panel = lv_obj_create(parent);
    lv_obj_set_size(prog_panel, 320, 150);
    lv_obj_center(prog_panel);
    lv_obj_set_style_bg_color(prog_panel, lv_color_hex(0x222244), 0);
    lv_obj_set_style_border_color(prog_panel, lv_color_hex(0x4488cc), 0);
    lv_obj_set_style_border_width(prog_panel, 2, 0);
    lv_obj_set_style_radius(prog_panel, 10, 0);
    lv_obj_set_style_pad_all(prog_panel, 12, 0);

    /* 标题：方向 + 文件名 */
    prog_label = lv_label_create(prog_panel);
    char title[300];
    snprintf(title, sizeof(title), "%s: %s",
             is_upload ? "Uploading" : "Downloading", filename);
    lv_label_set_text(prog_label, title);
    lv_obj_set_style_text_color(prog_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(prog_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* 进度条 */
    prog_bar = lv_bar_create(prog_panel);
    lv_obj_set_size(prog_bar, 280, 20);
    lv_obj_align(prog_bar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_bar_set_range(prog_bar, 0, 100);
    lv_bar_set_value(prog_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(prog_bar, lv_color_hex(0x333355), 0);
    lv_obj_set_style_bg_color(prog_bar, lv_color_hex(0xCC0000), LV_PART_INDICATOR);
    lv_obj_set_style_anim_time(prog_bar, 200, 0);

    /* 文件大小信息 */
    prog_info = lv_label_create(prog_panel);
    lv_label_set_text(prog_info, "0 B / 0 B");
    lv_obj_set_style_text_color(prog_info, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(prog_info, LV_ALIGN_BOTTOM_MID, 0, -4);

    /* 关闭按钮（仅隐藏，传输继续） */
    lv_obj_t *close_btn = lv_button_create(prog_panel);
    lv_obj_set_size(close_btn, 70, 26);
    lv_obj_align(close_btn, LV_ALIGN_BOTTOM_LEFT, 8, -4);
    lv_obj_t *cbl_close = lv_label_create(close_btn);
    lv_label_set_text(cbl_close, "Close");
    lv_obj_center(cbl_close);
    lv_obj_add_event_cb(close_btn, on_close_progress_btn_clicked, LV_EVENT_CLICKED, NULL);

    /* 取消按钮（停止传输 + 隐藏弹窗） */
    lv_obj_t *cancel_btn = lv_button_create(prog_panel);
    lv_obj_set_size(cancel_btn, 70, 26);
    lv_obj_align(cancel_btn, LV_ALIGN_BOTTOM_RIGHT, -8, -4);
    lv_obj_t *cbl = lv_label_create(cancel_btn);
    lv_label_set_text(cbl, "Cancel");
    lv_obj_center(cbl);
    lv_obj_add_event_cb(cancel_btn, on_cancel_btn_clicked, LV_EVENT_CLICKED, NULL);
}

/** 隐藏并销毁所有进度弹窗 */
void ui_hide_progress(void)
{
    if (batch_prog_panel) {
        lv_obj_del(batch_prog_panel);
        batch_prog_panel = NULL;
        memset(prog_slots, 0, sizeof(prog_slots));
        batch_prog_count = 0;
    }
    if (prog_panel) {
        lv_obj_del(prog_panel);
        prog_panel = NULL;
    }
    prog_label = NULL;
    prog_bar   = NULL;
    prog_info  = NULL;
}

/**
 * 更新进度条显示（由网络线程通过 lv_async_call 调用）
 * 同时更新进度条、详情信息和标题
 */
void ui_update_progress(int percent, int current_bytes, int total_bytes,
                        const char *filename, bool is_upload)
{
    /* 更新进度条 */
    if (prog_bar) lv_bar_set_value(prog_bar, percent, LV_ANIM_ON);

    /* 更新文件大小信息 */
    if (prog_info) {
        char buf[128];
        if (total_bytes >= 1048576)
            snprintf(buf, sizeof(buf), "%.1f MB / %.1f MB",
                     current_bytes / 1048576.0f, total_bytes / 1048576.0f);
        else if (total_bytes >= 1024)
            snprintf(buf, sizeof(buf), "%.1f KB / %.1f KB",
                     current_bytes / 1024.0f, total_bytes / 1024.0f);
        else
            snprintf(buf, sizeof(buf), "%d B / %d B",
                     current_bytes, total_bytes);
        lv_label_set_text(prog_info, buf);
    }

    /* 更新标题 */
    if (prog_label && filename) {
        char title[300];
        snprintf(title, sizeof(title), "%s: %s  %d%%",
                 is_upload ? "Uploading" : "Downloading", filename, percent);
        lv_label_set_text(prog_label, title);
    }
}

/* ================================================================== */
/*  状态栏和错误提示辅助函数                                              */
/* ================================================================== */

/** 设置状态栏文字（绿色，表示成功信息） */
void ui_set_status(const char *msg)
{
    if (main_status_bar) {
        lv_label_set_text(main_status_bar, msg);
        lv_obj_set_style_text_color(main_status_bar, lv_color_hex(0x88ff88), 0);
    }
}

/**
 * 显示错误信息
 * 优先显示在主界面的状态栏；如果还在登录界面则显示在登录状态标签
 */
void ui_show_error(const char *msg)
{
    if (main_status_bar) {
        lv_label_set_text(main_status_bar, msg);
        lv_obj_set_style_text_color(main_status_bar, lv_color_hex(0xff6666), 0);
    } else if (login_status) {
        lv_label_set_text(login_status, msg);
        lv_obj_set_style_text_color(login_status, lv_color_hex(0xff6666), 0);
    }

    /* 如果在登录界面，重新启用登录按钮 */
    if (login_btn && lv_scr_act() == login_screen) {
        lv_obj_clear_state(login_btn, LV_STATE_DISABLED);
    }
}

/* ================================================================== */
/*  事件处理 —— 远程文件列表点击                                         */
/* ================================================================== */

/**
 * 远程文件列表项点击处理
 * 支持三种操作：
 *   1. ".." → 返回上级目录
 *   2. 目录（以'/'结尾）→ 进入子目录
 *   3. 普通文件 → 切换选中状态
 */
static void on_file_item_clicked(lv_event_t *e)
{
    /* 如果刚发生了长按，抑制本次点击事件 */
    if (g_remote_long_pressed) {
        g_remote_long_pressed = false;
        return;
    }

    lv_obj_t *btn = lv_event_get_target(e);
    if (!btn || !main_file_list) return;

    const char *text = lv_list_get_button_text(main_file_list, btn);
    if (!text) return;

    size_t tlen = strlen(text);

    /* ".." 条目：返回上级目录 */
    if (strcmp(text, "..") == 0) {
        char *slash = strrchr(g_remote_cur_path, '/');
        if (slash) *slash = '\0';                        /* 去掉最后一级 */
        else g_remote_cur_path[0] = '\0';                /* 已在根目录 */
        network_cmd_ls(g_remote_cur_path[0] ? g_remote_cur_path : NULL);
        return;
    }

    /* 目录（以'/'结尾）：进入子目录 */
    if (tlen > 0 && text[tlen - 1] == '/') {
        char dirname[256];
        strncpy(dirname, text, sizeof(dirname) - 1);
        dirname[sizeof(dirname) - 1] = '\0';
        if (tlen < sizeof(dirname)) dirname[tlen - 1] = '\0';  /* 去掉末尾'/' */

        if (g_remote_cur_path[0]) {
            char new_path[256];
            snprintf(new_path, sizeof(new_path), "%s/%s",
                     g_remote_cur_path, dirname);
            strncpy(g_remote_cur_path, new_path, sizeof(g_remote_cur_path) - 1);
        } else
            strncpy(g_remote_cur_path, dirname, sizeof(g_remote_cur_path) - 1);

        fprintf(stderr, "[remote click] folder '%s' -> cur_path='%s'\n",
                text, g_remote_cur_path);
        network_cmd_ls(g_remote_cur_path);               /* 请求子目录列表 */
        return;
    }

    /* 普通文件：切换选中状态（存储完整路径） */
    char full_path[256];
    if (g_remote_cur_path[0])
        snprintf(full_path, sizeof(full_path), "%s/%s", g_remote_cur_path, text);
    else
        strncpy(full_path, text, sizeof(full_path) - 1);

    /* 检查是否已选中 → 取消选中 */
    for (int i = 0; i < g_remote_sel_count; i++) {
        if (strcmp(g_selected_remote[i], full_path) == 0) {
            lv_obj_remove_style(btn, &style_selected_remote, 0);  /* 移除高亮 */
            for (int j = i; j < g_remote_sel_count - 1; j++)
                strncpy(g_selected_remote[j], g_selected_remote[j + 1],
                        sizeof(g_selected_remote[j]) - 1);
            g_remote_sel_count--;
            goto update_label;
        }
    }

    /* 未选中 → 选中 */
    if (g_remote_sel_count >= MAX_SELECTED_FILES) return;
    lv_obj_add_style(btn, &style_selected_remote, 0);     /* 添加绿色高亮 */
    strncpy(g_selected_remote[g_remote_sel_count], full_path,
            sizeof(g_selected_remote[g_remote_sel_count]) - 1);
    g_remote_sel_count++;

update_label:
    /* 更新底部选中计数 */
    if (main_selected_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Remote: %d | Local: %d",
                 g_remote_sel_count, g_local_sel_count);
        lv_label_set_text(main_selected_label, buf);
    }
}

/* ================================================================== */
/*  事件处理 —— 刷新按钮                                                 */
/* ================================================================== */

/** 刷新按钮：重置路径，重新请求远程文件列表和扫描本地文件 */
static void on_refresh_btn_clicked(lv_event_t *e)
{
    (void)e;
    ui_set_status("Refreshing...");
    g_local_cur_path[0]  = '\0';                         /* 重置本地路径到根目录 */
    g_remote_cur_path[0] = '\0';                         /* 重置远程路径到根目录 */
    network_cmd_ls(NULL);                                /* 请求远程根目录列表  */
    ui_refresh_local_files();                             /* 扫描本地目录        */
    /* 立即恢复状态栏显示 */
    if (g_login_ok) {
        char sb[256];
        snprintf(sb, sizeof(sb), "User: %s  |  %s  |  Connected",
                 g_login_user[0] ? g_login_user : "admin",
                 g_session_info[0] ? g_session_info : "N/A");
        ui_set_status(sb);
    }
}

/* ================================================================== */
/*  事件处理 —— 下载按钮                                                 */
/* ================================================================== */

/** 下载按钮：收集所有选中的远程文件，调用批量下载 */
static void on_download_btn_clicked(lv_event_t *e)
{
    (void)e;
    if (g_remote_sel_count == 0) {
        ui_show_error("No file selected");
        return;
    }

    const char *files[MAX_SELECTED_FILES];
    for (int i = 0; i < g_remote_sel_count; i++)
        files[i] = g_selected_remote[i];

    ui_set_status("Downloading...");
    if (!network_cmd_get_multi(files, g_remote_sel_count))
        ui_show_error("Failed to start download");
}

/* ================================================================== */
/*  事件处理 —— 上传按钮                                                 */
/* ================================================================== */

/** 上传按钮：收集所有选中的本地文件，调用批量上传 */
static void on_upload_btn_clicked(lv_event_t *e)
{
    (void)e;
    if (g_local_sel_count == 0) {
        ui_show_error("No file selected");
        return;
    }

    const char *files[MAX_SELECTED_FILES];
    for (int i = 0; i < g_local_sel_count; i++)
        files[i] = g_selected_local[i];

    ui_set_status("Uploading...");
    if (!network_cmd_put_multi(files, g_local_sel_count))
        ui_show_error("No valid files to upload");
}

/* ================================================================== */
/*  删除按钮 —— 仅支持删除本地文件                                       */
/* ================================================================== */

/**
 * 删除按钮点击处理
 * 优先级：
 *   1. 远程文件被选中 → 阻止并显示错误弹窗
 *   2. 未选中本地文件  → 状态栏提示
 *   3. 正常 → 显示确认弹窗
 */
static void on_delete_btn_clicked(lv_event_t *e)
{
    (void)e;

    /* 优先1：远程文件选中 → 错误弹窗 */
    if (g_remote_sel_count > 0) {
        ui_show_error_popup("error delete");
        return;
    }

    /* 优先2：无本地文件选中 → 状态栏提示 */
    if (g_local_sel_count == 0) {
        ui_show_error("No file selected");
        return;
    }

    /* 优先3：显示确认弹窗 */
    ui_show_confirm_popup();
}

/* ================================================================== */
/*  删除确认弹窗                                                         */
/* ================================================================== */

/** 显示删除确认弹窗（"确认删除？"，Yes/No 按钮） */
static void ui_show_confirm_popup(void)
{
    lv_obj_t *parent = ui_get_window_parent();
    if (!parent) return;

    /* 单例模式：防止重复创建 */
    if (confirm_popup && lv_obj_is_valid(confirm_popup))
        return;

    /* 关闭任何已打开的错误弹窗，避免堆叠 */
    if (err_popup && lv_obj_is_valid(err_popup)) {
        lv_obj_del(err_popup);
        err_popup = NULL;
        err_label = NULL;
    }

    confirm_popup = lv_obj_create(parent);
    lv_obj_set_size(confirm_popup, 280, 150);
    lv_obj_center(confirm_popup);
    lv_obj_set_style_bg_color(confirm_popup, lv_color_hex(0x333355), 0);
    lv_obj_set_style_border_color(confirm_popup, lv_color_hex(0xffaa00), 0);  /* 橙色边框 */
    lv_obj_set_style_border_width(confirm_popup, 2, 0);
    lv_obj_set_style_radius(confirm_popup, 8, 0);
    lv_obj_set_style_pad_all(confirm_popup, 15, 0);
    lv_obj_set_flex_flow(confirm_popup, LV_FLEX_FLOW_COLUMN);    /* 列排列 */
    lv_obj_set_flex_align(confirm_popup,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    /* 提示文字 */
    lv_obj_t *lbl = lv_label_create(confirm_popup);
    lv_label_set_text(lbl, "Confirm delete?");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    /* 按钮行：Yes | No */
    lv_obj_t *btn_row = lv_obj_create(confirm_popup);
    lv_obj_set_size(btn_row, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row,
                          LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *yes_btn = lv_button_create(btn_row);
    lv_obj_set_size(yes_btn, 80, 28);
    lv_obj_t *yes_lbl = lv_label_create(yes_btn);
    lv_label_set_text(yes_lbl, "Yes");
    lv_obj_center(yes_lbl);
    lv_obj_add_event_cb(yes_btn, on_delete_confirm_yes_btn_clicked,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *no_btn = lv_button_create(btn_row);
    lv_obj_set_size(no_btn, 80, 28);
    lv_obj_t *no_lbl = lv_label_create(no_btn);
    lv_label_set_text(no_lbl, "No");
    lv_obj_center(no_lbl);
    lv_obj_add_event_cb(no_btn, on_delete_confirm_no_btn_clicked,
                        LV_EVENT_CLICKED, NULL);
}

/** 确认删除 Yes 按钮：执行删除并关闭弹窗 */
static void on_delete_confirm_yes_btn_clicked(lv_event_t *e)
{
    (void)e;
    execute_local_delete();
    if (confirm_popup) { lv_obj_del(confirm_popup); confirm_popup = NULL; }
}

/** 确认删除 No 按钮：仅关闭弹窗，保持选中状态不变 */
static void on_delete_confirm_no_btn_clicked(lv_event_t *e)
{
    (void)e;
    if (confirm_popup) { lv_obj_del(confirm_popup); confirm_popup = NULL; }
}

/**
 * 递归删除文件或目录
 * 如果是目录则先递归删除所有子文件和子目录
 *
 * @param path  要删除的路径
 * @return      0=成功, -1=失败
 */
static int remove_recursive(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;                 /* 文件/目录不存在 */

    if (S_ISDIR(st.st_mode)) {
        /* 目录：遍历并递归删除所有内容 */
        DIR *dir = opendir(path);
        if (!dir) return -1;
        struct dirent *d;
        while ((d = readdir(dir)) != NULL) {
            if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
                continue;                                /* 跳过 . 和 .. */
            char child[520];
            snprintf(child, sizeof(child), "%s/%s", path, d->d_name);
            remove_recursive(child);                      /* 递归删除子项 */
        }
        closedir(dir);
        return rmdir(path);                              /* 删除空目录 */
    }
    return unlink(path);                                 /* 删除普通文件 */
}

/* ================================================================== */
/*  执行本地文件删除                                                     */
/* ================================================================== */

/**
 * 删除所有选中的本地文件
 * 使用规范的路径前缀 "./client/"
 * 删除后清空选中状态并刷新本地文件列表
 */
static void execute_local_delete(void)
{
    int success = 0, fail = 0;

    for (int i = 0; i < g_local_sel_count; i++) {
        char *name = g_selected_local[i];
        /* g_selected_local[i] 已包含子目录前缀（如 "load/haha.txt"），
         * 只需在前面加上 "./client/" 即可 */
        char path[520];
        snprintf(path, sizeof(path), "./client/%s", name);
        /* 去掉末尾的 '/'（remove_recursive 需要） */
        size_t plen = strlen(path);
        if (plen > 0 && path[plen - 1] == '/') path[plen - 1] = '\0';

        if (remove_recursive(path) == 0)
            success++;
        else
            fail++;
    }

    /* 刷新前清空选中状态（避免悬空指针） */
    g_local_sel_count = 0;
    memset(g_selected_local, 0, sizeof(g_selected_local));

    /* 更新底部选中计数 */
    if (main_selected_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Remote: %d | Local: %d",
                 g_remote_sel_count, g_local_sel_count);
        lv_label_set_text(main_selected_label, buf);
    }

    ui_refresh_local_files();                            /* 刷新列表 */

    if (fail == 0)
        ui_set_status("Deleted");
    else
        ui_show_error("Some files failed to delete");
}

/** 断开连接按钮 */
static void on_disconnect_btn_clicked(lv_event_t *e)
{
    (void)e;
    ui_set_status("Disconnecting...");
    network_disconnect();
}

/* ================================================================== */
/*  错误弹窗                                                             */
/* ================================================================== */

/** 关闭错误弹窗按钮点击处理 */
static void on_close_error_popup_btn_clicked(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *popup = lv_obj_get_parent(btn);
    lv_obj_del(popup);
    err_popup = NULL;
    err_label = NULL;
}

/**
 * 显示错误弹窗（模态）
 * 如果已存在错误弹窗则更新文字；否则创建新的
 * 包含错误信息和关闭按钮
 */
void ui_show_error_popup(const char *msg)
{
    lv_obj_t *parent = ui_get_window_parent();
    if (!parent) return;

    /* 如果已有弹窗且仍有效，只更新文字 */
    if (err_popup && lv_obj_is_valid(err_popup)) {
        if (err_label) lv_label_set_text(err_label, msg);
        return;
    }

    /* 关闭任何已打开的确认弹窗，避免堆叠 */
    if (confirm_popup && lv_obj_is_valid(confirm_popup)) {
        lv_obj_del(confirm_popup);
        confirm_popup = NULL;
    }

    /* 如果进度弹窗可见，先隐藏（避免堆叠） */
    if (batch_prog_panel && !lv_obj_has_flag(batch_prog_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(batch_prog_panel, LV_OBJ_FLAG_HIDDEN);
    }

    err_popup = lv_obj_create(parent);
    lv_obj_set_size(err_popup, 280, 150);
    lv_obj_center(err_popup);
    lv_obj_set_style_bg_color(err_popup, lv_color_hex(0x333355), 0);
    lv_obj_set_style_border_color(err_popup, lv_color_hex(0xff4444), 0);  /* 红色边框 */
    lv_obj_set_style_border_width(err_popup, 2, 0);
    lv_obj_set_style_radius(err_popup, 8, 0);
    lv_obj_set_style_pad_all(err_popup, 15, 0);

    /* 使用 Flex 列布局避免标签和按钮重叠 */
    lv_obj_set_flex_flow(err_popup, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(err_popup, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    err_label = lv_label_create(err_popup);
    lv_label_set_text(err_label, msg);
    lv_obj_set_style_text_color(err_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(err_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(err_label, LV_LABEL_LONG_WRAP);   /* 允许自动换行 */
    lv_obj_set_width(err_label, 240);                         /* 限制宽度以触发换行 */

    lv_obj_t *close_btn = lv_button_create(err_popup);
    lv_obj_set_size(close_btn, 80, 28);
    lv_obj_t *cbl = lv_label_create(close_btn);
    lv_label_set_text(cbl, "Close");
    lv_obj_center(cbl);
    lv_obj_add_event_cb(close_btn, on_close_error_popup_btn_clicked, LV_EVENT_CLICKED, NULL);
}

/** 关闭进度弹窗按钮（仅隐藏弹窗，传输继续在后台进行） */
static void on_close_progress_btn_clicked(lv_event_t *e)
{
    (void)e;
    if (batch_prog_panel) {
        lv_obj_add_flag(batch_prog_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (prog_panel) {
        lv_obj_add_flag(prog_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

/** 取消传输按钮（停止传输 + 隐藏弹窗 + 显示提示） */
static void on_cancel_btn_clicked(lv_event_t *e)
{
    (void)e;
    network_cancel_transfer();
    ui_hide_progress();
    ui_show_error("Transfer cancelled");
}

/* ================================================================== */
/*  屏幕键盘管理                                                         */
/* ================================================================== */

/**
 * 文本框获得焦点或点击时显示屏幕键盘
 * 键盘作为顶层图层的子组件创建，多个输入框共享
 */
static void on_ta_focused(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!ta) return;

    if (!kb) {
        /* 在顶层图层创建键盘（确保在所有界面之上） */
        lv_obj_t *parent = lv_layer_top();
        kb = lv_keyboard_create(parent);
        lv_obj_add_event_cb(kb, on_keyboard_event, LV_EVENT_ALL, NULL);
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);         /* 初始隐藏 */
    }

    /* 将键盘绑定到此文本框并显示 */
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_clear_flag(kb, LV_OBJ_FLAG_HIDDEN);
}

/**
 * 键盘事件：取消/就绪时隐藏键盘并解绑文本框
 */
static void on_keyboard_event(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CANCEL || code == LV_EVENT_READY) {
        if (kb) {
            lv_keyboard_set_textarea(kb, NULL);          /* 解绑文本框 */
            lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);      /* 隐藏键盘 */
        }
    }
}

/* ================================================================== */
/*  异步回调 —— 更新文件列表（由网络线程通过 lv_async_call 调用）        */
/* ================================================================== */

/**
 * 更新远程文件列表的回调
 * 解析换行分隔的文件名列表，为每个条目创建 LVGL 按钮
 * 自动跳过 "." 和 ".." 条目（由本地扫描添加）
 *
 * @param data  指向换行分隔文件名字符串（此函数负责 free）
 */
void ui_update_file_list_cb(void *data)
{
    char *filelist = (char *)data;
    if (!main_file_list) { free(filelist); return; }

    /* 清空旧列表项 */
    lv_obj_clean(main_file_list);
    g_remote_displayed_count = 0;

    /* 在子目录中显示路径头和 ".." 返回按钮 */
    if (g_remote_cur_path[0]) {
        char hdr[320];
        snprintf(hdr, sizeof(hdr), "Path: %s/", g_remote_cur_path);
        lv_list_add_text(main_file_list, hdr);
        lv_obj_t *back_btn = lv_list_add_button(main_file_list, NULL, "..");
        lv_obj_add_event_cb(back_btn, on_file_item_clicked,
                             LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(back_btn, on_remote_file_item_long_pressed,
                             LV_EVENT_LONG_PRESSED, NULL);
    }

    if (!filelist || strlen(filelist) == 0) {
        lv_list_add_text(main_file_list, "(empty directory)");
        free(filelist);
        return;
    }

    /* 解析换行分隔的条目并创建按钮 */
    char *save;
    char *token = strtok_r(filelist, "\n", &save);
    while (token) {
        /* 去除末尾空白和回车 */
        size_t tlen = strlen(token);
        while (tlen > 0 && (token[tlen-1] == '\r' || token[tlen-1] == ' '))
            token[--tlen] = '\0';
        if (tlen == 0) { token = strtok_r(NULL, "\n", &save); continue; }

        /* 跳过服务器返回的 "." 和 ".." 条目 */
        if (strcmp(token, ".") == 0 || strcmp(token, "..") == 0) {
            token = strtok_r(NULL, "\n", &save);
            continue;
        }

        lv_obj_t *btn = lv_list_add_button(main_file_list, NULL, token);
        lv_obj_add_event_cb(btn, on_file_item_clicked, LV_EVENT_CLICKED,
                             NULL);
        lv_obj_add_event_cb(btn, on_remote_file_item_long_pressed,
                             LV_EVENT_LONG_PRESSED, NULL);
        /* 将条目写入镜像数组（供网络层查询） */
        if (g_remote_displayed_count < MAX_SELECTED_FILES) {
            strncpy(g_remote_displayed_files[g_remote_displayed_count],
                    token, sizeof(g_remote_displayed_files[0]) - 1);
            g_remote_displayed_files[g_remote_displayed_count][sizeof(g_remote_displayed_files[0]) - 1] = '\0';
            g_remote_displayed_count++;
        }
        token = strtok_r(NULL, "\n", &save);
    }

    free(filelist);
    /* 清空远程选中状态（列表已刷新） */
    g_remote_sel_count = 0;
    memset(g_selected_remote, 0, sizeof(g_selected_remote));
}

/* ================================================================== */
/*  本地文件操作辅助函数                                                  */
/* ================================================================== */

/**
 * 扫描本地目录并返回换行分隔的文件名列表
 * 目录条目会以 '/' 后缀标记
 * 在子目录中会自动添加 ".." 条目
 *
 * @param subpath  相对于 ./client/ 的子路径（NULL 表示根目录）
 * @return         malloc 分配的文件名列表字符串（调用者负责 free），
 *                 失败返回 NULL
 */
static char *scan_local_directory(const char *subpath)
{
    char *buf = (char *)malloc(SIZE);
    if (!buf) return NULL;
    int off = 0;
    buf[0] = '\0';

    char full[520];
    if (subpath && subpath[0])
        snprintf(full, sizeof(full), "./client/%s", subpath);
    else
        snprintf(full, sizeof(full), "./client");

    DIR *dir = opendir(full);
    fprintf(stderr, "[local] opendir('%s') = %p", full, (void *)dir);
    if (!dir)
        fprintf(stderr, "  [FAIL errno=%d: %s]", errno, strerror(errno));
    fprintf(stderr, "\n");
    if (!dir) {
        free(buf);
        return NULL;                                     /* 目录打开失败 */
    }

    /* 子目录中显示 ".." 返回条目 */
    if (subpath && subpath[0])
        off += snprintf(buf + off, (size_t)(SIZE - off - 1), "..\n");

    struct dirent *d;
    while ((d = readdir(dir)) != NULL) {
        if (strcmp(d->d_name, ".") == 0 || strcmp(d->d_name, "..") == 0)
            continue;                                    /* 跳过 . 和 .. */

        char item_path[560];
        snprintf(item_path, sizeof(item_path), "%s/%s", full, d->d_name);
        struct stat st;
        if (stat(item_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            /* 目录：以 '/' 后缀显示 */
            off += snprintf(buf + off, (size_t)(SIZE - off - 1),
                            "%s/\n", d->d_name);
        } else {
            /* 文件：正常显示 */
            off += snprintf(buf + off, (size_t)(SIZE - off - 1),
                            "%s\n", d->d_name);
        }
    }
    closedir(dir);
    return buf;
}

/** 刷新本地文件列表并重置到根目录 */
void ui_refresh_local_files_root(void)
{
    g_local_cur_path[0] = '\0';
    ui_refresh_local_files();
}

/** 刷新远程文件列表并重置到根目录 */
void ui_refresh_remote_list_root(void)
{
    g_remote_cur_path[0] = '\0';
    network_cmd_ls(NULL);
}

/** 刷新本地文件列表（通过 lv_async_call 确保在 UI 线程执行） */
void ui_refresh_local_files(void)
{
    char *filelist = scan_local_directory(g_local_cur_path);
    lv_async_call(ui_update_local_file_list_cb, filelist);  /* NULL 在回调中处理 */
}

/**
 * 更新本地文件列表的回调
 * 解析扫描结果并填充 LVGL 列表组件
 * 如果 filelist 为 NULL（目录打开失败），显示错误并回退到父目录
 */
void ui_update_local_file_list_cb(void *data)
{
    char *filelist = (char *)data;
    if (!main_local_list) { free(filelist); return; }

    lv_obj_clean(main_local_list);                       /* 清空旧列表 */

    /* 处理 NULL（目录打开失败）→ 显示错误并返回父目录 */
    if (!filelist) {
        lv_list_add_text(main_local_list, "(unable to open)");
        ui_show_error_popup("Unable file");
        /* 回退到父目录 */
        char *slash = strrchr(g_local_cur_path, '/');
        if (slash) *slash = '\0';
        else g_local_cur_path[0] = '\0';
        ui_refresh_local_files();
        return;
    }

    /* 显示当前路径头 */
    {
        char header[320];
        snprintf(header, sizeof(header), "Local: client/%s",
                 g_local_cur_path[0] ? g_local_cur_path : "");
        lv_list_add_text(main_local_list, header);
    }

    if (!filelist || strlen(filelist) == 0) {
        lv_list_add_text(main_local_list, "(empty directory)");
        free(filelist);
        return;
    }

    char *save;
    g_local_displayed_count = 0;
    char *token = strtok_r(filelist, "\n", &save);
    while (token) {
        /* 去除末尾空白 */
        size_t tlen = strlen(token);
        while (tlen > 0 && (token[tlen-1] == '\r' || token[tlen-1] == ' '))
            token[--tlen] = '\0';
        if (tlen == 0) { token = strtok_r(NULL, "\n", &save); continue; }

        /* 存储所有条目到镜像数组（包括以 '/' 结尾的目录） */
        if (g_local_displayed_count < MAX_SELECTED_FILES) {
            strncpy(g_local_displayed_files[g_local_displayed_count],
                    token, sizeof(g_local_displayed_files[0]) - 1);
            g_local_displayed_files[g_local_displayed_count][sizeof(g_local_displayed_files[0]) - 1] = '\0';
            g_local_displayed_count++;
        }

        lv_obj_t *btn = lv_list_add_button(main_local_list, NULL, token);
        lv_obj_add_event_cb(btn, on_local_file_item_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(btn, on_local_file_item_long_pressed, LV_EVENT_LONG_PRESSED, NULL);
        token = strtok_r(NULL, "\n", &save);
    }
    free(filelist);
    /* 清空本地选中状态 */
    g_local_sel_count = 0;
    memset(g_selected_local, 0, sizeof(g_selected_local));
    if (main_selected_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Remote: %d | Local: %d",
                 g_remote_sel_count, g_local_sel_count);
        lv_label_set_text(main_selected_label, buf);
    }
}

/* ================================================================== */
/*  长按事件处理（多选）                                                  */
/* ================================================================== */

/**
 * 本地文件列表项长按处理
 * 长按用于切换多选状态（与单击区分）
 * 设置 g_local_long_pressed 标志以抑制后续的单击事件
 */
static void on_local_file_item_long_pressed(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (!btn || !main_local_list) return;

    const char *text = lv_list_get_button_text(main_local_list, btn);
    if (!text) return;

    size_t tlen = strlen(text);

    /* ".." 条目：忽略长按 */
    if (strcmp(text, "..") == 0) return;

    /* 构建完整相对路径（用于比较和存储） */
    char full_path[256];
    if (g_local_cur_path[0])
        snprintf(full_path, sizeof(full_path), "%s/%s", g_local_cur_path, text);
    else
        strncpy(full_path, text, sizeof(full_path) - 1);

    /* 切换选中状态 */
    for (int i = 0; i < g_local_sel_count; i++) {
        if (strcmp(g_selected_local[i], full_path) == 0) {
            /* 已选中 → 取消选中 */
            lv_obj_remove_style(btn, &style_selected_local, 0);
            for (int j = i; j < g_local_sel_count - 1; j++) {
                strncpy(g_selected_local[j], g_selected_local[j + 1],
                        sizeof(g_selected_local[j]) - 1);
            }
            g_local_sel_count--;
            g_local_long_pressed = true;                 /* 抑制后续 CLICKED 事件 */
            goto update_label;
        }
    }

    /* 未选中 → 选中 */
    if (g_local_sel_count >= MAX_SELECTED_FILES) return;

    lv_obj_add_style(btn, &style_selected_local, 0);
    strncpy(g_selected_local[g_local_sel_count], full_path,
            sizeof(g_selected_local[g_local_sel_count]) - 1);
    g_local_sel_count++;
    g_local_long_pressed = true;                         /* 抑制后续 CLICKED 事件 */

update_label:
    if (main_selected_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Remote: %d | Local: %d",
                 g_remote_sel_count, g_local_sel_count);
        lv_label_set_text(main_selected_label, buf);
    }
}

/**
 * 远程文件列表项长按处理
 * 逻辑与本地长按相同，但操作远程文件选中状态
 */
static void on_remote_file_item_long_pressed(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    if (!btn || !main_file_list) return;

    const char *text = lv_list_get_button_text(main_file_list, btn);
    if (!text) return;

    /* "." 或 ".." 条目：忽略 */
    if (strcmp(text, ".") == 0 || strcmp(text, "..") == 0) return;

    /* 构建完整相对路径（含远程当前路径前缀） */
    char full_path[256];
    size_t tlen = strlen(text);
    if (g_remote_cur_path[0]) {
        if (tlen > 0 && text[tlen - 1] == '/')
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     g_remote_cur_path, text);
        else
            snprintf(full_path, sizeof(full_path), "%s/%s",
                     g_remote_cur_path, text);
    } else {
        strncpy(full_path, text, sizeof(full_path) - 1);
    }

    /* 使用完整路径切换选中状态 */
    for (int i = 0; i < g_remote_sel_count; i++) {
        if (strcmp(g_selected_remote[i], full_path) == 0) {
            lv_obj_remove_style(btn, &style_selected_remote, 0);
            for (int j = i; j < g_remote_sel_count - 1; j++) {
                strncpy(g_selected_remote[j], g_selected_remote[j + 1],
                        sizeof(g_selected_remote[j]) - 1);
            }
            g_remote_sel_count--;
            g_remote_long_pressed = true;                /* 抑制后续 CLICKED */
            goto update_label;
        }
    }

    /* 未选中 → 选中 */
    if (g_remote_sel_count >= MAX_SELECTED_FILES) return;

    lv_obj_add_style(btn, &style_selected_remote, 0);
    strncpy(g_selected_remote[g_remote_sel_count], full_path,
            sizeof(g_selected_remote[g_remote_sel_count]) - 1);
    g_remote_sel_count++;
    g_remote_long_pressed = true;                        /* 抑制后续 CLICKED */

update_label:
    if (main_selected_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Remote: %d | Local: %d",
                 g_remote_sel_count, g_local_sel_count);
        lv_label_set_text(main_selected_label, buf);
    }
}

/* ================================================================== */
/*  查询辅助函数：检查文件列表中是否已存在指定条目                         */
/*  这些函数由网络线程调用（只读访问，UI 线程是唯一的写入者）              */
/* ================================================================== */

/**
 * 检查远程文件列表镜像中是否已有指定名称
 * 同时检查精确匹配和添加 '/' 后缀的目录形式
 */
bool ui_remote_list_has_entry(const char *name)
{
    if (!name) return false;
    size_t nlen = strlen(name);
    for (int i = 0; i < g_remote_displayed_count; i++) {
        if (strcmp(g_remote_displayed_files[i], name) == 0) return true;
        /* 同时检查 name/ 形式（目录变体） */
        if (nlen > 0 && name[nlen - 1] != '/') {
            char name_with_slash[260];
            snprintf(name_with_slash, sizeof(name_with_slash), "%s/", name);
            if (strcmp(g_remote_displayed_files[i], name_with_slash) == 0) return true;
        }
    }
    return false;
}

/**
 * 检查本地文件列表镜像中是否已有指定名称
 */
bool ui_local_list_has_entry(const char *name)
{
    if (!name) return false;
    size_t nlen = strlen(name);
    for (int i = 0; i < g_local_displayed_count; i++) {
        if (strcmp(g_local_displayed_files[i], name) == 0) return true;
        /* 同时检查 name/ 形式（目录变体） */
        if (nlen > 0 && name[nlen - 1] != '/') {
            char name_with_slash[260];
            snprintf(name_with_slash, sizeof(name_with_slash), "%s/", name);
            if (strcmp(g_local_displayed_files[i], name_with_slash) == 0) return true;
        }
    }
    return false;
}

/**
 * 本地文件列表项点击处理
 * 支持：目录导航（".." 返回上级、目录进入子目录）、文件选中切换
 */
static void on_local_file_item_clicked(lv_event_t *e)
{
    /* 如果刚发生了长按，抑制本次点击 */
    if (g_local_long_pressed) {
        g_local_long_pressed = false;
        return;
    }

    lv_obj_t *btn = lv_event_get_target(e);
    if (!btn || !main_local_list) return;

    const char *text = lv_list_get_button_text(main_local_list, btn);
    if (!text) return;

    size_t tlen = strlen(text);

    /* ".." 条目：返回上级目录 */
    if (strcmp(text, "..") == 0) {
        char *slash = strrchr(g_local_cur_path, '/');
        if (slash) *slash = '\0';
        else g_local_cur_path[0] = '\0';
        ui_refresh_local_files();
        return;
    }

    /* 目录（以'/'结尾）：进入子目录 */
    if (tlen > 0 && text[tlen - 1] == '/') {
        char dirname[256];
        strncpy(dirname, text, sizeof(dirname) - 1);
        dirname[sizeof(dirname) - 1] = '\0';
        if (tlen < sizeof(dirname)) dirname[tlen - 1] = '\0';  /* 去掉末尾'/' */

        if (g_local_cur_path[0]) {
            char new_path[256];
            snprintf(new_path, sizeof(new_path), "%s/%s",
                     g_local_cur_path, dirname);
            strncpy(g_local_cur_path, new_path, sizeof(g_local_cur_path) - 1);
        } else
            strncpy(g_local_cur_path, dirname, sizeof(g_local_cur_path) - 1);

        fprintf(stderr, "[local click] folder '%s' -> cur_path='%s'\n",
                text, g_local_cur_path);
        ui_refresh_local_files();
        return;
    }

    /* 普通文件：切换选中状态 */

    /* 构建完整相对路径用于比较和存储
     * 保留子目录上下文（如 "load/haha.txt"） */
    char full_path[256];
    if (g_local_cur_path[0])
        snprintf(full_path, sizeof(full_path), "%s/%s", g_local_cur_path, text);
    else
        strncpy(full_path, text, sizeof(full_path) - 1);

    /* 检查是否已选中 → 取消选中 */
    for (int i = 0; i < g_local_sel_count; i++) {
        if (strcmp(g_selected_local[i], full_path) == 0) {
            lv_obj_remove_style(btn, &style_selected_local, 0);
            for (int j = i; j < g_local_sel_count - 1; j++) {
                strncpy(g_selected_local[j], g_selected_local[j + 1],
                        sizeof(g_selected_local[j]) - 1);
            }
            g_local_sel_count--;
            g_local_long_pressed = true;                 /* 抑制后续 CLICKED */
            goto update_label;
        }
    }

    /* 未选中 → 选中 */
    if (g_local_sel_count >= MAX_SELECTED_FILES) return;

    lv_obj_add_style(btn, &style_selected_local, 0);
    strncpy(g_selected_local[g_local_sel_count], full_path,
            sizeof(g_selected_local[g_local_sel_count]) - 1);
    g_local_sel_count++;

update_label:
    if (main_selected_label) {
        char buf[128];
        snprintf(buf, sizeof(buf), "Remote: %d | Local: %d",
                 g_remote_sel_count, g_local_sel_count);
        lv_label_set_text(main_selected_label, buf);
    }
}

/* ================================================================== */
/*  传输完成后恢复状态栏                                                  */
/* ================================================================== */

/** 定时器回调：延迟恢复状态栏为默认连接信息 */
static void restore_status_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);                                     /* 单次定时器，执行后删除 */
    if (g_login_ok) {
        char sb[256];
        snprintf(sb, sizeof(sb), "User: %s  |  %s  |  Connected",
                 g_login_user[0] ? g_login_user : "admin",
                 g_session_info[0] ? g_session_info : "N/A");
        ui_set_status(sb);
    }
}

/**
 * 在指定延迟后恢复状态栏
 * 创建一个单次定时器，在 3000ms 后将状态栏文字恢复为默认连接信息
 */
void ui_restore_status_after_delay(void)
{
    lv_timer_t *t = lv_timer_create(restore_status_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(t, 1);                     /* 仅执行一次 */
}

/** 清理所有界面资源 */
void ui_cleanup(void)
{
    if (login_screen) { lv_obj_del(login_screen); login_screen = NULL; }
    if (main_screen)  { lv_obj_del(main_screen);  main_screen  = NULL; }
    ui_hide_progress();
}

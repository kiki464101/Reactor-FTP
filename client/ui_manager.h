/**
 * @file ui_manager.h
 * @brief LVGL FTP 客户端 UI 管理器模块头文件
 *
 * 本模块管理三个界面的所有 LVGL 组件：
 * - 登录界面（login screen）：IP/端口/用户名/密码输入
 * - 主界面（main screen）：远程/本地文件列表，支持多选
 * - 进度弹窗（progress overlay）：显示传输进度条
 *
 * 线程安全设计：
 *   所有 LVGL 组件操作必须在 UI 线程中执行；
 *   网络线程通过 lv_async_call() 将数据推送到 UI 线程。
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "../lvgl/lvgl.h"

/* ------------------------------------------------------------------ */
/*  常量定义                                                            */
/* ------------------------------------------------------------------ */
#define MAX_SELECTED_FILES  128     /* 最大可选择文件数（与 network_task.h 保持一致） */

/* ------------------------------------------------------------------ */
/*  界面对象引用（extern 声明）                                          */
/* ------------------------------------------------------------------ */
extern lv_obj_t *login_screen;      /* 登录界面对象                     */
extern lv_obj_t *main_screen;       /* 主界面对象（文件管理器）          */

/* ------------------------------------------------------------------ */
/*  多选状态（extern 声明）                                              */
/*  存储用户当前选中的远程/本地文件路径列表                               */
/* ------------------------------------------------------------------ */
extern char g_selected_remote[MAX_SELECTED_FILES][256];  /* 远程选中文件路径数组 */
extern int  g_remote_sel_count;                           /* 远程选中文件数量    */
extern char g_selected_local[MAX_SELECTED_FILES][256];    /* 本地选中文件路径数组 */
extern int  g_local_sel_count;                            /* 本地选中文件数量    */

/* ================================================================== */
/*  初始化函数（在 main() 中调用一次）                                   */
/* ================================================================== */

/** 初始化并显示登录界面 */
void ui_login_init(void);

/** 初始化主界面（文件管理器），但不自动切换到此界面 */
void ui_main_init(void);

/* ================================================================== */
/*  界面切换                                                            */
/* ================================================================== */

/** 切换到登录界面（通常在断开连接时调用） */
void ui_switch_to_login(void);

/** 切换到主界面（登录成功后调用），同时触发 LS 命令获取文件列表 */
void ui_switch_to_main(void);

/* ================================================================== */
/*  进度弹窗管理                                                        */
/* ================================================================== */

/**
 * 显示单文件传输进度弹窗
 * @param filename   正在传输的文件名
 * @param is_upload  true=上传, false=下载
 */
void ui_show_progress(const char *filename, bool is_upload);

/** 显示批量传输进度面板（包含多个进度条） */
void ui_show_progress_batch(void);

/** 隐藏/销毁所有进度弹窗 */
void ui_hide_progress(void);

/**
 * 更新进度条显示
 * @param percent        完成百分比 (0-100)
 * @param current_bytes  已传输字节数
 * @param total_bytes    总字节数
 * @param filename       文件名
 * @param is_upload      true=上传, false=下载
 */
void ui_update_progress(int percent, int current_bytes, int total_bytes,
                        const char *filename, bool is_upload);

/**
 * 更新批量传输面板中某个文件的进度
 * 参数含义同上
 */
void ui_update_transfer_progress(const char *filename, int percent,
                                  int current_bytes, int total_bytes,
                                  bool is_upload);

/**
 * 单个传输任务完成回调
 * 在批量传输面板中更新对应文件的状态（成功/失败）
 */
void ui_on_transfer_done(const char *filename, bool success, bool is_upload);

/* ================================================================== */
/*  状态栏和错误提示                                                     */
/* ================================================================== */

/** 设置主界面状态栏文字（成功信息，绿色） */
void ui_set_status(const char *msg);

/** 显示错误信息（优先显示在状态栏，登录界面则在登录状态标签显示） */
void ui_show_error(const char *msg);

/* ================================================================== */
/*  异步回调函数（由网络线程通过 lv_async_call() 调用）                  */
/*  注意：这些函数在 UI 线程的上下文中执行，可以安全操作 LVGL 组件         */
/* ================================================================== */

/**
 * 更新远程文件列表的回调
 * @param data  指向以换行符分隔的文件名字符串（由调用者 malloc，回调内 free）
 */
void ui_update_file_list_cb(void *data);

/**
 * 更新本地文件列表的回调
 * @param data  指向以换行符分隔的文件名字符串
 */
void ui_update_local_file_list_cb(void *data);

/** 刷新本地文件列表（扫描 ./client/ 目录） */
void ui_refresh_local_files(void);

/** 刷新本地文件列表并重置到根目录 */
void ui_refresh_local_files_root(void);

/** 刷新远程文件列表并重置到根目录 */
void ui_refresh_remote_list_root(void);

/* ================================================================== */
/*  查询辅助函数                                                        */
/* ================================================================== */

/**
 * 检查远程文件列表中是否已存在指定条目
 * @param name  要查询的文件名或目录名
 * @return      true=存在, false=不存在
 */
bool ui_remote_list_has_entry(const char *name);

/**
 * 检查本地文件列表中是否已存在指定条目
 * @param name  要查询的文件名或目录名
 * @return      true=存在, false=不存在
 */
bool ui_local_list_has_entry(const char *name);

/** 在指定延迟后恢复状态栏为默认连接信息 */
void ui_restore_status_after_delay(void);

/**
 * 显示错误弹窗（模态对话框）
 * 如果已有错误弹窗，则更新其文字内容；否则创建新的
 * @param msg  错误提示信息
 */
void ui_show_error_popup(const char *msg);

#endif /* UI_MANAGER_H */

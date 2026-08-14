# Embedded Remote File Manager

基于 Reactor 架构的高并发 FTP 服务器及可视化客户端。采用 Reactor（epoll）+ 线程池（pthread）架构，支持文件夹递归传输、多选操作、实时进度显示。

## 项目架构

```
┌─────────────────────────────────────────────────────────┐
│                    客户端 (LVGL + SDL)                    │
│  ┌──────────┐  ┌──────────────┐  ┌────────────────────┐ │
│  │ UI 线程   │  │ 网络主线程    │  │ 传输线程池(3线程)   │ │
│  │ LVGL渲染  │←→│ 命令通道      │  │ 并发上传/下载       │ │
│  │ 事件处理  │  │ LS/GET/PUT   │  │ 独立socket          │ │
│  └──────────┘  └──────────────┘  └────────────────────┘ │
└─────────────────────────┬───────────────────────────────┘
                          │ TCP (自定义协议)
┌─────────────────────────┴───────────────────────────────┐
│                   服务器 (epoll + 线程池)                 │
│  ┌────────────┐  ┌──────────────────────────────────┐   │
│  │ 主线程      │  │ 工作线程池 (8线程)                │   │
│  │ epoll_wait │→│ LOGIN/LS/GET/PUT/LISTDIR/BYE     │   │
│  │ 连接管理    │  │ 文件I/O + 协议响应               │   │
│  └────────────┘  └──────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────┐   │
│  │ TUI 线程: 共享内存监控客户端状态                    │   │
│  └──────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────┘
```

## 目录结构

```
Ubantudemo/
├── main.c                    # 客户端入口：LVGL初始化 + 事件循环
├── CMakeLists.txt            # 客户端CMake配置
├── Makefile                  # 客户端Makefile（备用）
├── build.sh                  # 客户端构建脚本
├── rebuild.sh                # 一键构建客户端+服务器
├── lv_conf.h                 # LVGL配置
├── mouse_cursor_icon.c       # 鼠标光标图标
│
├── client/                   # 客户端源码
│   ├── network_task.c        # 网络线程：协议解析、传输队列、线程池
│   ├── network_task.h        # 网络模块接口定义
│   ├── ui_manager.c          # UI管理：登录界面、文件管理器、进度弹窗
│   ├── ui_manager.h          # UI模块接口定义
│   ├── load/                 # 下载文件存放目录
│   └── my_ftp/               # 原始FTP实现（参考）
│
├── server/                   # 服务器源码
│   ├── CMakeLists.txt        # 服务器CMake配置
│   ├── build.sh              # 服务器构建脚本
│   ├── users.conf            # 用户认证配置
│   ├── inc/
│   │   ├── protocol.h        # 协议定义（命令号、MY_FTP_BOOT路径）
│   │   ├── handler.h         # 命令处理器接口 + client_session_t
│   │   ├── thread_pool.h     # 线程池定义（task_t, thread_pool_t）
│   │   ├── ipc_shm.h         # 共享内存IPC（客户端状态监控）
│   │   └── sys_auth.h        # 用户认证接口
│   ├── src/
│   │   ├── main.c            # 服务器入口：epoll事件循环 + 连接管理
│   │   ├── handler.c         # 命令处理器：LOGIN/LS/GET/PUT/LISTDIR/BYE
│   │   ├── protocol.c        # 协议封包/解包（read_packet/send_packet）
│   │   ├── thread_pool.c     # 线程池实现
│   │   ├── ipc_shm.c         # 共享内存实现
│   │   └── sys_auth.c        # 用户认证（读取users.conf）
│   ├── copy/                 # 服务器共享文件目录
│   └── remote_share/         # 远程共享文件目录
│
├── lvgl/                     # LVGL图形库源码
└── freetype-2.13.3/          # FreeType字体库源码
```

## 核心特性

### 网络架构
- **Reactor模式**：服务器主线程使用 epoll 边缘触发模式，高效管理并发连接
- **线程池**：服务器8线程 + 客户端3线程，复用线程避免频繁创建开销
- **Half-sync/Half-reactive**：主线程异步接收，工作线程同步处理文件I/O

### 传输功能
- **文件夹递归传输**：上传/下载整个文件夹（含子目录），自动创建目录结构
- **多文件批量传输**：支持多选文件，任务队列 + 线程池并发处理
- **传输进度显示**：实时进度条，支持取消传输
- **重复文件检测**：上传/下载重复文件时弹出提示

### UI功能
- **双面板文件管理器**：左侧服务器文件列表，右侧本地文件列表
- **长按多选**：长按选中文件/文件夹，支持批量操作
- **文件夹导航**：单击进入子目录，`..`返回上级目录
- **Delete功能**：仅删除客户端文件，删除服务器文件时弹出错误提示

### 安全特性
- **用户认证**：基于 `users.conf` 的用户名/密码验证
- **路径安全检查**：禁止绝对路径和 `..` 目录遍历攻击
- **文件锁**：认证文件读取使用 `flock(LOCK_SH)` 防止并发修改

## 通信协议

### 帧格式
```
0xC0 | pkg_len(4B LE) | cmd_no(4B LE) | [args...] | 0xC0
```
- 帧头/帧尾：`0xC0` 标记数据包边界
- `pkg_len`：包总长度（小端序）
- `cmd_no`：命令编号
- 文件数据以原始字节流传输，不经过协议封帧

### 命令列表

| 命令号 | 命令名         | 方向   | 说明                     |
|--------|---------------|--------|--------------------------|
| 1024   | FTP_CMD_LS    | C→S    | 列出目录内容             |
| 1025   | FTP_CMD_GET   | C→S    | 下载文件                 |
| 1026   | FTP_CMD_PUT   | C→S    | 上传文件                 |
| 1027   | FTP_CMD_BYE   | C→S    | 断开连接                 |
| 1028   | FTP_CMD_LOGIN | C→S    | 登录认证                 |
| 1029   | FTP_CMD_CANCEL| C→S    | 取消传输                 |
| 1032   | FTP_CMD_DONE  | S→C    | 传输完成通知             |
| 1033   | FTP_CMD_LISTDIR| C→S   | 递归列出目录所有文件     |

## 构建与运行

### 依赖获取（重要）

LVGL 与 FreeType 为第三方依赖，**不随仓库提交**（仓库只含本项目源码）。首次构建前需准备：

```bash
# 1. LVGL v8（客户端 GUI 库）→ 放到项目根目录 lvgl/
git clone --branch v8.3.11 --depth 1 https://github.com/lvgl/lvgl.git lvgl

# 2. FreeType 2.13.3（字体渲染）→ 放到项目根目录 freetype-2.13.3/
wget https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.gz
tar -xzf freetype-2.13.3.tar.gz

# 3. 服务器端依赖（与客户端共用）→ 复制到 server/ 下
cp -r lvgl server/
cp -r freetype-2.13.3 server/
```

> 注：`server/users.conf` 为示例账号，部署时请自行修改（勿提交真实密码）。

### 环境要求
- Ubuntu 24.04 / Linux
- GCC, CMake, Make
- SDL2（客户端图形显示）

### 一键构建
```bash
bash rebuild.sh
```

### 分别构建

**客户端：**
```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

**服务器：**
```bash
cd server
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

### 运行

**终端1 - 启动服务器：**
```bash
./bin/ftp_server 0.0.0.0 8888
```

**终端2 - 启动客户端：**
```bash
./bin/main
```

### 登录凭据
默认用户配置见 `server/users.conf`：

| 用户名 | 密码   |
|--------|--------|
| admin  | 123456 |
| user   | pass   |
| root   | root   |
| test   | test   |

## 使用说明

### 文件操作
- **单击文件/文件夹**：进入子目录或选中文件
- **长按文件/文件夹**：多选模式（`.`和`..`不可选）
- **Upload按钮**：上传选中的本地文件/文件夹到服务器
- **Download按钮**：下载选中的服务器文件/文件夹到本地
- **Delete按钮**：删除本地文件（删除服务器文件会弹出错误）
- **Refresh按钮**：刷新文件列表

### 传输控制
- 传输时弹出进度条弹窗，实时显示传输进度
- **Close按钮**：关闭弹窗，不影响传输
- **Cancel按钮**：取消传输，从队列中移除任务

### 重复文件处理
- 上传/下载时检测重复文件，弹出 `repeat file` 提示
- 文件夹传输时检测重复目录，弹出 `Dirent has exist` 提示

## 线程安全设计

### 服务器端
- **epoll fd 摘除**：工作线程处理期间，fd 从 epoll 集合中移除，处理完成后重新挂载
- **线程池 mutex/cond**：保护任务队列的 push/pop 操作
- **共享内存**：TUI 线程只读，工作线程写入对应 slot

### 客户端
- **传输队列 mutex/cond**：`g_tx_queue` 内置互斥锁，实现生产者-消费者模型
- **UI镜像数组**：填充 LVGL 列表时同步维护纯 C 数组，网络层无需访问 LVGL 内部结构
- **lv_async_call**：网络线程到 UI 线程的异步通信，避免跨线程操作 LVGL
- **volatile bool**：`g_transfer_cancelled` 跨线程可见的取消标志

## 技术栈

| 组件         | 技术                          |
|--------------|-------------------------------|
| 语言         | C99                           |
| 图形界面     | LVGL 9.x + SDL2              |
| 字体渲染     | FreeType 2.13.3              |
| 网络I/O      | epoll + pthread              |
| 构建系统     | CMake + Make                 |
| IPC          | 共享内存（shm）               |
| 认证         | flock 文件锁 + users.conf     |

## 许可证

本项目基于 MIT 许可证开源，详情请参见 [LICENSE](LICENSE) 文件。

Copyright (c) 2025 kiki_he

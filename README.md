<div align="center">

# TodoFloat

**超低占用的 Windows 原生单文件当日待办悬浮窗**

[![Platform](https://img.shields.io/badge/Platform-Windows%2010%2F11-0078D6?style=flat-square&logo=windows)](https://github.com/zih19964-commits/TodoFloat)
[![Language](https://img.shields.io/badge/C%2B%2B-23-00599C?style=flat-square&logo=cplusplus)](https://github.com/zih19964-commits/TodoFloat)
[![Size](https://img.shields.io/badge/Binary-1.2%20MB-2EA44F?style=flat-square)](https://github.com/zih19964-commits/TodoFloat/releases)
[![Memory](https://img.shields.io/badge/Private%20RAM-~19%20MB-8A2BE2?style=flat-square)](https://github.com/zih19964-commits/TodoFloat)
[![Renderer](https://img.shields.io/badge/Renderer-Direct2D%20%2F%20DirectWrite-FF6F00?style=flat-square)](https://github.com/zih19964-commits/TodoFloat)

基于 C++23、`User32`、`Direct2D`、`DirectWrite` 与逐像素 Alpha 分层窗口构建的单文件便携工具。  
解压即用、静置近乎零开销，专为高频专注工作流与本地 AI 协作设计。

</div>

---

## 核心亮点

- **极致轻量单文件**：静态链接编译仅约 `1.2 MB`，常驻私有内存约 `19 MB`，无 Electron / WebView 运行时负担。
- **逐像素半透 HUD**：深色半透圆角画布配合带柔和暗影的高对比纯白文字，在深色编辑器与浅色网页上均清晰可读；滚动顶栏滚轮可在 `15%`–`95%` 之间实时调节背景不透明度。
- **三种工作形态无缝切换**：
  - **自适应展开面板**：高度随当日待办条目自动伸缩，未完成事项置顶，已完成事项带删除线与完成时间沉降于下方；条目过多时自动转为内部滚轮滚动。
  - **单行聚焦胶囊**：双击顶栏或按 `` Alt + ` `` 收拢为单行胶囊，直接展示第一条未完成待办，一键勾选并自动切至下一条。
  - **双侧贴边微光耳标**：拖至屏幕左边缘或右边缘释放，收缩为紧贴边界的竖向耳标（可上下拖动调整高度）；鼠标触碰平滑滑出，移出 `260ms` 后自动收回。
- **防误收与鼠标穿透**：通过 `WS_EX_TOOLWINDOW` 隐身于任务栏与 `Alt+Tab`，并守护 Z 轴层级使 `Win + D` 显示桌面时依然可见；支持一键切换鼠标穿透（HUD 轻透态，点击与滚轮直达底层应用）。
- **本地 Markdown 与 AI 实时联动**：固定以同级目录 `todo.md` 作为当日待办与 AI 接入入口，内置 `ReadDirectoryChangesW` 内核监听（`50ms` 防抖），外部 AI 或编辑器保存后界面即时刷新，每日自动归档至 `logs/YYYY-MM-DD.md`。
- **一键开机自启**：系统托盘与悬浮窗右键菜单内置“开机自动启动”开关，自动按当前程序路径维护 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`，配合单实例互斥锁避免重复启动。

---

## 快速上手

1. 前往 [Releases](https://github.com/zih19964-commits/TodoFloat/releases) 下载 `TodoFloat-v1.0.0-win64.zip` 并解压到任意固定目录。
2. 双击运行 `TodoFloat.exe`，程序会在同级目录自动创建 `todo.md` 与 `logs/`。
3. 右键单击系统托盘图标可快速开启 **开机自动启动**、切换置顶/鼠标穿透、打开日志目录或复制 AI 接入说明。

---

## 快捷键一览

### 全局快捷键（任意前台应用可用）

| 快捷键 | 功能说明 |
| :--- | :--- |
| `Alt + Q` | 闪电新建待办：自动唤醒面板并聚焦中文输入法，回车创建，`Esc` 取消并恢复原状态 |
| `` Alt + ` `` | 一键召唤或收起：贴边态下滑出/收回，自由态下切换展开与单行聚焦胶囊 |
| `Alt + T` | 切换鼠标穿透模式（左键单击系统托盘图标同样生效） |

### 面板内单键交互（激活焦点且未处于输入状态）

| 快捷键 | 功能说明 |
| :--- | :--- |
| `↑` / `↓` 或 `J` / `K` | 上下移动选中待办行 |
| `Space` | 切换当前选中项的完成状态 |
| `Delete` | 删除当前选中项 |
| `Enter` / `N` | 打开底部内联输入框 |
| `[` / `]` | 贴靠屏幕左侧边 / 右侧边 |
| `Ctrl + ↑` / `Ctrl + ↓` | 增加 / 降低背景画布不透明度（顶栏滚轮同样生效） |
| `Esc` | 收回侧边滑出面板 |

---

## 本地 AI 协作与跨端同步架构

- **外部 AI 实时写入**：点击顶栏 `AI` 按钮即可复制包含 `todo.md` 绝对路径的接入提示。外部 Agent 直接读写 `todo.md` 后，悬浮窗自动同步展示与重算高度。
- **每日自动归档**：当日待办实时镜像至 `logs/YYYY-MM-DD.md`；跨入新日期后自动保留昨日完成与未完成记录并重置今日清单。
- **跨端同步扩展端口**：源码在 `src/sync_port.hpp` 与 `TodoStorage` 中预留了传输无关的 `ISyncTransport` 抽象接口、单调递增修订号 `revision` 与 `TodoDocument::mergeWithRemote` 无损并集合并算法。当日快照不足 `1 KB`，后续可直接挂载局域网（UDP 组播 + HTTP/WebSocket）或蓝牙（WinRT BLE GATT 对接 HarmonyOS `@ohos.bluetooth.ble`）传输模块实现多端实时同步。

---

## 从源码编译与测试

环境要求：Windows 10 / 11 + 支持 C++23 的 `MinGW-w64 GCC`（`g++`）。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
```

脚本会自动执行以下两步：
1. 编译并运行 `tests/test_core.cpp` 全部单元测试（涵盖 Markdown 解析、自适应 HUD 几何与双侧贴边状态机、内核文件监听、跨端合并端口、开机自启注册表逻辑）。
2. 以 `-std=c++23 -O2 -Wall -Wextra -Werror -static` 编译生成静态单文件 `TodoFloat.exe`。

---

## 项目结构

```text
TodoFloat/
├── src/
│   ├── main.cpp          # 入口、单实例锁、Per-Monitor DPI V2 初始化
│   ├── model.hpp/.cpp    # 待办文档模型、Markdown 解析/合并与窗口状态机
│   ├── storage.hpp/.cpp  # todo.md / logs/ 持久化与 ReadDirectoryChangesW 监听
│   ├── startup.hpp/.cpp  # Windows 当前用户开机自启注册表管理
│   ├── sync_port.hpp     # 传输无关的跨端同步抽象接口
│   └── ui_window.hpp/.cpp# Direct2D/DirectWrite 分层渲染、动画、IME 与托盘交互
├── tests/
│   └── test_core.cpp     # 核心与回归单元测试套件
└── build.ps1             # 一键测试与 Release 单文件编译脚本
```

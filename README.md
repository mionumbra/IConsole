# IConsole - GameMaker 控制台窗口扩展

为 GameMaker 2026 提供原生控制台窗口支持。GMS2 Runtime 没有内置控制台，IConsole 填补了这个空白。

## 功能特性

- 🖥️ **原生控制台窗口** — Windows 上通过 `AllocConsole` 创建独立控制台
- 🎨 **彩色输出** — 支持 16 种颜色，按日志级别自动着色
- 📝 **日志级别** — DEBUG / INFO / WARN / ERROR 四级过滤
- ⏱️ **时间戳** — 可选的时间戳前缀
- 💾 **文件日志** — 同时输出到控制台和文件
- ⌨️ **控制台输入** — 支持交互式输入（非阻塞 + 超时读取）
- 🌐 **跨平台** — Windows / Linux / macOS 支持

## 快速开始

### 1. 在 GameMaker 中集成

1. 创建扩展（右键 → 创建扩展）
2. 添加 `extgen/IConsole.dll` 作为 Windows 平台文件
3. 添加 `scripts/IConsole/IConsole.gml` 作为脚本

### 2. GML 使用

```gml
// 打开控制台
iconsole_open();
iconsole_set_title("我的游戏控制台");

// 基础输出
iconsole_print_line("游戏启动！");

// 日志级别
iconsole_set_log_level(1);  // 只显示 INFO 及以上
iconsole_log(0, "这行不会显示");   // DEBUG - 被过滤
iconsole_log(1, "游戏启动");       // INFO  - 白色
iconsole_log(2, "内存不足");       // WARN  - 黄色
iconsole_log(3, "加载失败");       // ERROR - 红色

// 时间戳
iconsole_set_timestamp(true);
iconsole_log(1, "带时间戳的日志");
// 输出: [09:57:35] [INFO]  带时间戳的日志

// 文件日志
iconsole_set_log_file("game.log");

// 控制台输入
if (iconsole_has_input()) {
    var input = iconsole_read_line(100);
    iconsole_print_line("你输入了: " + input);
}
```

## API 参考

### 基础功能

| 函数 | 说明 |
|---|---|
| `iconsole_open()` | 打开控制台窗口 |
| `iconsole_close()` | 关闭控制台窗口 |
| `iconsole_is_open()` | 检查控制台是否已打开 |
| `iconsole_print(text)` | 打印文本（无换行） |
| `iconsole_print_line(text)` | 打印一行文本 |
| `iconsole_clear()` | 清空控制台 |
| `iconsole_set_title(title)` | 设置窗口标题 |
| `iconsole_set_color(color)` | 设置文本颜色 (0-15) |

### 颜色代码

| 值 | 颜色 | 值 | 颜色 |
|---|---|---|---|
| 0 | 黑色 | 8 | 灰色 |
| 1 | 蓝色 | 9 | 亮蓝 |
| 2 | 绿色 | 10 | 亮绿 |
| 3 | 青色 | 11 | 亮青 |
| 4 | 红色 | 12 | 亮红 |
| 5 | 品红 | 13 | 亮品红 |
| 6 | 黄色 | 14 | 亮黄 |
| 7 | 白色 | 15 | 亮白 |

### 日志级别

| 函数 | 说明 |
|---|---|
| `iconsole_set_log_level(level)` | 设置日志级别 (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR) |
| `iconsole_get_log_level()` | 获取当前日志级别 |
| `iconsole_log(level, text)` | 带级别的日志输出（自动着色） |

### 时间戳

| 函数 | 说明 |
|---|---|
| `iconsole_set_timestamp(enabled)` | 启用/禁用时间戳前缀 |
| `iconsole_timestamp_enabled()` | 检查时间戳是否启用 |

### 文件日志

| 函数 | 说明 |
|---|---|
| `iconsole_set_log_file(path)` | 设置日志文件路径 |
| `iconsole_get_log_file()` | 获取当前日志文件路径 |
| `iconsole_close_log_file()` | 关闭日志文件 |
| `iconsole_log_file_open()` | 检查日志文件是否打开 |

### 控制台输入

| 函数 | 说明 |
|---|---|
| `iconsole_has_input()` | 检查是否有输入可用（非阻塞） |
| `iconsole_read_line(timeout_ms)` | 读取一行输入（最多等待 timeout_ms 毫秒） |
| `iconsole_read_key()` | 读取单个按键（非阻塞，返回虚拟键码） |

## 构建

### 前提条件

- extgen（代码生成器）
- CMake 3.25+
- Visual Studio 2026（v145 工具集）

### 构建步骤

```bash
cd extgen

# 配置
cmake --preset win-x64-debug

# 编译
cmake --build --preset win-x64-debug

# 输出: extgen/IConsole.dll
```

## 跨平台说明

| 平台 | 控制台行为 |
|---|---|
| **Windows** | 弹出原生控制台窗口，支持颜色、标题、Unicode |
| **Linux** | 输出到 stderr（终端启动时可见） |
| **macOS** | 同上，支持 ANSI 颜色转义码 |

## 项目结构

```
IConsole/
├── extgen/                          # extgen 项目
│   ├── config.json                  # 配置文件
│   ├── spec.gmidl                   # GMIDL API 定义
│   ├── CMakeLists.txt               # CMake 根文件
│   ├── CMakePresets.json            # 构建预设
│   ├── code_gen/                    # 自动生成的桥接代码
│   ├── src/native/                  # C++ 实现
│   │   ├── IConsole_native.cpp      # 核心实现
│   │   └── IConsole_native.h        # 头文件
│   └── cmake/                       # 平台 CMake 脚本
├── scripts/IConsole/
│   └── IConsole.gml                 # GML 绑定
└── README.md
```

## 许可证

MIT License
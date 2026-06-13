#include "IConsole_native.h"
#include <iostream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <string>
#include <cstdio>
#include <cstring>

#ifdef OS_WINDOWS
#include <windows.h>
#include <tlhelp32.h>
#else
#include <unistd.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/stat.h>
#endif

namespace {

// -----------------------------
// 全局状态
// -----------------------------
std::mutex g_console_mutex;
bool g_is_open = false;               // 是否已经调用过 iconsole_open 且存在可见控制台窗口
std::int32_t g_log_level = 0;         // 日志等级过滤（0=DEBUG,1=INFO,2=WARN,3=ERROR）
bool g_timestamp_enabled = false;     // 是否启用时间戳
std::FILE* g_user_log_file = nullptr; // 用户指定的日志文件
std::string g_user_log_path;
static bool g_force_ansi = false; // 默认 false：pipe 情况不输出 ANSI

#ifdef OS_WINDOWS
HANDLE g_hStdout = INVALID_HANDLE_VALUE;
HANDLE g_hStdin = INVALID_HANDLE_VALUE;
WORD g_default_attr = 7;
bool g_attached_console = false;  // 我们是否 Attach 到父控制台
bool g_allocated_console = false; // 我们是否 AllocConsole 创建了新控制台
#endif

// -----------------------------
// 平台检测辅助函数
// -----------------------------
#ifdef OS_WINDOWS

// 判断当前 Runtime 的 stdout 是否是一个真实控制台（CMD/PowerShell）
// 返回 true 表示 stdout 连接到控制台窗口；false 表示被重定向或不存在。
static bool runtime_stdout_is_console()
{
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr)
        return false;

    DWORD ft = ::GetFileType(hOut);
    bool isChar = (ft == FILE_TYPE_CHAR);

    DWORD mode = 0;
    BOOL hasConsoleMode = ::GetConsoleMode(hOut, &mode);

    HWND hwnd = ::GetConsoleWindow();
    bool consoleWindowVisible = false;
    if (hwnd != NULL) {
        consoleWindowVisible = (::IsWindowVisible(hwnd) != FALSE);
    }

    if (hasConsoleMode)
        return true;
    if (isChar && consoleWindowVisible)
        return true;
    return false;
}

// 判断当前 stdout 是否被重定向（例如 IDE 捕获或管道/文件）
// 返回 true 表示被重定向或写入文件（IDE 捕获通常表现为管道）。
static bool runtime_stdout_is_redirected_or_pipe()
{
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr)
        return false;
    DWORD ft = ::GetFileType(hOut);
    return (ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK);
}

// 将 UTF-8 字符串写入控制台（不影响 std::cout）
// 不添加额外换行；调用方负责 CRLF
static void WriteConsoleUtf8AsWide(HANDLE hConsole, const std::string& utf8)
{
    if (hConsole == INVALID_HANDLE_VALUE || hConsole == nullptr)
        return;
    int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    if (wlen <= 0)
        return;
    std::wstring wbuf((size_t)wlen, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), &wbuf[0], wlen);
    DWORD written = 0;
    ::WriteConsoleW(hConsole, wbuf.c_str(), static_cast<DWORD>(wlen), &written, nullptr);
}

// 辅助：判断 stdout 是否连接到可见控制台窗口
static bool stdout_is_console_window()
{
    HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr)
        return false;
    DWORD mode = 0;
    if (::GetConsoleMode(hOut, &mode))
        return true;
    HWND hwnd = ::GetConsoleWindow();
    return (hwnd != NULL && ::IsWindowVisible(hwnd) != FALSE);
}

#else

static bool runtime_stdout_is_console() { return isatty(STDOUT_FILENO) != 0; }
static bool runtime_stdout_is_redirected_or_pipe() { return !runtime_stdout_is_console(); }

#endif

// -----------------------------
// 辅助：时间戳 / 前缀 / 颜色
// -----------------------------
static std::string get_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    struct tm tm_buf;
#ifdef OS_WINDOWS
    localtime_s(&tm_buf, &time);
#else
    localtime_r(&time, &tm_buf);
#endif
    char buf[16];
    std::snprintf(buf, sizeof(buf), "[%02d:%02d:%02d]", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
    return std::string(buf);
}

static std::string get_level_prefix(std::int32_t level)
{
    switch (level) {
    case 0:
        return "[DEBUG] ";
    case 1:
        return "[INFO]  ";
    case 2:
        return "[WARN]  ";
    case 3:
        return "[ERROR] ";
    default:
        return "[LOG]   ";
    }
}

static std::int32_t get_color_for_level(std::int32_t level)
{
    switch (level) {
    case 0:
        return 8;
    case 1:
        return 7;
    case 2:
        return 6;
    case 3:
        return 4;
    default:
        return 7;
    }
}

// ANSI color helpers for runtime (std::cout) output (保留以备可选开关使用)
static const char* ansi_color_for_level(int level)
{
    switch (level) {
    case 0:
        return "\x1b[90m"; // DEBUG - bright black
    case 1:
        return "\x1b[37m"; // INFO  - white
    case 2:
        return "\x1b[33m"; // WARN  - yellow
    case 3:
        return "\x1b[31m"; // ERROR - red
    default:
        return "\x1b[37m";
    }
}
static const char* ansi_reset() { return "\x1b[0m"; }

// -----------------------------
// 写入用户日志文件（如果打开）
// -----------------------------
static void write_to_log_file(const std::string& text)
{
    if (g_user_log_file) {
        std::fwrite(text.data(), 1, text.size(), g_user_log_file);
        std::fflush(g_user_log_file);
    }
}

} // anonymous namespace

// ============================================================================
// iconsole_set_force_ansi
//  - 可选开关：当调用者明确知道消费端能解释 ANSI 时可开启（默认 false）
// ============================================================================
void iconsole_set_force_ansi(bool enable)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    g_force_ansi = enable;
}

// ============================================================================
// iconsole_open
//  - Windows: 先复用已有控制台；否则尝试 AttachConsole（不做 freopen）；若不可见或 Attach 失败则 AllocConsole
//  - POSIX: 简单方案，仅标记 g_is_open = true（不创建窗口）
// ============================================================================
void iconsole_open()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (g_is_open)
        return;

#ifdef OS_WINDOWS
    // 1) 如果进程已经连接到控制台（例如以控制台子系统启动），复用它
    if (stdout_is_console_window()) {
        g_hStdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_hStdin = ::GetStdHandle(STD_INPUT_HANDLE);

        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(g_hStdout, &csbi)) {
            g_default_attr = csbi.wAttributes;
        }

        g_attached_console = false;
        g_allocated_console = false;
        g_is_open = true;

        // 尝试启用 VT 模式（允许解释 ANSI 转义序列），失败则忽略
        DWORD outMode = 0;
        if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleMode(g_hStdout, &outMode)) {
            ::SetConsoleMode(g_hStdout, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }

        // 仅当控制台不是我们自己 AllocConsole 创建的（即不是双击新窗口）时写开头换行
        if (!g_allocated_console && g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
            WriteConsoleUtf8AsWide(g_hStdout, std::string("\r\n"));
        }

        return;
    }

    // 2) 尝试 AttachConsole（优先附加父控制台）
    if (::AttachConsole(ATTACH_PARENT_PROCESS)) {
        // 不做 freopen，保留 std::cout 给可能的父进程/IDE 捕获
        g_attached_console = true;
        g_allocated_console = false;

        g_hStdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_hStdin = ::GetStdHandle(STD_INPUT_HANDLE);

        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleMode(g_hStdin, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);

        HWND hwnd = ::GetConsoleWindow();
        bool console_visible = (hwnd != NULL) && (::IsWindowVisible(hwnd) != FALSE);

        if (console_visible) {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(g_hStdout, &csbi)) {
                g_default_attr = csbi.wAttributes;
            }

            g_is_open = true;

            // 尝试启用 VT 模式（允许解释 ANSI 转义序列），失败则忽略
            DWORD outMode = 0;
            if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleMode(g_hStdout, &outMode)) {
                ::SetConsoleMode(g_hStdout, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }

            // AttachConsole 表示附加到父控制台（通常是 CMD/PowerShell），写开头换行（但仍检查不是我们自己 AllocConsole）
            if (!g_allocated_console && g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
                WriteConsoleUtf8AsWide(g_hStdout, std::string("\r\n"));
            }

            return;
        } else {
            // 附加后不可见（通常是由父进程以管道捕获），不要 FreeConsole/AllocConsole，也不要 freopen
            g_is_open = false;
            return;
        }
    }

    // 3) Attach 失败：创建新的独立控制台窗口（AllocConsole）
    if (::AllocConsole()) {
        HANDLE h = CreateFileA(
            "CONOUT$",
            GENERIC_WRITE | GENERIC_READ,
            FILE_SHARE_WRITE | FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (h != INVALID_HANDLE_VALUE && h != nullptr) {
            g_hStdout = h;
            g_hStdin = ::GetStdHandle(STD_INPUT_HANDLE);

            // 为了兼容 C 标准库，重定向 stdout/stderr 到 CONOUT$（可选）
#ifdef _MSC_VER
            FILE* fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
#else
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
#endif

            ::SetConsoleOutputCP(CP_UTF8);
            ::SetConsoleMode(g_hStdin, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);

            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (::GetConsoleScreenBufferInfo(g_hStdout, &csbi)) {
                g_default_attr = csbi.wAttributes;
            }

            // 标记为我们自己创建的控制台（双击等场景）
            g_allocated_console = true;
            g_attached_console = false;
            g_is_open = true;

            // AllocConsole 创建的是新窗口（双击场景），不要写开头换行以避免窗口顶部空白
            return;
        } else {
            // 极罕见：AllocConsole 成功但无法打开 CONOUT$
            g_hStdout = INVALID_HANDLE_VALUE;
            g_hStdin = INVALID_HANDLE_VALUE;
            g_allocated_console = true;
            g_attached_console = false;
            g_is_open = true;
            return;
        }
    }
#else
    g_is_open = true; // POSIX 简单方案
#endif
}

// ============================================================================
// iconsole_print
//  - 打印不带换行的文本到“控制台”或 runtime stdout（IDE 捕获）
// ============================================================================
void iconsole_print(std::string_view text)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);

    write_to_log_file(std::string(text));

#ifdef OS_WINDOWS
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open || g_allocated_console;

    // 不带换行的写入：按场景决定目标，避免重复输出
    if (isPipe && !prefer_console) {
        // IDE / pipe: 写 std::cout（不加 CRLF），不输出 ANSI（方案 A）
        if (g_force_ansi) {
            // print without newline; caller expects no CRLF
            std::cout << text;
        } else {
            std::cout << text;
        }
        std::fflush(stdout);
        return;
    }

    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
        // 控制台可见：写控制台（不加换行）
        WriteConsoleUtf8AsWide(g_hStdout, std::string(text));
        return;
    }

    // 兜底：写 std::cout
    std::cout << text;
    std::fflush(stdout);
#else
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open;

    if (isPipe && !prefer_console) {
        std::cout << text;
        std::fflush(stdout);
        return;
    }

    if (g_is_open) {
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fflush(stderr);
    } else if (isPipe) {
        std::cout << text;
        std::fflush(stdout);
    }
#endif
}

// ============================================================================
// iconsole_print_line
//  - 同 iconsole_print，但在末尾追加换行（Windows 使用 CRLF）
// ============================================================================
void iconsole_print_line(std::string_view text)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);

    // 为 runtime(pipe) 与 console 分别准备换行形式
    std::string runtime_line(text);
    runtime_line += '\n'; // 用于 std::cout / 被捕获场景

    std::string console_line(text);
    console_line += "\r\n"; // 用于 Windows 控制台窗口

    write_to_log_file(runtime_line); // 日志文件使用 LF

#ifdef OS_WINDOWS
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open || g_allocated_console;

    // Pipe 且无可见控制台：只写 runtime_line（LF），不要写 CRLF
    if (isPipe && !prefer_console) {
        if (g_force_ansi) {
            // 当强制 ANSI 时，仍输出纯文本或带颜色前缀（这里保持纯文本以避免 IDE 被污染）
            std::cout << runtime_line;
        } else {
            std::cout << runtime_line;
        }
        std::fflush(stdout);
        return;
    }

    // 有可见控制台：写控制台（CRLF）
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
        WriteConsoleUtf8AsWide(g_hStdout, console_line);
        return;
    }

    // 兜底：写 runtime_line
    std::cout << runtime_line;
    std::fflush(stdout);
#else
    // POSIX: 使用 LF，写日志并根据 g_is_open 决定写 stderr 或 stdout
    std::string runtime_line = text;
    runtime_line += '\n';
    write_to_log_file(runtime_line);

    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open;

    if (isPipe && !prefer_console) {
        std::cout << runtime_line;
        std::fflush(stdout);
        return;
    }

    if (g_is_open) {
        std::fwrite(runtime_line.data(), 1, runtime_line.size(), stderr);
        std::fflush(stderr);
    } else {
        std::cout << runtime_line;
        std::fflush(stdout);
    }
#endif
}

// ============================================================================
// iconsole_clear
// ============================================================================
void iconsole_clear()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!g_is_open)
        return;

#ifdef OS_WINDOWS
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        COORD home = {0, 0};
        if (::GetConsoleScreenBufferInfo(g_hStdout, &csbi)) {
            DWORD console_size = csbi.dwSize.X * csbi.dwSize.Y;
            DWORD written = 0;
            ::FillConsoleOutputCharacterW(g_hStdout, L' ', console_size, home, &written);
            ::FillConsoleOutputAttribute(g_hStdout, g_default_attr, console_size, home, &written);
            ::SetConsoleCursorPosition(g_hStdout, home);
        }
    }
#else
    std::fprintf(stderr, "\033[2J\033[H");
    std::fflush(stderr);
#endif
}

// ============================================================================
// iconsole_close
// ============================================================================
void iconsole_close()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!g_is_open && !g_attached_console && !g_allocated_console)
        return;

#ifdef OS_WINDOWS
    if (g_allocated_console) {
        if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
            CloseHandle(g_hStdout);
        }
        ::FreeConsole();
        g_hStdout = INVALID_HANDLE_VALUE;
        g_hStdin = INVALID_HANDLE_VALUE;
        g_allocated_console = false;
    } else if (g_attached_console) {
        // 分离附加的父控制台（可选行为）
        ::FreeConsole();
        g_hStdout = INVALID_HANDLE_VALUE;
        g_hStdin = INVALID_HANDLE_VALUE;
        g_attached_console = false;
    } else {
        g_hStdout = INVALID_HANDLE_VALUE;
        g_hStdin = INVALID_HANDLE_VALUE;
    }
#endif

    g_is_open = false;
}

// ============================================================================
// iconsole_is_open
// ============================================================================
bool iconsole_is_open()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    return g_is_open;
}

// ============================================================================
// iconsole_set_title
// ============================================================================
void iconsole_set_title(std::string_view title)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!g_is_open)
        return;

#ifdef OS_WINDOWS
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), nullptr, 0);
        if (wlen > 0) {
            std::wstring wtitle(static_cast<size_t>(wlen), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), &wtitle[0], wlen);
            ::SetConsoleTitleW(wtitle.c_str());
        }
    }
#else
    (void)title;
#endif
}

// ============================================================================
// iconsole_set_color
// ============================================================================
void iconsole_set_color(std::int32_t color)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!g_is_open)
        return;

#ifdef OS_WINDOWS
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
        WORD attr = static_cast<WORD>(color & 0x0F);
        ::SetConsoleTextAttribute(g_hStdout, attr);
    }
#else
    const char* ansi_code;
    switch (color & 0x0F) {
    case 0:
        ansi_code = "\033[30m";
        break;
    case 1:
        ansi_code = "\033[34m";
        break;
    case 2:
        ansi_code = "\033[32m";
        break;
    case 3:
        ansi_code = "\033[36m";
        break;
    case 4:
        ansi_code = "\033[31m";
        break;
    case 5:
        ansi_code = "\033[35m";
        break;
    case 6:
        ansi_code = "\033[33m";
        break;
    case 7:
        ansi_code = "\033[37m";
        break;
    case 8:
        ansi_code = "\033[90m";
        break;
    case 9:
        ansi_code = "\033[94m";
        break;
    case 10:
        ansi_code = "\033[92m";
        break;
    case 11:
        ansi_code = "\033[96m";
        break;
    case 12:
        ansi_code = "\033[91m";
        break;
    case 13:
        ansi_code = "\033[95m";
        break;
    case 14:
        ansi_code = "\033[93m";
        break;
    case 15:
        ansi_code = "\033[97m";
        break;
    default:
        ansi_code = "\033[37m";
        break;
    }
    std::fprintf(stderr, "%s", ansi_code);
    std::fflush(stderr);
#endif
}

// ============================================================================
// iconsole_set_log_level / iconsole_get_log_level
// ============================================================================
void iconsole_set_log_level(std::int32_t level)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (level < 0)
        level = 0;
    if (level > 3)
        level = 3;
    g_log_level = level;
}

std::int32_t iconsole_get_log_level()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    return g_log_level;
}

// ============================================================================
// iconsole_log
//  - 组合消息并按规则输出（runtime stdout 或 独立控制台）
// ============================================================================
void iconsole_log(std::int32_t level, std::string_view text)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (level < g_log_level)
        return;

    std::string plain;
    if (g_timestamp_enabled) {
        plain += get_timestamp();
        plain += ' ';
    }
    plain += get_level_prefix(level);
    plain += text;

#ifdef OS_WINDOWS
    std::string runtime_line = plain;
    runtime_line += '\n';
    std::string console_line = plain;
    console_line += "\r\n";

    write_to_log_file(runtime_line);

    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool hasConsole = (g_is_open || g_allocated_console);

    // Pipe 且无可见控制台：只输出纯文本（不输出 ANSI），除非显式强制
    if (isPipe && !hasConsole) {
        if (g_force_ansi) {
            const char* color = ansi_color_for_level(level);
            const char* reset = ansi_reset();
            std::cout << color << runtime_line << reset;
        } else {
            std::cout << runtime_line;
        }
        std::fflush(stdout);
        return;
    }

    // 有可见控制台：使用原生颜色
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr) {
        WORD oldAttr = g_default_attr;
        WORD attr = static_cast<WORD>(get_color_for_level(level) & 0x0F);
        ::SetConsoleTextAttribute(g_hStdout, attr);
        WriteConsoleUtf8AsWide(g_hStdout, console_line);
        ::SetConsoleTextAttribute(g_hStdout, oldAttr);
        return;
    }

    // 其它情况（例如 pipe 但未被上面捕获）：输出纯文本
    std::cout << runtime_line;
    std::fflush(stdout);
#else
    std::string runtime_line = plain;
    runtime_line += '\n';
    write_to_log_file(runtime_line);
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool hasConsole = g_is_open;
    if (isPipe && !hasConsole) {
        std::cout << runtime_line;
        std::fflush(stdout);
        return;
    }
    if (g_is_open) {
        const char* ansi_code;
        switch (get_color_for_level(level) & 0x0F) {
        case 8:
            ansi_code = "\033[90m";
            break;
        case 7:
            ansi_code = "\033[37m";
            break;
        case 6:
            ansi_code = "\033[33m";
            break;
        case 4:
            ansi_code = "\033[31m";
            break;
        default:
            ansi_code = "\033[37m";
            break;
        }
        std::fprintf(stderr, "%s%s\033[0m", ansi_code, runtime_line.c_str());
        std::fflush(stderr);
    } else {
        std::cout << runtime_line;
        std::fflush(stdout);
    }
#endif
}

// ============================================================================
// iconsole_set_timestamp / iconsole_timestamp_enabled
// ============================================================================
void iconsole_set_timestamp(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    g_timestamp_enabled = enabled;
}

bool iconsole_timestamp_enabled()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    return g_timestamp_enabled;
}

// ============================================================================
// iconsole_set_log_file / iconsole_get_log_file / iconsole_close_log_file / iconsole_log_file_open
// ============================================================================
void iconsole_set_log_file(std::string_view path)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);

    if (g_user_log_file) {
        std::fclose(g_user_log_file);
        g_user_log_file = nullptr;
    }

    g_user_log_path = std::string(path);
    g_user_log_file = std::fopen(g_user_log_path.c_str(), "w");
    if (g_user_log_file) {
        std::fprintf(g_user_log_file, "=== IConsole Log File ===\n");
        std::fprintf(g_user_log_file, "Path: %s\n", g_user_log_path.c_str());
        std::fprintf(g_user_log_file, "=========================\n");
        std::fflush(g_user_log_file);
    }
}

std::string iconsole_get_log_file()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    return g_user_log_path;
}

void iconsole_close_log_file()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (g_user_log_file) {
        std::fprintf(g_user_log_file, "=== Log file closed ===\n");
        std::fflush(g_user_log_file);
        std::fclose(g_user_log_file);
        g_user_log_file = nullptr;
    }
    g_user_log_path.clear();
}

bool iconsole_log_file_open()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    return g_user_log_file != nullptr;
}

// ============================================================================
// iconsole_has_input / iconsole_read_line / iconsole_read_key
// ============================================================================
bool iconsole_has_input()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!g_is_open)
        return false;

#ifdef OS_WINDOWS
    if (g_hStdin == INVALID_HANDLE_VALUE || g_hStdin == nullptr)
        return false;

    DWORD events = 0;
    if (!::GetNumberOfConsoleInputEvents(g_hStdin, &events))
        return false;
    if (events == 0)
        return false;

    INPUT_RECORD ir;
    DWORD read = 0;
    while (::PeekConsoleInput(g_hStdin, &ir, 1, &read) && read > 0) {
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.uChar.AsciiChar != 0) {
            return true;
        }
        ::ReadConsoleInput(g_hStdin, &ir, 1, &read);
    }
    return false;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    return ::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
#endif
}

std::string iconsole_read_line(std::int32_t timeout_ms)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!g_is_open)
        return "";

#ifdef OS_WINDOWS
    if (g_hStdin == INVALID_HANDLE_VALUE || g_hStdin == nullptr)
        return "";

    DWORD start = GetTickCount();
    while (true) {
        DWORD events = 0;
        if (!::GetNumberOfConsoleInputEvents(g_hStdin, &events))
            return "";
        if (events > 0)
            break;
        if (timeout_ms >= 0 && (GetTickCount() - start) >= static_cast<DWORD>(timeout_ms))
            return "";
        Sleep(1);
    }

    wchar_t buffer[4096];
    DWORD chars_read = 0;
    BOOL success = ::ReadConsoleW(g_hStdin, buffer, 4095, &chars_read, nullptr);
    if (!success || chars_read == 0)
        return "";

    while (chars_read > 0 && (buffer[chars_read - 1] == L'\n' || buffer[chars_read - 1] == L'\r')) {
        chars_read--;
    }
    buffer[chars_read] = L'\0';

    int utf8_len
        = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(chars_read), nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0)
        return "";
    std::string result(static_cast<size_t>(utf8_len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(chars_read), &result[0], utf8_len, nullptr, nullptr);
    return result;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    struct timeval tv;
    if (timeout_ms < 0) {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    } else {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
    }

    if (::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return "";

    char buffer[4096];
    ssize_t bytes = ::read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
    if (bytes <= 0)
        return "";

    while (bytes > 0 && (buffer[bytes - 1] == '\n' || buffer[bytes - 1] == '\r'))
        bytes--;
    buffer[bytes] = '\0';
    return std::string(buffer);
#endif
}

std::int32_t iconsole_read_key()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (!g_is_open)
        return 0;

#ifdef OS_WINDOWS
    if (g_hStdin == INVALID_HANDLE_VALUE || g_hStdin == nullptr)
        return 0;

    INPUT_RECORD ir;
    DWORD read = 0;
    while (::PeekConsoleInput(g_hStdin, &ir, 1, &read) && read > 0) {
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            ::ReadConsoleInput(g_hStdin, &ir, 1, &read);
            return static_cast<std::int32_t>(ir.Event.KeyEvent.wVirtualKeyCode);
        }
        ::ReadConsoleInput(g_hStdin, &ir, 1, &read);
    }
    return 0;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);

    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    if (::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return 0;

    char c = 0;
    ssize_t bytes = ::read(STDIN_FILENO, &c, 1);
    if (bytes <= 0)
        return 0;
    return static_cast<std::int32_t>(static_cast<unsigned char>(c));
#endif
}

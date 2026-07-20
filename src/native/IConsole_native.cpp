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

namespace
{

    // -----------------------------
    // Global state
    // -----------------------------
    std::mutex g_console_mutex;
    bool g_is_open = false;               // Whether iconsole_open has been called and a visible console exists
    std::int32_t g_log_level = 0;         // Log level filter (0=DEBUG,1=INFO,2=WARN,3=ERROR)
    bool g_timestamp_enabled = false;     // Whether timestamps are enabled
    std::FILE *g_user_log_file = nullptr; // User-specified log file
    std::string g_user_log_path;
    static bool g_force_ansi = false; // Default false: do not emit ANSI when stdout is a pipe

#ifdef OS_WINDOWS
    HANDLE g_hStdout = INVALID_HANDLE_VALUE;
    HANDLE g_hStdin = INVALID_HANDLE_VALUE;
    WORD g_default_attr = 7;
    bool g_attached_console = false;  // Whether we attached to the parent console
    bool g_allocated_console = false; // Whether we created a new console via AllocConsole
#endif

// -----------------------------
// Platform detection helpers
// -----------------------------
#ifdef OS_WINDOWS

    // Determine whether the runtime stdout is a real console (CMD/PowerShell).
    // Returns true if stdout is connected to a console window; false if redirected or absent.
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
        if (hwnd != NULL)
        {
            consoleWindowVisible = (::IsWindowVisible(hwnd) != FALSE);
        }

        if (hasConsoleMode)
            return true;
        if (isChar && consoleWindowVisible)
            return true;
        return false;
    }

    // Determine whether stdout is redirected (IDE capture or pipe/file).
    // Returns true when redirected or writing to disk (IDE capture usually appears as a pipe).
    static bool runtime_stdout_is_redirected_or_pipe()
    {
        HANDLE hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut == INVALID_HANDLE_VALUE || hOut == nullptr)
            return false;
        DWORD ft = ::GetFileType(hOut);
        return (ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK);
    }

    // Write a UTF-8 string to the Windows console (does not affect std::cout).
    // Does not append newline; caller is responsible for CRLF.
    static void WriteConsoleUtf8AsWide(HANDLE hConsole, const std::string &utf8)
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

    // Helper: determine whether stdout is connected to a visible console window
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
    // Helpers: timestamp / prefix / color
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
        switch (level)
        {
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
        switch (level)
        {
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

    // ANSI color helpers for runtime (std::cout) output (kept for optional forced ANSI)
    static const char *ansi_color_for_level(int level)
    {
        switch (level)
        {
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
    static const char *ansi_reset() { return "\x1b[0m"; }

    // -----------------------------
    // Write to user log file (if open)
    // -----------------------------
    static void write_to_log_file(const std::string &text)
    {
        if (g_user_log_file)
        {
            std::fwrite(text.data(), 1, text.size(), g_user_log_file);
            std::fflush(g_user_log_file);
        }
    }

} // anonymous namespace

// ============================================================================
// iconsole_set_force_ansi
//  - Optional switch: enable when caller knows the consumer supports ANSI (default false)
// ============================================================================
void iconsole_set_force_ansi(bool enable)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    g_force_ansi = enable;
}

// ============================================================================
// iconsole_open
//  - Windows: reuse existing console; otherwise try AttachConsole (no freopen); if not visible or attach fails, AllocConsole
//  - POSIX: simple approach, just mark g_is_open = true (do not create a window)
// ============================================================================
void iconsole_open()
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (g_is_open)
        return;

#ifdef OS_WINDOWS
    // 1) If the process is already connected to a console (console subsystem), reuse it
    if (stdout_is_console_window())
    {
        g_hStdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_hStdin = ::GetStdHandle(STD_INPUT_HANDLE);

        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(g_hStdout, &csbi))
        {
            g_default_attr = csbi.wAttributes;
        }

        g_attached_console = false;
        g_allocated_console = false;
        g_is_open = true;

        // Try to enable VT mode (allow ANSI escape sequences); ignore failure
        DWORD outMode = 0;
        if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleMode(g_hStdout, &outMode))
        {
            ::SetConsoleMode(g_hStdout, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }

        // Only write an initial blank line when the console was not created by us (i.e., not AllocConsole)
        if (!g_allocated_console && g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
        {
            WriteConsoleUtf8AsWide(g_hStdout, std::string("\r\n"));
        }

        return;
    }

    // 2) Try AttachConsole (attach to parent console)
    if (::AttachConsole(ATTACH_PARENT_PROCESS))
    {
        // Do not freopen; keep std::cout for possible parent/IDE capture
        g_attached_console = true;
        g_allocated_console = false;

        g_hStdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_hStdin = ::GetStdHandle(STD_INPUT_HANDLE);

        ::SetConsoleOutputCP(CP_UTF8);
        ::SetConsoleMode(g_hStdin, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);

        HWND hwnd = ::GetConsoleWindow();
        bool console_visible = (hwnd != NULL) && (::IsWindowVisible(hwnd) != FALSE);

        if (console_visible)
        {
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(g_hStdout, &csbi))
            {
                g_default_attr = csbi.wAttributes;
            }

            g_is_open = true;

            // Try to enable VT mode (allow ANSI escape sequences); ignore failure
            DWORD outMode = 0;
            if (g_hStdout != INVALID_HANDLE_VALUE && ::GetConsoleMode(g_hStdout, &outMode))
            {
                ::SetConsoleMode(g_hStdout, outMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
            }

            // Write an initial blank line when attached to a parent console (for visual separation)
            if (!g_allocated_console && g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
            {
                WriteConsoleUtf8AsWide(g_hStdout, std::string("\r\n"));
            }

            return;
        }
        else
        {
            // Attached but not visible (likely parent captured via pipe); do not FreeConsole/AllocConsole or freopen
            g_is_open = false;
            return;
        }
    }

    // 3) Attach failed: create a new independent console window (AllocConsole)
    if (::AllocConsole())
    {
        HANDLE h = CreateFileA(
            "CONOUT$",
            GENERIC_WRITE | GENERIC_READ,
            FILE_SHARE_WRITE | FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (h != INVALID_HANDLE_VALUE && h != nullptr)
        {
            g_hStdout = h;
            g_hStdin = ::GetStdHandle(STD_INPUT_HANDLE);

            // Redirect stdout/stderr to CONOUT$ for C runtime compatibility (optional)
#ifdef _MSC_VER
            FILE *fp = nullptr;
            freopen_s(&fp, "CONOUT$", "w", stdout);
            freopen_s(&fp, "CONOUT$", "w", stderr);
#else
            freopen("CONOUT$", "w", stdout);
            freopen("CONOUT$", "w", stderr);
#endif

            ::SetConsoleOutputCP(CP_UTF8);
            ::SetConsoleMode(g_hStdin, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);

            CONSOLE_SCREEN_BUFFER_INFO csbi;
            if (::GetConsoleScreenBufferInfo(g_hStdout, &csbi))
            {
                g_default_attr = csbi.wAttributes;
            }

            // Mark as console we created (double-click scenario)
            g_allocated_console = true;
            g_attached_console = false;
            g_is_open = true;

            // For AllocConsole (new window), avoid writing an initial blank line to prevent top padding
            return;
        }
        else
        {
            // Rare: AllocConsole succeeded but CONOUT$ couldn't be opened
            g_hStdout = INVALID_HANDLE_VALUE;
            g_hStdin = INVALID_HANDLE_VALUE;
            g_allocated_console = true;
            g_attached_console = false;
            g_is_open = true;
            return;
        }
    }
#else
    g_is_open = true; // POSIX simple approach
#endif
}

// ============================================================================
// iconsole_print
//  - Print text without newline to console or runtime stdout (IDE capture)
// ============================================================================
void iconsole_print(std::string_view text)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);

    write_to_log_file(std::string(text));

#ifdef OS_WINDOWS
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open || g_allocated_console;

    // No-newline write: choose target based on scenario to avoid duplicate output
    if (isPipe && !prefer_console)
    {
        // IDE / pipe: write to std::cout (no CRLF), do not emit ANSI (scheme A)
        if (g_force_ansi)
        {
            std::cout << text;
        }
        else
        {
            std::cout << text;
        }
        std::fflush(stdout);
        return;
    }

    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
    {
        // Visible console: write to console (no newline)
        WriteConsoleUtf8AsWide(g_hStdout, std::string(text));
        return;
    }

    // Fallback: write to std::cout
    std::cout << text;
    std::fflush(stdout);
#else
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open;

    if (isPipe && !prefer_console)
    {
        std::cout << text;
        std::fflush(stdout);
        return;
    }

    if (g_is_open)
    {
        std::fwrite(text.data(), 1, text.size(), stderr);
        std::fflush(stderr);
    }
    else if (isPipe)
    {
        std::cout << text;
        std::fflush(stdout);
    }
#endif
}

// ============================================================================
// iconsole_print_line
//  - Same as iconsole_print but append newline (Windows uses CRLF)
// ============================================================================
void iconsole_print_line(std::string_view text)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);

    // Prepare newline forms for runtime(pipe) and console
    std::string runtime_line(text);
    runtime_line += '\n'; // for std::cout / captured scenarios

    std::string console_line(text);
    console_line += "\r\n"; // for Windows console window

    write_to_log_file(runtime_line); // log file uses LF

#ifdef OS_WINDOWS
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open || g_allocated_console;

    // Pipe and no visible console: write runtime_line (LF) only
    if (isPipe && !prefer_console)
    {
        if (g_force_ansi)
        {
            std::cout << runtime_line;
        }
        else
        {
            std::cout << runtime_line;
        }
        std::fflush(stdout);
        return;
    }

    // Visible console: write console_line (CRLF)
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
    {
        WriteConsoleUtf8AsWide(g_hStdout, console_line);
        return;
    }

    // Fallback: write runtime_line
    std::cout << runtime_line;
    std::fflush(stdout);
#else
    // POSIX: use LF, write to log and decide between stderr or stdout based on g_is_open
    std::string runtime_line = text;
    runtime_line += '\n';
    write_to_log_file(runtime_line);

    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool prefer_console = g_is_open;

    if (isPipe && !prefer_console)
    {
        std::cout << runtime_line;
        std::fflush(stdout);
        return;
    }

    if (g_is_open)
    {
        std::fwrite(runtime_line.data(), 1, runtime_line.size(), stderr);
        std::fflush(stderr);
    }
    else
    {
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
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
    {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        COORD home = {0, 0};
        if (::GetConsoleScreenBufferInfo(g_hStdout, &csbi))
        {
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
    if (g_allocated_console)
    {
        if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
        {
            CloseHandle(g_hStdout);
        }
        ::FreeConsole();
        g_hStdout = INVALID_HANDLE_VALUE;
        g_hStdin = INVALID_HANDLE_VALUE;
        g_allocated_console = false;
    }
    else if (g_attached_console)
    {
        // Detach from attached parent console (optional behavior)
        ::FreeConsole();
        g_hStdout = INVALID_HANDLE_VALUE;
        g_hStdin = INVALID_HANDLE_VALUE;
        g_attached_console = false;
    }
    else
    {
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
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
    {
        int wlen = ::MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), nullptr, 0);
        if (wlen > 0)
        {
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
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
    {
        WORD attr = static_cast<WORD>(color & 0x0F);
        ::SetConsoleTextAttribute(g_hStdout, attr);
    }
#else
    const char *ansi_code;
    switch (color & 0x0F)
    {
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
//  - Compose message and output according to rules (runtime stdout or native console)
// ============================================================================
void iconsole_log(std::int32_t level, std::string_view text)
{
    std::lock_guard<std::mutex> lock(g_console_mutex);
    if (level < g_log_level)
        return;

    std::string plain;
    if (g_timestamp_enabled)
    {
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

    // Pipe and no visible console: output plain text (no ANSI) unless forced
    if (isPipe && !hasConsole)
    {
        if (g_force_ansi)
        {
            const char *color = ansi_color_for_level(level);
            const char *reset = ansi_reset();
            std::cout << color << runtime_line << reset;
        }
        else
        {
            std::cout << runtime_line;
        }
        std::fflush(stdout);
        return;
    }

    // Visible console: use native color
    if (g_hStdout != INVALID_HANDLE_VALUE && g_hStdout != nullptr)
    {
        WORD oldAttr = g_default_attr;
        WORD attr = static_cast<WORD>(get_color_for_level(level) & 0x0F);
        ::SetConsoleTextAttribute(g_hStdout, attr);
        WriteConsoleUtf8AsWide(g_hStdout, console_line);
        ::SetConsoleTextAttribute(g_hStdout, oldAttr);
        return;
    }

    // Other cases (e.g., pipe not caught above): output plain text
    std::cout << runtime_line;
    std::fflush(stdout);
#else
    std::string runtime_line = plain;
    runtime_line += '\n';
    write_to_log_file(runtime_line);
    bool isPipe = runtime_stdout_is_redirected_or_pipe();
    bool hasConsole = g_is_open;
    if (isPipe && !hasConsole)
    {
        std::cout << runtime_line;
        std::fflush(stdout);
        return;
    }
    if (g_is_open)
    {
        const char *ansi_code;
        switch (get_color_for_level(level) & 0x0F)
        {
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
    }
    else
    {
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

    if (g_user_log_file)
    {
        std::fclose(g_user_log_file);
        g_user_log_file = nullptr;
    }

    g_user_log_path = std::string(path);
    g_user_log_file = std::fopen(g_user_log_path.c_str(), "w");
    if (g_user_log_file)
    {
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
    if (g_user_log_file)
    {
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
    while (::PeekConsoleInput(g_hStdin, &ir, 1, &read) && read > 0)
    {
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.uChar.AsciiChar != 0)
        {
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
    while (true)
    {
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

    while (chars_read > 0 && (buffer[chars_read - 1] == L'\n' || buffer[chars_read - 1] == L'\r'))
    {
        chars_read--;
    }
    buffer[chars_read] = L'\0';

    int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(chars_read), nullptr, 0, nullptr, nullptr);
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
    if (timeout_ms < 0)
    {
        tv.tv_sec = 0;
        tv.tv_usec = 0;
    }
    else
    {
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
    while (::PeekConsoleInput(g_hStdin, &ir, 1, &read) && read > 0)
    {
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown)
        {
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

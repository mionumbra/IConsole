#include "IConsole_native.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>

#ifdef OS_WINDOWS
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace
{
    std::mutex g_mutex;
    bool g_is_open = false;
    std::int32_t g_log_level = 0;
    bool g_timestamp_enabled = false;
    bool g_force_ansi = false;
    std::FILE* g_log_file = nullptr;
    std::string g_log_path;

#ifdef OS_WINDOWS
    HANDLE g_hOut = INVALID_HANDLE_VALUE;
    HANDLE g_hIn = INVALID_HANDLE_VALUE;
    WORD g_default_attr = 7;
    bool g_attached = false;
    bool g_allocated = false;
    bool g_owns_out_handle = false;
#endif

#ifdef OS_WINDOWS
    static bool stdout_is_console()
    {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr)
            return false;
        DWORD mode = 0;
        if (::GetConsoleMode(h, &mode))
            return true;
        HWND hwnd = ::GetConsoleWindow();
        return hwnd != nullptr && ::IsWindowVisible(hwnd) != FALSE;
    }

    static bool stdout_is_pipe_or_disk()
    {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr)
            return false;
        const DWORD ft = ::GetFileType(h);
        return ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK;
    }

    static void write_console_utf8(HANDLE h, std::string_view utf8)
    {
        if (h == INVALID_HANDLE_VALUE || h == nullptr || utf8.empty())
            return;
        const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0)
            return;
        std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wbuf.data(), wlen);
        DWORD written = 0;
        ::WriteConsoleW(h, wbuf.c_str(), static_cast<DWORD>(wlen), &written, nullptr);
    }

    static void enable_vt(HANDLE h)
    {
        DWORD mode = 0;
        if (h != INVALID_HANDLE_VALUE && h != nullptr && ::GetConsoleMode(h, &mode))
            ::SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
#else
    static bool stdout_is_console() { return ::isatty(STDOUT_FILENO) != 0; }
    static bool stdout_is_pipe_or_disk() { return !stdout_is_console(); }
#endif

    static std::string timestamp()
    {
        const auto now = std::chrono::system_clock::now();
        const auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf{};
#ifdef OS_WINDOWS
        localtime_s(&tm_buf, &t);
#else
        localtime_r(&t, &tm_buf);
#endif
        char buf[16];
        std::snprintf(buf, sizeof(buf), "[%02d:%02d:%02d]", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
        return buf;
    }

    static std::string_view level_prefix(std::int32_t level)
    {
        switch (level)
        {
        case 0: return "[DEBUG] ";
        case 1: return "[INFO]  ";
        case 2: return "[WARN]  ";
        case 3: return "[ERROR] ";
        default: return "[LOG]   ";
        }
    }

    static std::int32_t level_color(std::int32_t level)
    {
        switch (level)
        {
        case 0: return 8;
        case 1: return 7;
        case 2: return 6;
        case 3: return 4;
        default: return 7;
        }
    }

    static const char* ansi_for_level(std::int32_t level)
    {
        switch (level)
        {
        case 0: return "\x1b[90m";
        case 1: return "\x1b[37m";
        case 2: return "\x1b[33m";
        case 3: return "\x1b[31m";
        default: return "\x1b[37m";
        }
    }

    [[maybe_unused]] static const char* ansi_for_color(std::int32_t color)
    {
        switch (color & 0x0F)
        {
        case 0: return "\x1b[30m";
        case 1: return "\x1b[34m";
        case 2: return "\x1b[32m";
        case 3: return "\x1b[36m";
        case 4: return "\x1b[31m";
        case 5: return "\x1b[35m";
        case 6: return "\x1b[33m";
        case 7: return "\x1b[37m";
        case 8: return "\x1b[90m";
        case 9: return "\x1b[94m";
        case 10: return "\x1b[92m";
        case 11: return "\x1b[96m";
        case 12: return "\x1b[91m";
        case 13: return "\x1b[95m";
        case 14: return "\x1b[93m";
        case 15: return "\x1b[97m";
        default: return "\x1b[37m";
        }
    }

    static void write_log_file(std::string_view text)
    {
        if (!g_log_file)
            return;
        std::fwrite(text.data(), 1, text.size(), g_log_file);
        std::fflush(g_log_file);
    }

    static void open_log_file_unlocked(std::string_view path, bool append)
    {
        if (g_log_file)
        {
            std::fclose(g_log_file);
            g_log_file = nullptr;
        }
        g_log_path.assign(path);
        g_log_file = std::fopen(g_log_path.c_str(), append ? "a" : "w");
        if (!g_log_file)
        {
            g_log_path.clear();
            return;
        }
        std::fprintf(g_log_file, "=== IConsole Log %s ===\n", append ? "(append)" : "(new)");
        std::fprintf(g_log_file, "Path: %s\n", g_log_path.c_str());
        std::fprintf(g_log_file, "========================\n");
        std::fflush(g_log_file);
    }

    static void close_log_file_unlocked()
    {
        if (!g_log_file)
            return;
        std::fprintf(g_log_file, "=== Log file closed ===\n");
        std::fflush(g_log_file);
        std::fclose(g_log_file);
        g_log_file = nullptr;
        g_log_path.clear();
    }

    // Prefer visible console when open; else IDE pipe via cout; else cout fallback.
    static void emit_text(std::string_view text, std::string_view console_text, std::int32_t color_attr, bool colored)
    {
        write_log_file(text);

#ifdef OS_WINDOWS
        const bool is_pipe = stdout_is_pipe_or_disk();
        const bool has_console = g_is_open && g_hOut != INVALID_HANDLE_VALUE && g_hOut != nullptr;

        if (is_pipe && !has_console)
        {
            if (g_force_ansi && colored)
                std::cout << ansi_for_level(color_attr) << text << "\x1b[0m";
            else
                std::cout << text;
            std::fflush(stdout);
            return;
        }

        if (has_console)
        {
            WORD old = g_default_attr;
            if (colored)
            {
                CONSOLE_SCREEN_BUFFER_INFO csbi{};
                if (::GetConsoleScreenBufferInfo(g_hOut, &csbi))
                    old = csbi.wAttributes;
                ::SetConsoleTextAttribute(g_hOut, static_cast<WORD>(color_attr & 0x0F));
            }
            write_console_utf8(g_hOut, console_text);
            if (colored)
                ::SetConsoleTextAttribute(g_hOut, old);
            return;
        }

        std::cout << text;
        std::fflush(stdout);
#else
        const bool is_pipe = stdout_is_pipe_or_disk();
        if (is_pipe && !g_is_open)
        {
            if (g_force_ansi && colored)
                std::cout << ansi_for_level(color_attr) << text << "\x1b[0m";
            else
                std::cout << text;
            std::fflush(stdout);
            return;
        }
        if (g_is_open)
        {
            if (colored)
                std::fprintf(stderr, "%s%.*s\x1b[0m", ansi_for_level(color_attr), static_cast<int>(text.size()), text.data());
            else
                std::fwrite(text.data(), 1, text.size(), stderr);
            std::fflush(stderr);
            return;
        }
        std::cout << text;
        std::fflush(stdout);
#endif
    }

    static void emit_plain(std::string_view text, bool newline)
    {
        std::string runtime(text);
        std::string console(text);
        if (newline)
        {
            runtime.push_back('\n');
            console.append("\r\n");
        }
        emit_text(runtime, console, 7, false);
    }

    static void emit_log_unlocked(std::int32_t level, std::string_view text)
    {
        if (level < g_log_level)
            return;

        std::string plain;
        plain.reserve(text.size() + 32);
        if (g_timestamp_enabled)
        {
            plain += timestamp();
            plain.push_back(' ');
        }
        plain += level_prefix(level);
        plain.append(text);
        plain.push_back('\n');

        std::string console = plain;
        // convert LF -> CRLF for Windows console path
        if (!console.empty() && console.back() == '\n')
        {
            console.pop_back();
            console.append("\r\n");
        }

        emit_text(plain, console, level_color(level), true);
    }

#ifdef OS_WINDOWS
    static void bind_existing_console()
    {
        g_hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_hIn = ::GetStdHandle(STD_INPUT_HANDLE);
        g_owns_out_handle = false;
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (g_hOut != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(g_hOut, &csbi))
            g_default_attr = csbi.wAttributes;
        enable_vt(g_hOut);
        g_attached = false;
        g_allocated = false;
        g_is_open = true;
    }
#endif
} // namespace

void iconsole_open()
{
    std::lock_guard lock(g_mutex);
    if (g_is_open)
        return;

#ifdef OS_WINDOWS
    if (stdout_is_console())
    {
        bind_existing_console();
        return;
    }

    if (::AttachConsole(ATTACH_PARENT_PROCESS))
    {
        g_hOut = ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_hIn = ::GetStdHandle(STD_INPUT_HANDLE);
        g_owns_out_handle = false;
        ::SetConsoleOutputCP(CP_UTF8);
        if (g_hIn != INVALID_HANDLE_VALUE && g_hIn != nullptr)
            ::SetConsoleMode(g_hIn, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);

        HWND hwnd = ::GetConsoleWindow();
        const bool visible = hwnd != nullptr && ::IsWindowVisible(hwnd) != FALSE;
        if (!visible)
        {
            // Parent attached via pipe (IDE capture). Do not FreeConsole/AllocConsole.
            g_attached = true;
            g_allocated = false;
            g_is_open = false;
            g_hOut = INVALID_HANDLE_VALUE;
            g_hIn = INVALID_HANDLE_VALUE;
            return;
        }

        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (g_hOut != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(g_hOut, &csbi))
            g_default_attr = csbi.wAttributes;
        enable_vt(g_hOut);
        g_attached = true;
        g_allocated = false;
        g_is_open = true;
        write_console_utf8(g_hOut, "\r\n");
        return;
    }

    if (::AllocConsole())
    {
        HANDLE h = ::CreateFileA(
            "CONOUT$",
            GENERIC_WRITE | GENERIC_READ,
            FILE_SHARE_WRITE | FILE_SHARE_READ,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);

        g_hOut = (h != INVALID_HANDLE_VALUE && h != nullptr) ? h : ::GetStdHandle(STD_OUTPUT_HANDLE);
        g_owns_out_handle = (h != INVALID_HANDLE_VALUE && h != nullptr);
        g_hIn = ::GetStdHandle(STD_INPUT_HANDLE);

#ifdef _MSC_VER
        FILE* fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
#else
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
#endif
        ::SetConsoleOutputCP(CP_UTF8);
        if (g_hIn != INVALID_HANDLE_VALUE && g_hIn != nullptr)
            ::SetConsoleMode(g_hIn, ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);

        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (g_hOut != INVALID_HANDLE_VALUE && ::GetConsoleScreenBufferInfo(g_hOut, &csbi))
            g_default_attr = csbi.wAttributes;
        enable_vt(g_hOut);

        g_allocated = true;
        g_attached = false;
        g_is_open = true;
        return;
    }
#else
    g_is_open = true;
#endif
}

void iconsole_close()
{
    std::lock_guard lock(g_mutex);

#ifdef OS_WINDOWS
    if (g_allocated)
    {
        if (g_owns_out_handle && g_hOut != INVALID_HANDLE_VALUE && g_hOut != nullptr)
            ::CloseHandle(g_hOut);
        ::FreeConsole();
        g_owns_out_handle = false;
        g_allocated = false;
    }
    else if (g_attached)
    {
        ::FreeConsole();
        g_attached = false;
    }
    g_hOut = INVALID_HANDLE_VALUE;
    g_hIn = INVALID_HANDLE_VALUE;
#endif
    g_is_open = false;
}

void iconsole_shutdown()
{
    // close + log without re-entrant lock issues
    {
        std::lock_guard lock(g_mutex);
        close_log_file_unlocked();
    }
    iconsole_close();
}

bool iconsole_is_open()
{
    std::lock_guard lock(g_mutex);
    return g_is_open;
}

void iconsole_print(std::string_view text)
{
    std::lock_guard lock(g_mutex);
    emit_plain(text, false);
}

void iconsole_print_line(std::string_view text)
{
    std::lock_guard lock(g_mutex);
    emit_plain(text, true);
}

void iconsole_clear()
{
    std::lock_guard lock(g_mutex);
    if (!g_is_open)
        return;

#ifdef OS_WINDOWS
    if (g_hOut == INVALID_HANDLE_VALUE || g_hOut == nullptr)
        return;
    CONSOLE_SCREEN_BUFFER_INFO csbi{};
    COORD home{0, 0};
    if (!::GetConsoleScreenBufferInfo(g_hOut, &csbi))
        return;
    const DWORD size = static_cast<DWORD>(csbi.dwSize.X) * static_cast<DWORD>(csbi.dwSize.Y);
    DWORD written = 0;
    ::FillConsoleOutputCharacterW(g_hOut, L' ', size, home, &written);
    ::FillConsoleOutputAttribute(g_hOut, g_default_attr, size, home, &written);
    ::SetConsoleCursorPosition(g_hOut, home);
#else
    std::fputs("\x1b[2J\x1b[H", stderr);
    std::fflush(stderr);
#endif
}

void iconsole_flush()
{
    std::lock_guard lock(g_mutex);
    std::fflush(stdout);
    std::fflush(stderr);
    if (g_log_file)
        std::fflush(g_log_file);
}

void iconsole_set_title(std::string_view title)
{
    std::lock_guard lock(g_mutex);
    if (!g_is_open)
        return;
#ifdef OS_WINDOWS
    const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), nullptr, 0);
    if (wlen <= 0)
        return;
    std::wstring wtitle(static_cast<size_t>(wlen), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, title.data(), static_cast<int>(title.size()), wtitle.data(), wlen);
    ::SetConsoleTitleW(wtitle.c_str());
#else
    (void)title;
#endif
}

void iconsole_set_color(std::int32_t color)
{
    std::lock_guard lock(g_mutex);
    if (!g_is_open)
        return;
#ifdef OS_WINDOWS
    if (g_hOut != INVALID_HANDLE_VALUE && g_hOut != nullptr)
        ::SetConsoleTextAttribute(g_hOut, static_cast<WORD>(color & 0x0F));
#else
    std::fputs(ansi_for_color(color), stderr);
    std::fflush(stderr);
#endif
}

void iconsole_reset_color()
{
    std::lock_guard lock(g_mutex);
    if (!g_is_open)
        return;
#ifdef OS_WINDOWS
    if (g_hOut != INVALID_HANDLE_VALUE && g_hOut != nullptr)
        ::SetConsoleTextAttribute(g_hOut, g_default_attr);
#else
    std::fputs("\x1b[0m", stderr);
    std::fflush(stderr);
#endif
}

void iconsole_set_force_ansi(bool enabled)
{
    std::lock_guard lock(g_mutex);
    g_force_ansi = enabled;
}

bool iconsole_force_ansi_enabled()
{
    std::lock_guard lock(g_mutex);
    return g_force_ansi;
}

void iconsole_set_log_level(std::int32_t level)
{
    std::lock_guard lock(g_mutex);
    if (level < 0)
        level = 0;
    if (level > 3)
        level = 3;
    g_log_level = level;
}

std::int32_t iconsole_get_log_level()
{
    std::lock_guard lock(g_mutex);
    return g_log_level;
}

void iconsole_log(std::int32_t level, std::string_view text)
{
    std::lock_guard lock(g_mutex);
    emit_log_unlocked(level, text);
}

void iconsole_debug(std::string_view text)
{
    std::lock_guard lock(g_mutex);
    emit_log_unlocked(0, text);
}

void iconsole_info(std::string_view text)
{
    std::lock_guard lock(g_mutex);
    emit_log_unlocked(1, text);
}

void iconsole_warn(std::string_view text)
{
    std::lock_guard lock(g_mutex);
    emit_log_unlocked(2, text);
}

void iconsole_error(std::string_view text)
{
    std::lock_guard lock(g_mutex);
    emit_log_unlocked(3, text);
}

void iconsole_set_timestamp(bool enabled)
{
    std::lock_guard lock(g_mutex);
    g_timestamp_enabled = enabled;
}

bool iconsole_timestamp_enabled()
{
    std::lock_guard lock(g_mutex);
    return g_timestamp_enabled;
}

void iconsole_set_log_file(std::string_view path)
{
    std::lock_guard lock(g_mutex);
    open_log_file_unlocked(path, false);
}

void iconsole_set_log_file_append(std::string_view path)
{
    std::lock_guard lock(g_mutex);
    open_log_file_unlocked(path, true);
}

std::string iconsole_get_log_file()
{
    std::lock_guard lock(g_mutex);
    return g_log_path;
}

void iconsole_close_log_file()
{
    std::lock_guard lock(g_mutex);
    close_log_file_unlocked();
}

bool iconsole_log_file_open()
{
    std::lock_guard lock(g_mutex);
    return g_log_file != nullptr;
}

bool iconsole_has_input()
{
    std::lock_guard lock(g_mutex);
    if (!g_is_open)
        return false;

#ifdef OS_WINDOWS
    if (g_hIn == INVALID_HANDLE_VALUE || g_hIn == nullptr)
        return false;
    DWORD events = 0;
    if (!::GetNumberOfConsoleInputEvents(g_hIn, &events) || events == 0)
        return false;
    INPUT_RECORD ir{};
    DWORD read = 0;
    while (::PeekConsoleInputW(g_hIn, &ir, 1, &read) && read > 0)
    {
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown && ir.Event.KeyEvent.uChar.UnicodeChar != 0)
            return true;
        ::ReadConsoleInputW(g_hIn, &ir, 1, &read);
    }
    return false;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{0, 0};
    return ::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0;
#endif
}

std::string iconsole_read_line(std::int32_t timeout_ms)
{
    std::lock_guard lock(g_mutex);
    if (!g_is_open)
        return {};

#ifdef OS_WINDOWS
    if (g_hIn == INVALID_HANDLE_VALUE || g_hIn == nullptr)
        return {};

    const DWORD start = ::GetTickCount();
    for (;;)
    {
        DWORD events = 0;
        if (!::GetNumberOfConsoleInputEvents(g_hIn, &events))
            return {};
        if (events > 0)
            break;
        if (timeout_ms >= 0 && (::GetTickCount() - start) >= static_cast<DWORD>(timeout_ms))
            return {};
        ::Sleep(1);
    }

    wchar_t buffer[4096];
    DWORD chars_read = 0;
    if (!::ReadConsoleW(g_hIn, buffer, 4095, &chars_read, nullptr) || chars_read == 0)
        return {};
    while (chars_read > 0 && (buffer[chars_read - 1] == L'\n' || buffer[chars_read - 1] == L'\r'))
        --chars_read;
    buffer[chars_read] = L'\0';

    const int utf8_len = ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(chars_read), nullptr, 0, nullptr, nullptr);
    if (utf8_len <= 0)
        return {};
    std::string result(static_cast<size_t>(utf8_len), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(chars_read), result.data(), utf8_len, nullptr, nullptr);
    return result;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{};
    timeval* ptv = nullptr;
    if (timeout_ms >= 0)
    {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }
    if (::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, ptv) <= 0)
        return {};
    char buffer[4096];
    const ssize_t bytes = ::read(STDIN_FILENO, buffer, sizeof(buffer) - 1);
    if (bytes <= 0)
        return {};
    ssize_t n = bytes;
    while (n > 0 && (buffer[n - 1] == '\n' || buffer[n - 1] == '\r'))
        --n;
    return std::string(buffer, static_cast<size_t>(n));
#endif
}

std::int32_t iconsole_read_key()
{
    std::lock_guard lock(g_mutex);
    if (!g_is_open)
        return 0;

#ifdef OS_WINDOWS
    if (g_hIn == INVALID_HANDLE_VALUE || g_hIn == nullptr)
        return 0;
    INPUT_RECORD ir{};
    DWORD read = 0;
    while (::PeekConsoleInputW(g_hIn, &ir, 1, &read) && read > 0)
    {
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown)
        {
            ::ReadConsoleInputW(g_hIn, &ir, 1, &read);
            return static_cast<std::int32_t>(ir.Event.KeyEvent.wVirtualKeyCode);
        }
        ::ReadConsoleInputW(g_hIn, &ir, 1, &read);
    }
    return 0;
#else
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    timeval tv{0, 0};
    if (::select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
        return 0;
    unsigned char c = 0;
    if (::read(STDIN_FILENO, &c, 1) <= 0)
        return 0;
    return static_cast<std::int32_t>(c);
#endif
}

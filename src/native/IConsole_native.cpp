#include "IConsole_native.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <iostream>
#include <limits>
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
    std::wstring g_input_line;
    wchar_t g_input_high_surrogate = 0;
    std::deque<std::string> g_input_lines;
    std::deque<std::int32_t> g_input_keys;
#endif

#ifdef OS_WINDOWS
    static bool stdout_is_pipe_or_disk()
    {
        HANDLE h = ::GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr)
            return false;
        const DWORD ft = ::GetFileType(h);
        return ft == FILE_TYPE_PIPE || ft == FILE_TYPE_DISK;
    }

    static std::wstring utf8_to_wide(std::string_view utf8)
    {
        if (utf8.empty() || utf8.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return {};
        const int wlen = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (wlen <= 0)
            return {};
        std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
        if (::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wbuf.data(), wlen) != wlen)
            return {};
        return wbuf;
    }

    static std::string wide_to_utf8(std::wstring_view wide)
    {
        if (wide.empty() || wide.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return {};
        const int length = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (length <= 0)
            return {};
        std::string result(static_cast<std::size_t>(length), '\0');
        if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), length, nullptr, nullptr) != length)
            return {};
        return result;
    }

    static void write_console_wide(HANDLE h, std::wstring_view text)
    {
        std::size_t offset = 0;
        while (offset < text.size())
        {
            const DWORD length = static_cast<DWORD>(std::min<std::size_t>(text.size() - offset, 32767));
            DWORD written = 0;
            if (!::WriteConsoleW(h, text.data() + offset, length, &written, nullptr) || written == 0)
                return;
            offset += written;
        }
    }

    static void write_console_utf8(HANDLE h, std::string_view utf8)
    {
        if (h == INVALID_HANDLE_VALUE || h == nullptr || utf8.empty())
            return;
        write_console_wide(h, utf8_to_wide(utf8));
    }

    static bool open_console_handles()
    {
        HANDLE out = ::CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (out == INVALID_HANDLE_VALUE)
            return false;
        HANDLE in = ::CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (in == INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(out);
            return false;
        }

        g_hOut = out;
        g_hIn = in;
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (::GetConsoleScreenBufferInfo(g_hOut, &info))
            g_default_attr = info.wAttributes;
        return true;
    }

    static void close_console_unlocked()
    {
        if (g_hOut != INVALID_HANDLE_VALUE && g_hOut != nullptr)
            ::CloseHandle(g_hOut);
        if (g_hIn != INVALID_HANDLE_VALUE && g_hIn != nullptr)
            ::CloseHandle(g_hIn);
        g_hOut = INVALID_HANDLE_VALUE;
        g_hIn = INVALID_HANDLE_VALUE;

        if (g_allocated || g_attached)
            ::FreeConsole();
        g_allocated = false;
        g_attached = false;
        g_input_line.clear();
        g_input_high_surrogate = 0;
        g_input_lines.clear();
        g_input_keys.clear();
        g_is_open = false;
    }

    static void poll_console_input_unlocked()
    {
        if (!g_is_open || g_hIn == INVALID_HANDLE_VALUE || g_hIn == nullptr)
            return;

        INPUT_RECORD events[64];
        DWORD remaining = 256;
        while (remaining > 0)
        {
            DWORD available = 0;
            if (!::GetNumberOfConsoleInputEvents(g_hIn, &available) || available == 0)
                return;

            DWORD read = 0;
            if (!::ReadConsoleInputW(g_hIn, events, std::min<DWORD>({available, 64, remaining}), &read))
                return;
            remaining -= read;

            for (DWORD i = 0; i < read; ++i)
            {
                const INPUT_RECORD& event = events[i];
                if (event.EventType != KEY_EVENT || !event.Event.KeyEvent.bKeyDown)
                    continue;

                const KEY_EVENT_RECORD& key = event.Event.KeyEvent;
                const WORD repeats = std::max<WORD>(key.wRepeatCount, 1);
                for (WORD repeat = 0; repeat < repeats; ++repeat)
                {
                    if (g_input_keys.size() == 256)
                        g_input_keys.pop_front();
                    g_input_keys.push_back(static_cast<std::int32_t>(key.wVirtualKeyCode));

                    const wchar_t character = key.uChar.UnicodeChar;
                    if (character == L'\r')
                    {
                        g_input_high_surrogate = 0;
                        if (g_input_lines.size() == 64)
                            g_input_lines.pop_front();
                        g_input_lines.push_back(wide_to_utf8(g_input_line));
                        g_input_line.clear();
                        write_console_wide(g_hOut, L"\r\n");
                    }
                    else if (character == L'\b')
                    {
                        if (g_input_high_surrogate != 0)
                        {
                            g_input_high_surrogate = 0;
                        }
                        else if (!g_input_line.empty())
                        {
                            g_input_line.pop_back();
                            if (!g_input_line.empty() && g_input_line.back() >= 0xD800 && g_input_line.back() <= 0xDBFF)
                                g_input_line.pop_back();
                            write_console_wide(g_hOut, L"\b \b");
                        }
                    }
                    else if (character >= 0xD800 && character <= 0xDBFF)
                    {
                        g_input_high_surrogate = character;
                    }
                    else if (character >= 0xDC00 && character <= 0xDFFF)
                    {
                        if (g_input_high_surrogate != 0 && g_input_line.size() <= 1024 * 1024 - 2)
                        {
                            const wchar_t pair[] = {g_input_high_surrogate, character};
                            g_input_line.append(pair, 2);
                            write_console_wide(g_hOut, std::wstring_view(pair, 2));
                        }
                        g_input_high_surrogate = 0;
                    }
                    else if (character >= L' ')
                    {
                        g_input_high_surrogate = 0;
                        if (g_input_line.size() < 1024 * 1024)
                        {
                            g_input_line.push_back(character);
                            write_console_wide(g_hOut, std::wstring_view(&character, 1));
                        }
                    }
                }
            }
        }
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
        if (std::fwrite(text.data(), 1, text.size(), g_log_file) != text.size() || std::fflush(g_log_file) != 0)
        {
            std::fclose(g_log_file);
            g_log_file = nullptr;
            g_log_path.clear();
        }
    }

    static void open_log_file_unlocked(std::string_view path, bool append)
    {
        if (path.empty())
            return;
        if (!append && g_log_file && path == g_log_path)
            return;

        std::FILE* next = nullptr;
#ifdef OS_WINDOWS
        const std::wstring wide_path = utf8_to_wide(path);
        if (wide_path.empty() || _wfopen_s(&next, wide_path.c_str(), append ? L"ab" : L"wb") != 0)
            return;
#else
        next = std::fopen(std::string(path).c_str(), append ? "ab" : "wb");
#endif
        if (!next)
            return;

        const std::string next_path(path);
        if (std::fprintf(next, "=== IConsole Log %s ===\n", append ? "(append)" : "(new)") < 0 ||
            std::fprintf(next, "Path: %s\n", next_path.c_str()) < 0 ||
            std::fprintf(next, "========================\n") < 0 || std::fflush(next) != 0)
        {
            std::fclose(next);
            return;
        }

        if (g_log_file)
            std::fclose(g_log_file);
        g_log_file = next;
        g_log_path = next_path;
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

    // Prefer an open console; otherwise preserve the Runner/IDE stdout path.
    static void emit_text(std::string_view text, std::string_view console_text, std::int32_t color_attr, bool colored)
    {
        write_log_file(text);

#ifdef OS_WINDOWS
        const bool is_pipe = stdout_is_pipe_or_disk();
        const bool has_console = g_is_open && g_hOut != INVALID_HANDLE_VALUE && g_hOut != nullptr;

        if (is_pipe && !has_console)
        {
            if (g_force_ansi && colored)
                std::cout << ansi_for_color(color_attr) << text << "\x1b[0m";
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
                ::SetConsoleTextAttribute(g_hOut, static_cast<WORD>((old & 0xFFF0) | (color_attr & 0x0F)));
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
                std::cout << ansi_for_color(color_attr) << text << "\x1b[0m";
            else
                std::cout << text;
            std::fflush(stdout);
            return;
        }
        if (g_is_open)
        {
            if (colored)
                std::fprintf(stderr, "%s%.*s\x1b[0m", ansi_for_color(color_attr), static_cast<int>(text.size()), text.data());
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

} // namespace

void iconsole_open()
{
    std::lock_guard lock(g_mutex);
    if (g_is_open)
        return;

#ifdef OS_WINDOWS
    // IDEs and redirected command-line launches already provide the desired
    // output channel. Do not attach or allocate a console in that case.
    if (stdout_is_pipe_or_disk())
    {
        g_is_open = true;
        return;
    }

    if (open_console_handles())
    {
        g_is_open = true;
        return;
    }

    if (::AttachConsole(ATTACH_PARENT_PROCESS))
    {
        if (!open_console_handles())
        {
            ::FreeConsole();
        }
        else
        {
            g_attached = true;
            g_is_open = true;
            write_console_utf8(g_hOut, "\r\n");
            return;
        }
    }

    if (::AllocConsole())
    {
        if (open_console_handles())
        {
            g_allocated = true;
            g_is_open = true;
            return;
        }
        ::FreeConsole();
    }
#else
    g_is_open = true;
#endif
}

void iconsole_close()
{
    std::lock_guard lock(g_mutex);

#ifdef OS_WINDOWS
    close_console_unlocked();
#else
    g_is_open = false;
#endif
}

void iconsole_shutdown()
{
    std::lock_guard lock(g_mutex);
    close_log_file_unlocked();
#ifdef OS_WINDOWS
    close_console_unlocked();
#else
    g_is_open = false;
#endif
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
    {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (::GetConsoleScreenBufferInfo(g_hOut, &info))
            ::SetConsoleTextAttribute(g_hOut, static_cast<WORD>((info.wAttributes & 0xFFF0) | (color & 0x0F)));
    }
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
    level = std::clamp<std::int32_t>(level, 0, 3);
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
    poll_console_input_unlocked();
    return !g_input_lines.empty();
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
    std::unique_lock lock(g_mutex);
    if (!g_is_open)
        return {};

#ifdef OS_WINDOWS
    if (g_hIn == INVALID_HANDLE_VALUE || g_hIn == nullptr)
        return {};

    const ULONGLONG start = ::GetTickCount64();
    for (;;)
    {
        poll_console_input_unlocked();
        if (!g_input_lines.empty())
        {
            std::string result = std::move(g_input_lines.front());
            g_input_lines.pop_front();
            return result;
        }
        if (!g_is_open)
            return {};
        if (timeout_ms >= 0 && (::GetTickCount64() - start) >= static_cast<ULONGLONG>(timeout_ms))
            return {};
        lock.unlock();
        ::Sleep(1);
        lock.lock();
    }
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
    poll_console_input_unlocked();
    if (g_input_keys.empty())
        return 0;
    const std::int32_t key = g_input_keys.front();
    g_input_keys.pop_front();
    return key;
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

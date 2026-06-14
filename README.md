**Note:** Linux and macOS platforms are untested.

# IConsole: Native Console Extension for GameMaker

A compact native console extension for GameMaker 2026 that provides colored logging, optional timestamps, file logging, and basic interactive input. Designed to fill the gap where the GMS2 runtime lacks a built-in console.

---

## Overview

**What it does**

- Creates a native console window on Windows and provides cross‑platform output fallbacks for POSIX systems.  
- Supports **16 colors** and automatic coloring by log level.  
- Provides **log level filtering** (DEBUG / INFO / WARN / ERROR).  
- Optional **timestamp** prefix for log lines.  
- Writes logs to a user-specified file in addition to console output.  
- Non-blocking input checks and timed line reads for simple interactive flows.  
- Thread-safe internal implementation.

**Design notes**

- On Windows the extension prefers native console output (colored via WinAPI).  
- When stdout is captured (IDE or pipe), the extension writes plain text to stdout to avoid ANSI escape pollution.  
- `iconsole_set_force_ansi(true)` lets callers force ANSI sequences when they know the consumer supports them. Use with caution.

---

## Quick Start

### Integration in GameMaker

1. Create an extension in GameMaker (right click → Create Extension).  
2. Add `extgen/IConsole.dll` for Windows platform.  
3. Add `scripts/IConsole/IConsole.gml` as the GML binding script.

### Minimal GML Example

```gml
// Open console and set title
iconsole_open();
iconsole_set_title("My Game Console");

// Print a line
iconsole_print_line("Game started!");

// Log with levels
iconsole_set_log_level(1); // show INFO and above
iconsole_log(1, "Game initialized");
iconsole_log(2, "Low memory warning");
iconsole_log(3, "Load failed");

// Timestamped logs
iconsole_set_timestamp(true);
iconsole_log(1, "Timestamped message");

// File logging
iconsole_set_log_file("game.log");

// Read input if available
if (iconsole_has_input()) {
    var s = iconsole_read_line(200);
    iconsole_print_line("You typed: " + s);
}

// Close when done
iconsole_close();
```

**Path note**: `iconsole_set_log_file` uses the process working directory for relative paths. Use absolute paths if needed.

---

## API Reference

### Basic Control

| Function | Description |
|---|---|
| `iconsole_open()` | Open or attach to a console |
| `iconsole_close()` | Close or detach the console |
| `iconsole_is_open()` | Returns whether a visible console is open |

### Output

| Function | Description |
|---|---|
| `iconsole_print(text)` | Print text without newline |
| `iconsole_print_line(text)` | Print text with newline |
| `iconsole_clear()` | Clear console contents |
| `iconsole_set_title(title)` | Set console window title |
| `iconsole_set_color(color)` | Set text color (0–15) for subsequent native console writes |

### Logging

| Function | Description |
|---|---|
| `iconsole_set_log_level(level)` | Set minimum log level (0=DEBUG,1=INFO,2=WARN,3=ERROR) |
| `iconsole_get_log_level()` | Get current log level |
| `iconsole_log(level, text)` | Log a message with automatic prefix and coloring |

**Color codes**

| Value | Color |
|---:|---|
| 0 | Black |
| 1 | Blue |
| 2 | Green |
| 3 | Cyan |
| 4 | Red |
| 5 | Magenta |
| 6 | Yellow |
| 7 | White |
| 8 | Gray |
| 9 | Bright Blue |
| 10 | Bright Green |
| 11 | Bright Cyan |
| 12 | Bright Red |
| 13 | Bright Magenta |
| 14 | Bright Yellow |
| 15 | Bright White |

### Timestamp and File Logging

| Function | Description |
|---|---|
| `iconsole_set_timestamp(enabled)` | Enable or disable timestamp prefix |
| `iconsole_timestamp_enabled()` | Query timestamp state |
| `iconsole_set_log_file(path)` | Open or replace log file (writes LF) |
| `iconsole_get_log_file()` | Get current log file path |
| `iconsole_close_log_file()` | Close the log file |
| `iconsole_log_file_open()` | Check if log file is open |

### Input

| Function | Description |
|---|---|
| `iconsole_has_input()` | Non-blocking check for available input |
| `iconsole_read_line(timeout_ms)` | Read a line, wait up to `timeout_ms` milliseconds (returns empty string on timeout) |
| `iconsole_read_key()` | Read a single key (non-blocking), returns virtual key code on Windows or byte value on POSIX |

### Advanced

| Function | Description |
|---|---|
| `iconsole_set_force_ansi(enable)` | Force ANSI output even when stdout is captured (use only if consumer supports ANSI) |

---

## Build Instructions

### Prerequisites

- **extgen** code generator (project-specific)  
- **CMake 3.25+**  
- **Visual Studio** with a compatible MSVC toolset (verify toolset version used in your environment)

### Windows Build Example

```bash
cd extgen

# Configure using provided preset
cmake --preset win-x64-debug

# Build
cmake --build --preset win-x64-debug

# Result: extgen/IConsole.dll
```

If presets are not available, use a standard CMake configure/build flow:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --config Debug
```

---

## Testing and Known Issues

**Tested platforms**

- **Windows** — tested with native console scenarios (CMD, PowerShell, double-click AllocConsole).  
- **Linux and macOS** — POSIX output paths implemented but not validated on real hardware. Use with caution.

**Known behaviors**

- When stdout is captured by an IDE or pipe, the extension writes plain text to stdout to avoid ANSI escape sequences appearing in logs. Use `iconsole_set_force_ansi(true)` only if you know the consumer supports ANSI.  
- When launched by double-click (AllocConsole), the extension avoids inserting an extra blank line at the top of the new window for visual cleanliness. When attached to an existing terminal, the extension writes a single blank line to separate the shell prompt from program output.  
- Log file lines use LF line endings for cross-platform readability.

**Troubleshooting**

- If colors appear as raw escape sequences in your IDE, enable `iconsole_set_force_ansi(true)` only if your IDE supports ANSI.  
- If `iconsole_open()` does not create a visible window when double-clicking, ensure the process has permission to create a console and that no parent process has captured stdout.  
- For file permission errors, verify the running process has write access to the target path.

---

## Contributing and License

**Contributing**

- Fork the repository, make changes in a feature branch, and open a pull request. Include tests for platform-specific behavior where possible.  
- Add or update `CHANGELOG.md` entries for breaking changes and new features.

**License**

- This project is released under the **MIT License**. See the `LICENSE` file for details.

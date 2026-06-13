### Declaration
**This extension is a product of Vibe Coding.**  
**Note** Linux and macOS platforms are untested.

# IConsole - GameMaker Native Console Extension

Provides a native console window for GameMaker 2026. The GMS2 runtime does not include a built-in console; **IConsole** fills that gap.

## Features
- **Native console window** — Creates a native console on Windows using `AllocConsole`  
- **Colored output** — 16-color support, automatic coloring by log level  
- **Log levels** — DEBUG, INFO, WARN, ERROR filtering  
- **Timestamps** — Optional timestamp prefix for log lines  
- **File logging** — Simultaneous output to console and file  
- **Console input** — Non-blocking input checks and timed line reads  
- **Cross-platform** — Windows, Linux, macOS support (Linux and macOS untested)

---

## Quick Start

### Integration in GameMaker
1. Create an extension in GameMaker (right click → Create Extension)  
2. Add `extgen/IConsole.dll` for Windows platform  
3. Add `scripts/IConsole/IConsole.gml` as the GML binding script

### GML Usage Example
```gml
// Open console
iconsole_open();
iconsole_set_title("My Game Console");

// Basic output
iconsole_print_line("Game started!");

// Log level
iconsole_set_log_level(1);  // show INFO and above
iconsole_log(0, "This will be filtered"); // DEBUG filtered
iconsole_log(1, "Game started");         // INFO white
iconsole_log(2, "Low memory");           // WARN yellow
iconsole_log(3, "Load failed");          // ERROR red

// Timestamp
iconsole_set_timestamp(true);
iconsole_log(1, "Timestamped log");
// Example output: [09:57:35] [INFO]  Timestamped log

// File logging
iconsole_set_log_file("game.log");

// Console input
if (iconsole_has_input()) {
    var input = iconsole_read_line(100);
    iconsole_print_line("You typed: " + input);
}
```

---

## API Reference

### Basic Functions

| Function | Description |
|---|---|
| **iconsole_open** | Open the console window |
| **iconsole_close** | Close the console window |
| **iconsole_is_open** | Check whether the console is open |
| **iconsole_print** | Print text without newline |
| **iconsole_print_line** | Print a line of text |
| **iconsole_clear** | Clear the console |
| **iconsole_set_title** | Set the console window title |
| **iconsole_set_color** | Set text color (0-15) |

### Color Codes

| Value | Color | Value | Color |
|---|---:|---:|---|
| **0** | Black | **8** | Gray |
| **1** | Blue | **9** | Bright Blue |
| **2** | Green | **10** | Bright Green |
| **3** | Cyan | **11** | Bright Cyan |
| **4** | Red | **12** | Bright Red |
| **5** | Magenta | **13** | Bright Magenta |
| **6** | Yellow | **14** | Bright Yellow |
| **7** | White | **15** | Bright White |

### Log Level Functions

| Function | Description |
|---|---|
| **iconsole_set_log_level** | Set log level (0=DEBUG, 1=INFO, 2=WARN, 3=ERROR) |
| **iconsole_get_log_level** | Get current log level |
| **iconsole_log** | Log with level and automatic coloring |

### Timestamp Functions

| Function | Description |
|---|---|
| **iconsole_set_timestamp** | Enable or disable timestamp prefix |
| **iconsole_timestamp_enabled** | Check whether timestamps are enabled |

### File Logging Functions

| Function | Description |
|---|---|
| **iconsole_set_log_file** | Set log file path |
| **iconsole_get_log_file** | Get current log file path |
| **iconsole_close_log_file** | Close the log file |
| **iconsole_log_file_open** | Check whether log file is open |

### Console Input Functions

| Function | Description |
|---|---|
| **iconsole_has_input** | Check for available input (non-blocking) |
| **iconsole_read_line** | Read a line with timeout in milliseconds |
| **iconsole_read_key** | Read a single key (non-blocking, returns virtual key code) |

---

## Build

### Prerequisites
- **extgen** code generator  
- **CMake 3.25+**  
- **Visual Studio 2026** with v145 toolset

### Build Steps
```bash
cd extgen

# Configure
cmake --preset win-x64-debug

# Build
cmake --build --preset win-x64-debug

# Output: extgen/IConsole.dll
```

---

## Cross Platform Notes

| Platform | Console Behavior |
|---|---|
| **Windows** | Native console window, color, title, Unicode support |
| **Linux** | Outputs to stderr (visible when launched from a terminal) |
| **macOS** | Same as Linux, ANSI color codes supported |

**Note** Linux and macOS platforms are untested. Use with caution and report issues.

---

## Project Structure

```
IConsole/
├── extgen/
│   ├── config.json
│   ├── spec.gmidl
│   ├── CMakeLists.txt
│   ├── CMakePresets.json
│   ├── code_gen/
│   ├── src/native/
│   │   ├── IConsole_native.cpp
│   │   └── IConsole_native.h
│   └── cmake/
├── scripts/IConsole/
│   └── IConsole.gml
└── README.md
```

---

## License

**MIT License**

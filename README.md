# IConsole

IConsole is a native console and logging extension for the GameMaker GMS2
runtime. The current generated extension target is Windows x64.

## Features

- Open, attach to, and close a console window.
- UTF-8 output, titles, clearing, and 16 console colours.
- DEBUG, INFO, WARN, and ERROR filtering with optional timestamps.
- Plain-text file logging with truncate and append modes.
- Non-blocking input checks, bounded line reads, and key reads.
- Optional ANSI colours for redirected output.

On Windows, `iconsole_open` selects its output backend in this order:

1. Preserve redirected stdout supplied by an IDE, pipe, or output file.
2. Reuse a console already attached to the process.
3. Attach to the parent process console.
4. Allocate a new console window only when no reusable output exists.

This means IDE and command-line launches do not create an extra window.
Launching the built game directly without a usable console or redirected
stdout creates an independent console window.

The public API is defined in `api.gmidl`. Native behavior is implemented in
`src/native/IConsole_native.cpp`; files under `code_gen/` are generated and
must not be edited manually.

## Generate And Build

The project uses GM-ExtensionGenerator v1.0.1 (`v1.225bddc`), gm-cli 2.2.0,
and CMake. The generated preset targets Visual Studio 2022 with v143:

```powershell
extgen --config "config.json"
cmake --preset win-x64-release
cmake --build --preset win-x64-release
```

The build copies `IConsole.dll` to
`project/extensions/IConsole/IConsole.dll`. Extgen also patches the function
declarations in `project/extensions/IConsole/IConsole.yy` from `api.gmidl`.

Generated root CMake files, `code_gen/`, extension injectors, `/docs`, and
binaries are ignored by Git. `documentation.json` is the tracked generated API
reference. Keep custom native code under `src/`.

## Validate

Run the complete validation pipeline from the repository root:

```powershell
.\validate.ps1
```

It verifies tool versions, regenerates bindings, performs a fresh native
Release build in `out/validate`, compiles the pinned GameMaker runtime, runs
the Runner self-test, and requires `ICONSOLE_SELF_TEST_PASS`. Use
`-SkipGenerate` only when testing hand-written native or GML changes without
regenerating extgen output. Unlike the generated preset, the validation script
uses the installed Visual Studio generator and its native toolset. Runner is
limited to 120 seconds; on timeout, only that gm-cli process tree is stopped.
The self-test also requires `ICONSOLE_REDIRECTED_OUTPUT_PASS`, proving that
`iconsole_open` kept gm-cli Runner's redirected stdout instead of opening a
separate console window.

The equivalent GameMaker compile command from the repository root is:

```powershell
gm-cli compile "project/IConsole.yyp" --target=windows --runtime=vm --config=Default
```

The test exits automatically and writes `ICONSOLE_SELF_TEST_PASS` to Runner
output when all checks pass. Running the project without the environment
variable opens the interactive demo and echoes completed console input lines.

## API Groups

- Lifecycle: `iconsole_open`, `iconsole_close`, `iconsole_shutdown`,
  `iconsole_is_open`.
- Output: `iconsole_print`, `iconsole_print_line`, `iconsole_clear`,
  `iconsole_flush`, title, colour, and ANSI controls.
- Logging: level filtering, timestamp controls, generic `iconsole_log`, and
  the four level-specific helpers.
- Files: truncate, append, path/status queries, and close.
- Input: `iconsole_has_input`, `iconsole_read_line`, and
  `iconsole_read_key`.

See `api.gmidl` for exact signatures and behavior.

## Platform Notes

Only Windows is enabled in `config.json` and included in the GameMaker
extension resource. POSIX implementation branches exist in the native source
but are not currently generated, packaged, or validated.

`iconsole_read_line(timeout_ms)` waits for a completed line. A timeout of zero
is non-blocking, a positive value is a bounded wait, and `-1` waits
indefinitely. On Windows, `iconsole_has_input` becomes true when Enter for a
completed line is present in the console input queue.

Input is consumed through one internal Windows event dispatcher, so key reads
do not remove characters from pending lines. It retains up to 64 completed
lines, 256 key presses, and 1 MiB of UTF-16 input for the current line. Oldest
queued entries are discarded when a queue is full. An empty completed line and
a timeout both return `""` because the public API has no separate status value.

Call the extension from GameMaker's main thread. Direct string bindings use
extgen's shared return storage and are not safe for concurrent native calls.
Text parameters are UTF-8 but, like other direct C-string bindings, cannot
preserve embedded NUL characters.

On Windows, log paths are decoded as UTF-8 and file contents are written as
UTF-8 bytes. Opening a replacement path is transactional: if the new file
cannot be opened and initialized, the current log remains active. Runtime
write failures close the active file and clear its path.

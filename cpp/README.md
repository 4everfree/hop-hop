# hop-hop — C++ client (Qt6)

The same tray monitor as [`../python/`](../python/README.md), built on Qt6 so one codebase covers macOS and Windows. Reads the same [`../config/clients.toml`](../config/clients.toml) and implements the [detection contract](../README.md#detection-contract).

## Status — read this before building

| Backend | State |
|---|---|
| **Linux** (`platform_linux.cpp`) | **Working and verified** — output matches the Python client process for process |
| **macOS** (`platform_macos.mm`) | **Working** — built and run on macOS 26.5 / arm64, Apple clang 17, Qt 6.11. Detection and `--activate` verified |
| **Windows** (`platform_windows.cpp`) | Written, **never compiled** — no Windows SDK was available |

The Linux backend exists so the shared core (config parsing, classification, ancestor filtering, Qt UI) could be exercised against a real system rather than shipped on faith. The Windows backend follows the API mapping below, but expect to fix small things — a missing header, a struct field spelled differently — on its first build. Nothing in the shared code is platform-specific, so those fixes should stay inside the one `platform_*` file.

The macOS backend's first build needed exactly one such fix, and it is worth knowing about because the compiler's diagnosis was misleading. `NSRunningApplication` has no `-activate` — that method belongs to `NSApplication`. The macOS 14 addition here is `-activateFromApplication:options:`. Clang reports this only as a *warning* ("may not respond to `activate`"), so silencing the accompanying type error compiles cleanly and then dies at runtime with `unrecognized selector sent to instance`. Treat "may not respond to" warnings in the platform layer as errors.

## Build

Needs CMake ≥ 3.21, a C++20 compiler, and Qt6 Widgets (plus Qt6 DBus on Linux, which ships inside qtbase). `toml++` is used if installed; otherwise CMake downloads the pinned single header into the build tree, so **the first configure needs network access**.

Install `qtbase`, not the full `qt` meta-package — the shared core only uses Widgets, and full Qt drags in QtWebEngine and ~37 other components for nothing.

### macOS

```bash
brew install cmake qtbase
```

Homebrew symlinks `qtbase` into its prefix, which CMake already searches — no `CMAKE_PREFIX_PATH` needed. Xcode is not required either; the Command Line Tools SDK is enough for the Objective-C++ backend.

### Linux (Debian / Ubuntu)

```bash
sudo apt install cmake g++ qt6-base-dev
```

### Linux (Fedora)

```bash
sudo dnf install cmake gcc-c++ qt6-qtbase-devel
```

### Build and run (macOS and Linux)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/hop-hop --list
```

### Windows

Install Qt 6 (MSVC build) via the [Qt online installer](https://www.qt.io/download-qt-installer) or vcpkg, then point CMake at it — adjust the path to your version and toolchain:

```bat
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build --config Release
```

### If configure fails to find Qt

```
Could not find a package configuration file provided by "Qt6"
```

means CMake cannot see Qt — usual when Qt came from the official installer rather than a package manager. Point at the directory that *contains* `lib/cmake/Qt6`:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$(brew --prefix qtbase)"
```

## Run

```
hop-hop                 GUI
hop-hop --tray          start minimized to the tray
hop-hop --list          print detected clients, and which config was used
hop-hop --activate PID  raise the window owning that client, then exit
```

`--activate` is also how you test the platform layer in isolation — it exercises the whole ancestor-walk plus activation path without the GUI.

## Layout

```
src/client.h          shared structs (Target, ProcInfo, Client)
src/config.*          clients.toml loading, search order, defaults
src/detect.*          the detection contract: classify, ancestors, CPU deltas
src/platform.h        everything the OS must answer that Qt won't
src/platform_linux.cpp    /proc + KWin scripting over D-Bus
src/platform_macos.mm     libproc/sysctl + NSRunningApplication
src/platform_windows.cpp  Toolhelp + WMI + EnumWindows
src/single_instance.*  flock (POSIX) / named mutex (Windows)
src/main_window.*     tray icon and table
src/main.cpp          argument handling
```

## Platform API mapping

| | macOS | Windows |
|---|---|---|
| enumerate PIDs | `proc_listpids` | `CreateToolhelp32Snapshot` |
| process name | `proc_pidpath` | `PROCESSENTRY32W.szExeFile` |
| full command line | `sysctl(KERN_PROCARGS2)` | one WMI query for all processes |
| parent pid | `sysctl(KERN_PROC_PID)` → `e_ppid` | `th32ParentProcessID` |
| working directory | `proc_pidinfo(PROC_PIDVNODEPATHINFO)` | not implemented — see gaps |
| CPU / RSS | `proc_pidinfo(PROC_PIDTASKINFO)` | `GetProcessTimes`, `GetProcessMemoryInfo` |
| start time | `kp_proc.p_starttime` | `GetProcessTimes` creation time |
| TTY | `devname(e_tdev)` | none — see gaps |
| activate window | `NSRunningApplication.activate` | `EnumWindows` → `SetForegroundWindow` |
| terminate / kill | `SIGTERM` / `SIGKILL` | `WM_CLOSE` / `TerminateProcess` |
| single instance | `flock` | named mutex + pid file |

Everything is scoped to processes owned by the current user.

## Known gaps

- **TTY is a Unix concept.** The column stays empty on Windows; the owning parent process (terminal / IDE / browser) is what identifies a console there.
- **Working directory on Windows** would require reading another process's PEB. It is left empty rather than faked — the cwd is the main way you tell two clients apart, so on Windows that weight shifts to the command line.
- **`SetForegroundWindow` is restricted.** Windows only grants focus to the foreground process or one with recent input; when it refuses, the code falls back to flashing the taskbar button. The Linux backend has the mirror-image problem on Wayland, which is why activation goes through KWin scripting instead of being done directly.
- **WMI costs a round trip** per refresh. It is one query for all processes, not one per process, but if a 2-second refresh proves too heavy on Windows, the fallback is reading each PEB directly.
- **The lock is shared with the Python client** — both use `hop-hop.lock`, so starting one while the other runs raises the running window instead of opening a second monitor. That is intended: one monitor at a time, whichever implementation it is.

## Contract rules that are easy to get wrong

Both were caught by comparing against the Python client, and both silently misclassify rather than crash:

- **Preserve config file order.** `toml++` returns tables sorted by key, but precedence when a process matches several kinds is defined by the order in the file. `config.cpp` re-sorts by source line to restore it.
- **Normalize before matching.** Backslashes to `/`, and strip `.exe`/`.cmd`/`.bat` from names — otherwise every path-shaped marker and every Windows executable misses.

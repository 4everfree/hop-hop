# hop-hop — C++ client (macOS / Windows)

**Status: planned.** No code yet — this file is the spec, so the implementation doesn't have to re-derive the design decisions already made in the Python client.

Goal: the same tray monitor as [`../python/`](../python/README.md), native on macOS and Windows, reading the same [`../config/clients.toml`](../config/clients.toml) and following the detection contract in the [root README](../README.md#detection-contract).

Binary name: `hop-hop`.

## Stack

- **Qt6** (`QSystemTrayIcon`, `QTableWidget`, `QMenu`) — the Python client is PySide6, so the UI layer ports over almost 1:1, and one codebase covers both targets. The alternative, going fully native (`NSStatusItem` on macOS, `Shell_NotifyIcon` on Windows), buys a smaller binary at the cost of writing the UI twice.
- **toml++** (header-only) for parsing the shared config.
- **CMake** + presets; vendored deps under `cpp/lib/` if not using a package manager.

## What Qt does not give you

Process inspection and window activation are per-platform and have to be written by hand. Both are small, isolated layers — put them behind one interface (`ProcessSource`, `WindowActivator`) with two implementations each.

| | macOS | Windows |
|---|---|---|
| enumerate PIDs | `proc_listpids(PROC_ALL_PIDS, …)` | `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS)` |
| process name / path | `proc_pidpath` | `PROCESSENTRY32.szExeFile` |
| full command line | `sysctl(KERN_PROCARGS2)` | WMI `Win32_Process.CommandLine`, or read the PEB via `NtQueryInformationProcess` + `ReadProcessMemory` |
| parent pid | `sysctl(KERN_PROC_PID)` → `kp_eproc.e_ppid` | `PROCESSENTRY32.th32ParentProcessID` |
| working directory | `proc_pidinfo(PROC_PIDVNODEPATHINFO)` | PEB only — best effort |
| CPU / RSS | `proc_pidinfo(PROC_PIDTASKINFO)` | `GetProcessTimes`, `GetProcessMemoryInfo` |
| start time | `kp_proc.p_starttime` | `GetProcessTimes` (creation time) |
| activate window | `NSRunningApplication(processIdentifier:)` → `activate(options:)` | `EnumWindows` + `GetWindowThreadProcessId` to find the top-level window, then `ShowWindow(SW_RESTORE)` + `SetForegroundWindow` |
| single instance | `flock` on a lock file (same as Python) | named mutex: `CreateMutexW` + `GetLastError() == ERROR_ALREADY_EXISTS` |

All of this only covers processes owned by the same user — which is what we want anyway.

## Known gaps

Worth deciding up front rather than discovering mid-implementation:

- **TTY is a Unix concept.** The column has no meaning on Windows; show the owning parent process (terminal / IDE / browser) instead. Keep TTY on macOS.
- **Working directory on Windows** requires reading another process's PEB and can fail on protected or cross-architecture processes. Treat it as best-effort and render an empty cell rather than failing the row — the cwd is the main way a user tells two clients apart, so where it's missing the parent process name should carry that weight.
- **`SetForegroundWindow` is restricted.** Windows only lets the foreground process (or one with recent input) steal focus; when it refuses, the usual fallback is flashing the taskbar button (`FlashWindowEx`). The Python client has the mirror-image problem on Wayland, solved through KWin scripting.
- **Config search order** should mirror the Python client, with platform-appropriate first entries: `%APPDATA%\hop-hop\clients.toml` on Windows, `~/Library/Application Support/hop-hop/clients.toml` on macOS, then a local `clients.toml`, then `../config/clients.toml`.
- **Marker normalization is mandatory** — convert `\` to `/` before matching, or every path-shaped marker in the shared config misses on Windows.
- **Strip executable extensions** before comparing `names` — `.exe`, `.cmd`, `.bat` — otherwise `codex.exe` never matches the config entry `codex`. The Python client does this in `_basename()`; both rules are stated in the config header.

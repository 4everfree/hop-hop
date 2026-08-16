# hop-hop

A tray monitor for **running AI coding clients** — Claude Code, Antigravity (agy), Codex, Gemini and anything else you list in the config. It answers the question you get once you have a dozen terminals open: *how many clients am I actually running, and which console is which?*

The tray icon carries the live count; the window lists every client with its working directory, terminal, uptime and resource use, and lets you jump straight to the window it lives in.

## Layout

```
config/clients.toml   shared detection config — the single source of truth
python/               working implementation (Linux; window switching needs KDE/KWin)
cpp/                  planned implementation (macOS + Windows)
```

| Implementation | Platforms | Status |
|---|---|---|
| [`python/`](python/README.md) — PySide6 + psutil | Linux (KDE Plasma for window switching) | Works |
| [`cpp/`](cpp/README.md) — Qt6 + toml++ | macOS, Windows | Planned — spec written, no code yet |

Both implementations read the same [`config/clients.toml`](config/clients.toml) and are meant to behave identically. Adding a new client is a config edit, not a code change.

## Quick start (Python)

```bash
sudo pacman -S python-psutil pyside6
python python/hop-hop.py --list   # check detection first
python python/hop-hop.py          # GUI
```

Details, autostart and configuration: [`python/README.md`](python/README.md).

## Detection contract

This is the behaviour both implementations must share; it's the reason the config can be shared at all.

**1. Matching.** A process is a client if either hits:
- `names` — its process name, or the basename of one of the first argv entries, matches exactly (case-insensitive);
- `markers` — a substring occurs anywhere in the full command line (case-insensitive).

**2. Platform-neutral names.** Backslashes in the command line are converted to `/` before matching, and executable extensions (`.exe`, `.cmd`, `.bat`) are stripped from names. That's what lets one config serve all three platforms: markers are written unix-style, names without an extension, and `codex` still matches `codex.exe`.

**3. Descendant filtering.** If a matched process has a matched ancestor, it's dropped. This is what keeps Electron renderers (Antigravity) and helper shells (Claude Code) from inflating the count — one row per client, not per process tree.

**4. Window ownership.** A client rarely owns its own window; the owner is an ancestor — a terminal emulator, an IDE, a browser. To switch to a client you walk `client pid → parents`, skipping `systemd`/init (half the desktop hangs off `systemd --user`, so matching it produces false hits), and activate the window whose pid appears in that chain.

**5. Single instance.** Only one monitor runs at a time; a repeat launch raises the existing window instead of spawning a copy.

Platform-specific notes — which APIs provide the process data, and where a platform simply can't (TTY on Windows, cwd on Windows) — are in [`cpp/README.md`](cpp/README.md).

# hop-hop — Python client (Linux / KDE)

A GUI monitor for running AI coding clients — **Claude Code**, **Antigravity (agy)**, **Codex**, **Gemini**, and whatever else you add to the config: a tray indicator showing the number of active clients and a window listing them. Handy when the number of client consoles grows and you can no longer tell which is which.

A single file built on **PySide6 + psutil**, with no external dependencies beyond those.

> Platform: Linux. Window switching additionally requires KDE Plasma (KWin) — see below. For macOS/Windows see [`../cpp/`](../cpp/README.md).

## Features

- **Tray icon** with the number of active clients. Grey at zero, red when the count exceeds `WARN_LEVEL` (6 by default).
- **List window**: type, PID, working directory, TTY, uptime, CPU, RAM, full command. Sortable by any column, plus a "with a terminal only" filter.
- **Double-click a row — switch to the client's window** (terminal, PyCharm, Chrome — whoever owns the window). Also available in the context menu.
- **Context menu** (right-click): switch to window, open directory, copy PID/command, send `SIGTERM`/`SIGKILL`.
- **Descendant filtering**: child processes of matched clients (Electron renderers under Antigravity, shells/ripgrep under Claude Code) don't inflate the count.
- **Single instance in memory**: a repeat launch doesn't spawn a copy — it raises the already-running window to the foreground.
- **Configurable clients**: the tracked set lives in the shared [`../config/clients.toml`](../config/clients.toml).

The directory and TTY are what you use to tell which console is which.

## Installation

Dependencies (Arch/CachyOS):

```bash
sudo pacman -S python-psutil pyside6
```

## Running

From the repository root:

```bash
python python/hop-hop.py --list   # first, check detection (print to terminal)
python python/hop-hop.py          # GUI
python python/hop-hop.py --tray   # start minimized to the tray
```

Start with `--list` to make sure your clients are matched.

### Autostart

Drop a `.desktop` file into `~/.config/autostart/` — the monitor will start minimized to the tray when you log in. Already set up as `~/.config/autostart/hop-hop.desktop`; to reproduce it on another machine, create a file with the following contents (fix the script path):

```ini
[Desktop Entry]
Type=Application
Name=hop-hop
Comment=Monitor for running AI coding clients
Exec=/usr/bin/python /home/htp/Projects/hop-hop/python/hop-hop.py --tray
Icon=utilities-system-monitor
Terminal=false
X-GNOME-Autostart-enabled=true
```

Validate the file: `desktop-file-validate ~/.config/autostart/hop-hop.desktop`.

**Managing autostart:**

- Disable temporarily without deleting the file: `X-GNOME-Autostart-enabled=false` (or in KDE: *System Settings → Autostart*).
- Remove entirely: `rm ~/.config/autostart/hop-hop.desktop`.
- Launch right now, as autostart would: `gtk-launch hop-hop`, or just `python python/hop-hop.py --tray`.

## Client config

Which clients to detect lives in [`../config/clients.toml`](../config/clients.toml), shared with the C++ client. Search order — the first file that exists wins and fully replaces the set:

1. `$XDG_CONFIG_HOME/hop-hop/clients.toml` (i.e. `~/.config/hop-hop/clients.toml`) — per-machine override
2. `clients.toml` next to the script (`python/clients.toml`) — local override
3. `../config/clients.toml` — the shared file in the repo

If none exists, built-in defaults (`claude`, `agy`) are used. A broken TOML doesn't take the monitor down — it warns on stderr and falls back.

Format and the forward-slash rule for markers are documented in the config file itself. After editing, verify:

```bash
python python/hop-hop.py --list
```

To see how a process shows up in the process list (so you know what to put in `names`/`markers`):

```bash
ps -eo pid,comm,args | grep -i -e codex -e gemini
```

## Switching to a client's window

The window is owned not by the `claude`/`agy` process itself but by its ancestor — a terminal emulator (kitty/konsole/…), PyCharm, or Chrome. So on click the monitor builds the chain `client pid → parents` (excluding `systemd --user` and init) and activates the window whose pid matches someone in the chain. Clients inside PyCharm jump to the PyCharm window, terminal ones to the terminal, `claude --chrome-native-host` to Chrome.

**Requires KDE Plasma (KWin).** Activating another window on Wayland is forbidden to ordinary applications, so KWin scripting over D-Bus (`gdbus` + `org.kde.KWin`) is used. On other window managers the activation silently does nothing — the rest of the functionality is unaffected. The only extra dependency is `gdbus` (usually already present on the system).

## Single instance

To avoid getting lost among several windows, only one instance is kept in memory. This is implemented via `flock` on the file `$XDG_RUNTIME_DIR/hop-hop.lock` (the lock is tied to the process and released by the kernel even on `kill -9`, so no stale locks are left behind). A repeat launch — by hand, from autostart, or by double-clicking the `.desktop` — doesn't create a second copy but raises the already-running window to the foreground (using the same KWin mechanism as switching to a client's window) and exits. `--list` isn't subject to the lock — it's a one-off print and can be called in parallel.

## Other settings

At the top of `hop-hop.py`:

- **`REFRESH_MS`** — polling interval (2000 ms by default).
- **`WARN_LEVEL`** — the threshold above which the tray icon turns red (6 by default).

## Detection check

| Process | Command line | Result |
|---|---|---|
| claude | `claude` | claude |
| node | `node /home/u/.local/share/claude/cli.js --resume` | claude |
| node | `/usr/bin/node .../@anthropic-ai/claude-code/cli.js` | claude |
| agy | `agy chat` | agy |
| antigravity | `/opt/antigravity/antigravity --type=renderer` | agy |
| bash | `bash` | — |
| python | `python /home/u/hop-hop.py` | — |
| rg | `rg --files` | — |

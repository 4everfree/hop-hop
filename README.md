# claude-agy-monitor

A GUI monitor for running **Claude Code** and **Antigravity (agy)** clients: a tray indicator showing the number of active clients and a window listing them. Handy when the number of client consoles grows and you can no longer tell which is which.

A single file built on **PySide6 + psutil**, with no external dependencies beyond those.

## Features

- **Tray icon** with the number of active clients. Grey at zero, red when the count exceeds `WARN_LEVEL` (6 by default).
- **List window**: type, PID, working directory, TTY, uptime, CPU, RAM, full command. Sortable by any column, plus a "with a terminal only" filter.
- **Double-click a row — switch to the client's window** (terminal, PyCharm, Chrome — whoever owns the window). Also available in the context menu.
- **Context menu** (right-click): switch to window, open directory, copy PID/command, send `SIGTERM`/`SIGKILL`.
- **Descendant filtering**: child processes of matched clients (Electron renderers under Antigravity, shells/ripgrep under Claude Code) don't inflate the count.
- **Single instance in memory**: a repeat launch doesn't spawn a copy — it raises the already-running window to the foreground.

The directory and TTY are what you use to tell which console is which.

## Installation

Dependencies (Arch/CachyOS):

```bash
sudo pacman -S python-psutil pyside6
```

## Running

```bash
python claude-agy-monitor.py --list   # first, check detection (print to terminal)
python claude-agy-monitor.py          # GUI
python claude-agy-monitor.py --tray   # start minimized to the tray
```

Start with `--list` to make sure your clients are matched.

### Autostart

Drop a `.desktop` file into `~/.config/autostart/` — the monitor will start minimized to the tray when you log in. This is already set up in the repo (`~/.config/autostart/claude-agy-monitor.desktop`); to reproduce it on another machine, create a file with the following contents (fix the script path):

```ini
[Desktop Entry]
Type=Application
Name=claude-agy-monitor
Comment=Monitor for running Claude Code and Antigravity clients
Exec=/usr/bin/python /home/htp/Projects/ai_clients_monitor/claude-agy-monitor.py --tray
Icon=utilities-system-monitor
Terminal=false
X-GNOME-Autostart-enabled=true
```

Validate the file: `desktop-file-validate ~/.config/autostart/claude-agy-monitor.desktop`.

**Managing autostart:**

- Disable temporarily without deleting the file: `X-GNOME-Autostart-enabled=false` (or in KDE: *System Settings → Autostart*).
- Remove entirely: `rm ~/.config/autostart/claude-agy-monitor.desktop`.
- Launch right now, as autostart would: `gtk-launch claude-agy-monitor`, or just `python claude-agy-monitor.py --tray`.

## Switching to a client's window

The window is owned not by the `claude`/`agy` process itself but by its ancestor — a terminal emulator (kitty/konsole/…), PyCharm, or Chrome. So on click the monitor builds the chain `client pid → parents` (excluding `systemd --user` and init) and activates the window whose pid matches someone in the chain. Clients inside PyCharm jump to the PyCharm window, terminal ones to the terminal, `claude --chrome-native-host` to Chrome.

**Requires KDE Plasma (KWin).** Activating another window on Wayland is forbidden to ordinary applications, so KWin scripting over D-Bus (`gdbus` + `org.kde.KWin`) is used. On other window managers the activation silently does nothing — the rest of the functionality is unaffected. The only extra dependency is `gdbus` (usually already present on the system).

## Single instance

To avoid getting lost among several windows, only one instance is kept in memory. This is implemented via `flock` on the file `$XDG_RUNTIME_DIR/claude-agy-monitor.lock` (the lock is tied to the process and released by the kernel even on `kill -9`, so no stale locks are left behind). A repeat launch — by hand, from autostart, or by double-clicking the `.desktop` — doesn't create a second copy but raises the already-running window to the foreground (using the same KWin mechanism as switching to a client's window) and exits. `--list` isn't subject to the lock — it's a one-off print and can be called in parallel.

## Configuration

At the top of the file:

- **`TARGETS`** — detection rules. If a client isn't matched, add the process name to `names` or a command-line substring to `markers`. Type colors live here too.
- **`REFRESH_MS`** — polling interval (2000 ms by default).
- **`WARN_LEVEL`** — the threshold above which the tray icon turns red (6 by default).

Check how a process shows up on the system:

```bash
ps -eo pid,comm,args | grep -i -e agy -e antigravity
```

## Detection check

| Process | Command line | Result |
|---|---|---|
| claude | `claude` | claude |
| node | `node /home/u/.local/share/claude/cli.js --resume` | claude |
| node | `/usr/bin/node .../@anthropic-ai/claude-code/cli.js` | claude |
| agy | `agy chat` | agy |
| antigravity | `/opt/antigravity/antigravity --type=renderer` | agy |
| bash | `bash` | — |
| python | `python /home/u/claude-agy-monitor.py` | — |
| rg | `rg --files` | — |

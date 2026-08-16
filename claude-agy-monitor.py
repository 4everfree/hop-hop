#!/usr/bin/env python3
"""
claude-agy-monitor — tray indicator + window listing the running
Claude Code and Antigravity (agy) clients.

  python claude-agy-monitor.py           # GUI
  python claude-agy-monitor.py --list    # print to terminal (check detection)

Dependencies (Arch/CachyOS):
  sudo pacman -S python-psutil pyside6
"""

from __future__ import annotations

import atexit
import fcntl
import os
import re
import signal
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass

try:
    import psutil
except ImportError:
    sys.exit("psutil is missing:  sudo pacman -S python-psutil")

# --------------------------------------------------------------------------
# Detection config. If something is not matched — tweak names/markers.
# names   — compared against the process name and the basename of the first argv items
# markers — substring in the full command line (lowercased)
# --------------------------------------------------------------------------

TARGETS = {
    "claude": {
        "color": "#D97757",
        "names": {"claude", "claude-code"},
        "markers": ("@anthropic-ai/claude-code", "/claude/cli.js", ".claude/local/"),
    },
    "agy": {
        "color": "#5A9BF6",
        "names": {"agy", "antigravity"},
        "markers": ("antigravity",),
    },
}

REFRESH_MS = 2000   # polling interval, ms
WARN_LEVEL = 6      # more clients than this — the tray icon turns red

SELF_MARK = os.path.basename(os.path.abspath(__file__)).lower()


# --------------------------------------------------------------------------
# Process collection
# --------------------------------------------------------------------------

@dataclass
class Client:
    pid: int
    kind: str
    cwd: str
    tty: str
    uptime: float
    cpu: float
    rss: int
    cmd: str


_cache: dict[int, psutil.Process] = {}


def _classify(name: str, cmdline: list[str]) -> str | None:
    joined = " ".join(cmdline).lower()
    if SELF_MARK in joined:          # don't count the monitor itself
        return None
    bases = {os.path.basename(a).lower() for a in cmdline[:3]}
    bases.add(name)
    for kind, cfg in TARGETS.items():
        if bases & cfg["names"]:
            return kind
        if any(m in joined for m in cfg["markers"]):
            return kind
    return None


def collect(only_tty: bool = False) -> list[Client]:
    me = os.getpid()
    matched: dict[int, str] = {}
    procs: dict[int, psutil.Process] = {}

    for p in psutil.process_iter(["name", "cmdline"]):
        if p.pid == me:
            continue
        cmdline = p.info.get("cmdline") or []
        if not cmdline:
            continue
        kind = _classify((p.info.get("name") or "").lower(), cmdline)
        if kind:
            matched[p.pid] = kind
            procs[p.pid] = p

    # drop descendants of already-matched processes (Electron helpers, sandbox, shells)
    def has_matched_ancestor(proc: psutil.Process) -> bool:
        try:
            for parent in proc.parents():
                if parent.pid in matched:
                    return True
        except psutil.Error:
            pass
        return False

    out: list[Client] = []
    now = time.time()
    for pid, kind in matched.items():
        proc = procs[pid]
        if has_matched_ancestor(proc):
            continue
        cached = _cache.get(pid)
        if cached is None or not cached.is_running():
            cached = proc
            _cache[pid] = cached
            cached.cpu_percent(None)  # first call sets the baseline
        try:
            with cached.oneshot():
                tty = cached.terminal() or ""
                if only_tty and not tty:
                    continue
                out.append(Client(
                    pid=pid,
                    kind=kind,
                    cwd=cached.cwd() or "",
                    tty=tty.replace("/dev/", ""),
                    uptime=now - cached.create_time(),
                    cpu=cached.cpu_percent(None),
                    rss=cached.memory_info().rss,
                    cmd=" ".join(cached.cmdline()),
                ))
        except psutil.Error:
            continue

    for dead in set(_cache) - set(matched):
        _cache.pop(dead, None)

    out.sort(key=lambda c: (c.kind, -c.uptime))
    return out


def fmt_uptime(sec: float) -> str:
    s = int(sec)
    d, s = divmod(s, 86400)
    h, s = divmod(s, 3600)
    m, s = divmod(s, 60)
    if d:
        return f"{d}d {h}h"
    if h:
        return f"{h}:{m:02d}:{s:02d}"
    return f"{m}:{s:02d}"


# --------------------------------------------------------------------------
# Switch to a client's window (KDE Wayland — KWin scripting over D-Bus).
# The window is owned not by the client itself but by its ancestor (terminal,
# PyCharm, Chrome…), so we activate the window whose pid matches the client's
# pid or one of its ancestors. systemd/init are dropped from the chain —
# otherwise half of the GUI windows sit under `systemd --user`'s pid and the
# match would be a false positive.
# --------------------------------------------------------------------------

_KWIN_PLUGIN = "claudeagymon_activate"


def ancestor_pids(pid: int) -> list[int]:
    out = [pid]
    try:
        for a in psutil.Process(pid).parents():
            if a.pid == 1:
                break
            try:
                if (a.name() or "").lower() == "systemd":
                    continue
            except psutil.Error:
                pass
            out.append(a.pid)
    except psutil.Error:
        pass
    return out


def activate_window(pids: list[int]) -> bool:
    """Activate the window whose pid is in pids. True — request sent."""
    targets = "[" + ",".join(str(p) for p in pids) + "]"
    js = (
        f"var t={targets};"
        "var ws=(typeof workspace.windowList==='function')"
        "?workspace.windowList():workspace.clientList();"
        "for(var i=0;i<ws.length;i++){if(t.indexOf(ws[i].pid)!==-1){"
        "if(ws[i].minimized)ws[i].minimized=false;"
        "workspace.activeWindow=ws[i];break;}}"
    )
    try:
        with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
            f.write(js)
            path = f.name
    except OSError:
        return False

    def g(obj: str, method: str, *args: str) -> subprocess.CompletedProcess:
        return subprocess.run(
            ["gdbus", "call", "--session", "--dest", "org.kde.KWin",
             "--object-path", obj, "--method", method, *args],
            capture_output=True, text=True, timeout=4)

    try:
        g("/Scripting", "org.kde.kwin.Scripting.unloadScript", _KWIN_PLUGIN)
        r = g("/Scripting", "org.kde.kwin.Scripting.loadScript", path, _KWIN_PLUGIN)
        m = re.search(r"\d+", r.stdout or "")
        if not m:
            return False
        g(f"/Scripting/Script{m.group()}", "org.kde.kwin.Script.run")
        g("/Scripting", "org.kde.kwin.Scripting.unloadScript", _KWIN_PLUGIN)
        return True
    except (OSError, subprocess.SubprocessError):
        return False
    finally:
        try:
            os.unlink(path)
        except OSError:
            pass


# --------------------------------------------------------------------------
# Single instance in memory. We hold an flock on a file in $XDG_RUNTIME_DIR;
# the lock is tied to the process and released by the kernel even on kill -9.
# A second launch doesn't spawn a copy — it raises the running window instead.
# --------------------------------------------------------------------------

_LOCK_PATH = os.path.join(
    os.environ.get("XDG_RUNTIME_DIR") or tempfile.gettempdir(),
    "claude-agy-monitor.lock")
_lock_fd: int | None = None


def _release_lock() -> None:
    global _lock_fd
    if _lock_fd is not None:
        try:
            fcntl.flock(_lock_fd, fcntl.LOCK_UN)
            os.close(_lock_fd)
            os.unlink(_LOCK_PATH)
        except OSError:
            pass
        _lock_fd = None


def single_instance_or_focus() -> bool:
    """True — we are the only instance. False — already running (window raised)."""
    global _lock_fd
    try:
        fd = os.open(_LOCK_PATH, os.O_RDWR | os.O_CREAT, 0o644)
    except OSError:
        return True  # couldn't create the lock — don't block startup
    try:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        try:
            other = int(os.read(fd, 32).decode().strip() or 0)
        except (OSError, ValueError):
            other = 0
        os.close(fd)
        if other:
            activate_window(ancestor_pids(other))
        return False
    os.ftruncate(fd, 0)
    os.write(fd, str(os.getpid()).encode())
    os.fsync(fd)
    _lock_fd = fd  # keep open for the process lifetime
    atexit.register(_release_lock)
    return True


def print_list() -> None:
    clients = collect()
    if not clients:
        print("Nothing found. Check TARGETS at the top of the file.")
        return
    counts: dict[str, int] = {}
    for c in clients:
        counts[c.kind] = counts.get(c.kind, 0) + 1
    print("  ".join(f"{k}: {v}" for k, v in counts.items()), f"(total {len(clients)})\n")
    for c in clients:
        print(f"{c.kind:<7} pid={c.pid:<8} tty={c.tty or '-':<8} "
              f"{fmt_uptime(c.uptime):>9}  {c.rss // 1048576:>5} MB  {c.cwd}")


# --------------------------------------------------------------------------
# GUI
# --------------------------------------------------------------------------

def run_gui() -> None:
    try:
        from PySide6.QtCore import Qt, QTimer
        from PySide6.QtGui import QAction, QColor, QFont, QIcon, QPainter, QPixmap
        from PySide6.QtWidgets import (
            QAbstractItemView, QApplication, QCheckBox, QHBoxLayout, QHeaderView,
            QLabel, QMainWindow, QMenu, QMessageBox, QPushButton, QSystemTrayIcon,
            QTableWidget, QTableWidgetItem, QVBoxLayout, QWidget,
        )
    except ImportError:
        sys.exit("PySide6 is missing:  sudo pacman -S pyside6")

    COLS = ["Type", "PID", "Directory", "TTY", "Uptime", "CPU %", "RAM", "Command"]

    def make_icon(total: int) -> QIcon:
        size = 64
        pm = QPixmap(size, size)
        pm.fill(Qt.transparent)
        p = QPainter(pm)
        p.setRenderHint(QPainter.Antialiasing)
        if total == 0:
            bg = QColor("#6E6E6E")
        elif total > WARN_LEVEL:
            bg = QColor("#C0392B")
        else:
            bg = QColor(TARGETS["claude"]["color"])
        p.setBrush(bg)
        p.setPen(Qt.NoPen)
        p.drawRoundedRect(2, 2, size - 4, size - 4, 14, 14)
        p.setPen(QColor("#FFFFFF"))
        f = QFont()
        f.setBold(True)
        f.setPixelSize(44 if total < 10 else 34)
        p.setFont(f)
        p.drawText(pm.rect(), Qt.AlignCenter, str(total))
        p.end()
        return QIcon(pm)

    class Window(QMainWindow):
        def __init__(self) -> None:
            super().__init__()
            self.setWindowTitle("Claude / Antigravity — running clients")
            self.resize(1000, 420)

            self.summary = QLabel()
            self.summary.setTextFormat(Qt.RichText)
            self.only_tty = QCheckBox("With a terminal only")
            refresh = QPushButton("Refresh")
            refresh.clicked.connect(self.refresh)

            top = QHBoxLayout()
            top.addWidget(self.summary)
            top.addStretch(1)
            top.addWidget(self.only_tty)
            top.addWidget(refresh)

            self.table = QTableWidget(0, len(COLS))
            self.table.setHorizontalHeaderLabels(COLS)
            self.table.setSelectionBehavior(QAbstractItemView.SelectRows)
            self.table.setEditTriggers(QAbstractItemView.NoEditTriggers)
            self.table.setSortingEnabled(True)
            self.table.setContextMenuPolicy(Qt.CustomContextMenu)
            self.table.customContextMenuRequested.connect(self.menu)
            self.table.doubleClicked.connect(lambda _idx: self.activate())
            self.table.setToolTip("Double-click — switch to the client's window")
            hh = self.table.horizontalHeader()
            hh.setSectionResizeMode(2, QHeaderView.Stretch)
            hh.setSectionResizeMode(7, QHeaderView.Interactive)

            body = QVBoxLayout()
            body.addLayout(top)
            body.addWidget(self.table)
            root = QWidget()
            root.setLayout(body)
            self.setCentralWidget(root)

            self.clients: list[Client] = []

            self.tray = QSystemTrayIcon(make_icon(0), self)
            menu = QMenu()
            act_show = QAction("Show window", self)
            act_show.triggered.connect(self.show_window)
            act_quit = QAction("Quit", self)
            act_quit.triggered.connect(QApplication.quit)
            menu.addAction(act_show)
            menu.addSeparator()
            menu.addAction(act_quit)
            self.tray.setContextMenu(menu)
            self.tray.activated.connect(
                lambda r: self.show_window() if r == QSystemTrayIcon.Trigger else None)
            self.tray.show()

            self.timer = QTimer(self)
            self.timer.timeout.connect(self.refresh)
            self.timer.start(REFRESH_MS)
            self.refresh()

        # ---- data ----
        def refresh(self) -> None:
            self.clients = collect(self.only_tty.isChecked())
            counts = {k: 0 for k in TARGETS}
            for c in self.clients:
                counts[c.kind] = counts.get(c.kind, 0) + 1
            total = len(self.clients)

            parts = [f"<span style='color:{TARGETS[k]['color']}'><b>{k}</b>: {v}</span>"
                     for k, v in counts.items()]
            self.summary.setText("&nbsp;&nbsp;&nbsp;".join(parts) + f"&nbsp;&nbsp;&nbsp;total: <b>{total}</b>")
            self.tray.setIcon(make_icon(total))
            self.tray.setToolTip("  ".join(f"{k}: {v}" for k, v in counts.items()))

            col, order = (self.table.horizontalHeader().sortIndicatorSection(),
                          self.table.horizontalHeader().sortIndicatorOrder())
            selected = {self.table.item(i.row(), 1).text()
                        for i in self.table.selectedIndexes() if self.table.item(i.row(), 1)}

            self.table.setSortingEnabled(False)
            self.table.setRowCount(len(self.clients))
            for row, c in enumerate(self.clients):
                values = [c.kind, c.pid, c.cwd, c.tty or "—", fmt_uptime(c.uptime),
                          round(c.cpu, 1), c.rss // 1048576, c.cmd]
                for i, v in enumerate(values):
                    item = QTableWidgetItem()
                    if i in (1, 5, 6):
                        item.setData(Qt.DisplayRole, v)
                        item.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)
                    elif i == 4:
                        item.setData(Qt.DisplayRole, v)
                        item.setData(Qt.UserRole, c.uptime)
                    else:
                        item.setText(str(v))
                    if i == 0:
                        item.setForeground(QColor(TARGETS[c.kind]["color"]))
                        font = item.font()
                        font.setBold(True)
                        item.setFont(font)
                    if i == 6:
                        item.setText(f"{c.rss // 1048576} MB")
                        item.setData(Qt.UserRole, c.rss)
                    self.table.setItem(row, i, item)
                if str(c.pid) in selected:
                    self.table.selectRow(row)
            self.table.setSortingEnabled(True)
            self.table.sortItems(col, order)
            self.table.resizeColumnsToContents()
            self.table.horizontalHeader().setSectionResizeMode(2, QHeaderView.Stretch)

        # ---- actions ----
        def current(self) -> Client | None:
            row = self.table.currentRow()
            if row < 0:
                return None
            pid = int(self.table.item(row, 1).text())
            return next((c for c in self.clients if c.pid == pid), None)

        def activate(self) -> None:
            c = self.current()
            if c:
                activate_window(ancestor_pids(c.pid))

        def menu(self, pos) -> None:
            c = self.current()
            if not c:
                return
            m = QMenu(self)
            a_win = m.addAction("Switch to window")
            m.addSeparator()
            a_dir = m.addAction("Open directory")
            a_copy = m.addAction("Copy PID")
            a_cmd = m.addAction("Copy command")
            m.addSeparator()
            a_term = m.addAction("Terminate (SIGTERM)")
            a_kill = m.addAction("Kill (SIGKILL)")
            chosen = m.exec(self.table.viewport().mapToGlobal(pos))
            if chosen == a_win:
                self.activate()
            elif chosen == a_dir and c.cwd:
                os.system(f"xdg-open {c.cwd!r} >/dev/null 2>&1 &")
            elif chosen == a_copy:
                QApplication.clipboard().setText(str(c.pid))
            elif chosen == a_cmd:
                QApplication.clipboard().setText(c.cmd)
            elif chosen in (a_term, a_kill):
                sig = signal.SIGTERM if chosen == a_term else signal.SIGKILL
                if QMessageBox.question(
                        self, "Confirm",
                        f"Send {sig.name} to process {c.pid} ({c.kind})?\n{c.cwd}"
                ) == QMessageBox.Yes:
                    try:
                        os.kill(c.pid, sig)
                    except OSError as e:
                        QMessageBox.warning(self, "Failed", str(e))
                    self.refresh()

        def show_window(self) -> None:
            self.showNormal()
            self.raise_()
            self.activateWindow()

        def closeEvent(self, event) -> None:   # window hides into the tray
            if self.tray.isVisible():
                event.ignore()
                self.hide()
            else:
                event.accept()

    app = QApplication(sys.argv)
    app.setQuitOnLastWindowClosed(False)
    win = Window()
    if "--tray" in sys.argv:
        win.hide()
    else:
        win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    if "--list" in sys.argv:
        print_list()
    elif not single_instance_or_focus():
        sys.exit("claude-agy-monitor is already running — raised its window.")
    else:
        run_gui()

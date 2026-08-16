// Linux backend: /proc for process data, KWin scripting over D-Bus for
// window activation (a Wayland client may not raise another app's window,
// so the compositor has to do it for us).
#include "platform.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextStream>

#include <csignal>
#include <unistd.h>

namespace platform {
namespace {

double clockTicks()
{
    static const double ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
    return ticks > 0 ? ticks : 100.0;
}

qint64 pageSize()
{
    static const qint64 size = sysconf(_SC_PAGESIZE);
    return size > 0 ? size : 4096;
}

// Seconds since the epoch at which the kernel started counting.
double bootTime()
{
    static double cached = -1;
    if (cached >= 0)
        return cached;
    cached = 0;
    // procfs reports size 0, so read it whole rather than streaming lines
    QFile f(QStringLiteral("/proc/stat"));
    if (f.open(QIODevice::ReadOnly)) {
        for (const QByteArray &line : f.readAll().split('\n')) {
            if (line.startsWith("btime ")) {
                cached = line.mid(6).trimmed().toDouble();
                break;
            }
        }
    }
    return cached;
}

// tty_nr is packed: major in bits 8-19, minor split across the low byte and
// the top 12 bits. 136 is the pts major, 4 the classic virtual consoles.
QString ttyName(unsigned int ttyNr)
{
    if (ttyNr == 0)
        return {};
    const unsigned int major = (ttyNr >> 8) & 0xfff;
    const unsigned int minor = (ttyNr & 0xff) | ((ttyNr >> 12) & 0xfff00);
    if (major == 136)
        return QStringLiteral("pts/%1").arg(minor);
    if (major == 4)
        return QStringLiteral("tty%1").arg(minor);
    return {};
}

bool readStat(const QString &procDir, ProcInfo &info)
{
    QFile f(procDir + QStringLiteral("/stat"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray raw = f.readAll();

    // comm sits in parentheses and may itself contain spaces or ')'
    const int open = raw.indexOf('(');
    const int close = raw.lastIndexOf(')');
    if (open < 0 || close < open)
        return false;
    info.name = QString::fromUtf8(raw.mid(open + 1, close - open - 1));

    const QList<QByteArray> f2 =
        raw.mid(close + 2).simplified().split(' ');
    // indices after comm: 0 state, 1 ppid, 4 tty_nr, 11 utime, 12 stime,
    // 19 starttime, 21 rss (in pages)
    if (f2.size() < 22)
        return false;
    info.ppid = f2[1].toLongLong();
    info.tty = ttyName(f2[4].toUInt());
    const double utime = f2[11].toDouble();
    const double stime = f2[12].toDouble();
    info.cpuSeconds = (utime + stime) / clockTicks();
    info.startTime = bootTime() + f2[19].toDouble() / clockTicks();
    info.rss = f2[21].toLongLong() * pageSize();
    return true;
}

} // namespace

std::vector<ProcInfo> enumerateProcesses()
{
    std::vector<ProcInfo> out;
    QDir proc(QStringLiteral("/proc"));
    const QStringList entries =
        proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);

    for (const QString &entry : entries) {
        bool isPid = false;
        const qint64 pid = entry.toLongLong(&isPid);
        if (!isPid)
            continue;

        const QString dir = QStringLiteral("/proc/") + entry;
        ProcInfo info;
        info.pid = pid;
        if (!readStat(dir, info))
            continue;   // process died mid-scan

        QFile cmd(dir + QStringLiteral("/cmdline"));
        if (cmd.open(QIODevice::ReadOnly)) {
            const QByteArray raw = cmd.readAll();
            for (const QByteArray &arg : raw.split('\0')) {
                if (!arg.isEmpty())
                    info.cmdline << QString::fromUtf8(arg);
            }
        }
        if (info.cmdline.isEmpty())
            continue;   // kernel thread

        info.cwd = QFile::symLinkTarget(dir + QStringLiteral("/cwd"));
        out.push_back(std::move(info));
    }
    return out;
}

bool activateWindow(const std::vector<qint64> &pids)
{
    if (pids.empty())
        return false;

    QStringList list;
    for (qint64 pid : pids)
        list << QString::number(pid);

    // Runs inside the compositor: find a window whose pid is in the chain,
    // un-minimize it and make it active.
    const QString js = QStringLiteral(
        "var t=[%1];"
        "var ws=(typeof workspace.windowList==='function')"
        "?workspace.windowList():workspace.clientList();"
        "for(var i=0;i<ws.length;i++){if(t.indexOf(ws[i].pid)!==-1){"
        "if(ws[i].minimized)ws[i].minimized=false;"
        "workspace.activeWindow=ws[i];break;}}").arg(list.join(QLatin1Char(',')));

    QTemporaryFile script(QDir::tempPath() + QStringLiteral("/hop-hop-XXXXXX.js"));
    script.setAutoRemove(true);
    if (!script.open())
        return false;
    script.write(js.toUtf8());
    script.flush();

    const QString plugin = QStringLiteral("hophop_activate");
    QDBusInterface scripting(QStringLiteral("org.kde.KWin"),
                             QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"),
                             QDBusConnection::sessionBus());
    if (!scripting.isValid())
        return false;   // not KWin — nothing we can do from a Wayland client

    scripting.call(QStringLiteral("unloadScript"), plugin);
    const QDBusReply<int> id =
        scripting.call(QStringLiteral("loadScript"), script.fileName(), plugin);
    if (!id.isValid())
        return false;

    QDBusInterface loaded(QStringLiteral("org.kde.KWin"),
                          QStringLiteral("/Scripting/Script%1").arg(id.value()),
                          QStringLiteral("org.kde.kwin.Script"),
                          QDBusConnection::sessionBus());
    loaded.call(QStringLiteral("run"));
    scripting.call(QStringLiteral("unloadScript"), plugin);
    return true;
}

QString terminateProcess(qint64 pid, bool force)
{
    if (::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) == 0)
        return {};
    return QString::fromLocal8Bit(strerror(errno));
}

QString userConfigDir()
{
    const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
    if (!xdg.isEmpty())
        return QString::fromLocal8Bit(xdg);
    return QDir::homePath() + QStringLiteral("/.config");
}

QString runtimeDir()
{
    const QByteArray dir = qgetenv("XDG_RUNTIME_DIR");
    if (!dir.isEmpty())
        return QString::fromLocal8Bit(dir);
    return QDir::tempPath();
}

} // namespace platform

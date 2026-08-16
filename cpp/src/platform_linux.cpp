// Linux backend: /proc for process data, KWin scripting over D-Bus for
// window activation (a Wayland client may not raise another app's window,
// so the compositor has to do it for us).
#include "platform.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVirtualObject>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <functional>
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

// loadScript/run only say that the script started, not whether it found a
// window — the search happens inside the compositor. So the script calls back
// here over the bus and this object catches the verdict. A virtual object
// keeps it moc-free: there are no signals or slots to export.
class ActivationReport : public QDBusVirtualObject {
public:
    QString introspect(const QString &) const override { return {}; }

    bool handleMessage(const QDBusMessage &message, const QDBusConnection &) override
    {
        if (message.member() != QLatin1String("report"))
            return false;
        activated = message.arguments().value(0).toBool();
        received = true;
        if (onReport)
            onReport();
        return true;
    }

    bool activated = false;
    bool received = false;
    std::function<void()> onReport;
};

constexpr const char *kReportPath = "/hophop/activation";
constexpr const char *kReportInterface = "org.hophop.Activation";
constexpr int kReportTimeoutMs = 700;

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

bool activateWindow(const std::vector<qint64> &pids, const QStringList &captionHints)
{
    if (pids.empty())
        return false;

    QStringList list;
    for (qint64 pid : pids)
        list << QString::number(pid);

    QStringList quotedHints;
    for (const QString &hint : captionHints) {
        QString escaped = hint;
        escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        escaped.replace(QLatin1Char('"'), QLatin1String("\\\""));
        quotedHints << QLatin1Char('"') + escaped + QLatin1Char('"');
    }

    QDBusConnection bus = QDBusConnection::sessionBus();
    ActivationReport report;
    const QString sink = bus.baseService();
    const bool listening = !sink.isEmpty()
        && bus.registerVirtualObject(QLatin1String(kReportPath), &report);
    const auto unregister = qScopeGuard([&] {
        if (listening)
            bus.unregisterObject(QLatin1String(kReportPath));
    });

    // Runs inside the compositor: collect the windows whose pid is in the
    // chain, prefer the one whose title matches the best hint — an IDE with
    // two projects open is a single pid with two windows — then un-minimize
    // it, make it active and tell us whether anything matched at all.
    const QString js = QStringLiteral(
        "var t=[%1];var h=[%2];"
        "var ws=(typeof workspace.windowList==='function')"
        "?workspace.windowList():workspace.clientList();"
        "var m=[];"
        "for(var i=0;i<ws.length;i++){if(t.indexOf(ws[i].pid)!==-1)m.push(ws[i]);}"
        "var win=null;"
        "for(var k=0;k<h.length&&!win;k++){"
        "for(var j=0;j<m.length;j++){"
        "if(String(m[j].caption).toLowerCase().indexOf(h[k])!==-1){win=m[j];break;}}}"
        "if(!win&&m.length)win=m[0];"
        "if(win){if(win.minimized)win.minimized=false;"
        "workspace.activeWindow=win;}"
        "if('%3'.length)callDBus('%3','%4','%5','report',win!==null);")
        .arg(list.join(QLatin1Char(',')), quotedHints.join(QLatin1Char(',')),
             listening ? sink : QString(),
             QLatin1String(kReportPath), QLatin1String(kReportInterface));

    QTemporaryFile script(QDir::tempPath() + QStringLiteral("/hop-hop-XXXXXX.js"));
    script.setAutoRemove(true);
    if (!script.open()) {
        qWarning("hop-hop: activateWindow — cannot write the KWin script");
        return false;
    }
    script.write(js.toUtf8());
    script.flush();

    const QString plugin = QStringLiteral("hophop_activate");
    QDBusInterface scripting(QStringLiteral("org.kde.KWin"),
                             QStringLiteral("/Scripting"),
                             QStringLiteral("org.kde.kwin.Scripting"),
                             QDBusConnection::sessionBus());
    if (!scripting.isValid()) {
        qWarning("hop-hop: activateWindow — no KWin scripting on the bus: %s",
                 qUtf8Printable(scripting.lastError().message()));
        return false;   // not KWin — nothing we can do from a Wayland client
    }

    scripting.call(QStringLiteral("unloadScript"), plugin);
    const QDBusReply<int> id =
        scripting.call(QStringLiteral("loadScript"), script.fileName(), plugin);
    if (!id.isValid()) {
        qWarning("hop-hop: activateWindow — loadScript failed: %s",
                 qUtf8Printable(id.error().message()));
        return false;
    }

    QDBusInterface loaded(QStringLiteral("org.kde.KWin"),
                          QStringLiteral("/Scripting/Script%1").arg(id.value()),
                          QStringLiteral("org.kde.kwin.Script"),
                          QDBusConnection::sessionBus());
    const QDBusMessage ran = loaded.call(QStringLiteral("run"));
    if (ran.type() == QDBusMessage::ErrorMessage) {
        qWarning("hop-hop: activateWindow — run failed: %s",
                 qUtf8Printable(ran.errorMessage()));
        scripting.call(QStringLiteral("unloadScript"), plugin);
        return false;
    }
    scripting.call(QStringLiteral("unloadScript"), plugin);

    if (!listening)
        return true;   // nowhere to report back to; assume the script did its job

    // callDBus is queued inside the compositor, so the verdict usually lands
    // just after run() returns.
    if (!report.received) {
        QEventLoop loop;
        report.onReport = [&loop] { loop.quit(); };
        QTimer::singleShot(kReportTimeoutMs, &loop, &QEventLoop::quit);
        loop.exec(QEventLoop::ExcludeUserInputEvents);
        report.onReport = nullptr;
    }
    if (!report.received) {
        qWarning("hop-hop: activateWindow — no verdict from KWin within %d ms",
                 kReportTimeoutMs);
        return true;   // it may well have worked; don't claim otherwise
    }
    return report.activated;
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

#include "detect.h"
#include "platform.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QSet>

#include <algorithm>

namespace detect {

QString stripExeSuffix(const QString &lowerName)
{
    for (const QString &ext : {QStringLiteral(".exe"), QStringLiteral(".cmd"),
                               QStringLiteral(".bat")}) {
        if (lowerName.endsWith(ext))
            return lowerName.left(lowerName.size() - ext.size());
    }
    return lowerName;
}

QString baseName(const QString &arg)
{
    QString s = arg.toLower();
    s.replace('\\', '/');
    const int slash = s.lastIndexOf('/');
    if (slash >= 0)
        s = s.mid(slash + 1);
    return stripExeSuffix(s);
}

QString classify(const QString &name, const QStringList &cmdline,
                 const std::vector<Target> &targets, const QString &selfMark)
{
    // backslashes -> forward slashes so unix-style markers also match Windows
    QString joined = cmdline.join(QLatin1Char(' ')).toLower();
    joined.replace('\\', '/');
    if (joined.isEmpty())
        return {};
    if (!selfMark.isEmpty() && joined.contains(selfMark))
        return {};  // don't count the monitor itself

    QSet<QString> bases;
    for (int i = 0; i < cmdline.size() && i < 3; ++i)
        bases.insert(baseName(cmdline.at(i)));
    bases.insert(baseName(name));

    for (const Target &t : targets) {
        for (const QString &n : t.names) {
            if (bases.contains(n))
                return t.kind;
        }
        for (const QString &m : t.markers) {
            if (joined.contains(m))
                return t.kind;
        }
    }
    return {};
}

std::vector<qint64> ancestorPids(qint64 pid, const QHash<qint64, ProcInfo> &byPid)
{
    std::vector<qint64> out{pid};
    QSet<qint64> seen{pid};
    qint64 current = pid;

    while (true) {
        auto it = byPid.constFind(current);
        if (it == byPid.constEnd())
            break;
        const qint64 parent = it->ppid;
        if (parent <= 1 || seen.contains(parent))
            break;  // init, or a cycle in a racy snapshot
        auto pit = byPid.constFind(parent);
        if (pit != byPid.constEnd() && pit->name.compare("systemd", Qt::CaseInsensitive) == 0) {
            current = parent;   // skip it, but keep climbing
            seen.insert(parent);
            continue;
        }
        out.push_back(parent);
        seen.insert(parent);
        current = parent;
    }
    return out;
}

QStringList captionHints(const QString &cwd)
{
    QStringList hints;
    if (cwd.isEmpty())
        return hints;

    const QString home = QDir::homePath();
    QDir dir(QDir::cleanPath(cwd));
    // three levels are enough to reach the project root from a client started
    // in, say, cpp/src, and few enough that the last hint stays specific
    for (int level = 0; level < 3; ++level) {
        const QString path = dir.absolutePath();
        if (path == home || dir.isRoot())
            break;
        const QString name = dir.dirName().toLower();
        if (!name.isEmpty() && !hints.contains(name))
            hints << name;
        if (!dir.cdUp())
            break;
    }
    return hints;
}

std::vector<Client> Detector::collect(bool onlyWithTty)
{
    const QString selfMark = QCoreApplication::applicationName().toLower();
    const std::vector<ProcInfo> procs = platform::enumerateProcesses();
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    const qint64 selfPid = QCoreApplication::applicationPid();

    snapshot_.clear();
    snapshot_.reserve(static_cast<int>(procs.size()));
    for (const ProcInfo &p : procs)
        snapshot_.insert(p.pid, p);

    // pass 1: which processes match at all
    QHash<qint64, QString> matched;
    for (const ProcInfo &p : procs) {
        if (p.pid == selfPid)
            continue;
        const QString kind = classify(p.name, p.cmdline, targets_, selfMark);
        if (!kind.isEmpty())
            matched.insert(p.pid, kind);
    }

    // pass 2: drop anything descending from another match, so an Electron
    // renderer or a helper shell doesn't show up as its own client
    const double interval = prevSampleTime_ > 0 ? now - prevSampleTime_ : 0;
    std::vector<Client> out;
    out.reserve(matched.size());

    for (auto it = matched.constBegin(); it != matched.constEnd(); ++it) {
        const ProcInfo &p = snapshot_.value(it.key());

        bool hasMatchedAncestor = false;
        qint64 walk = p.ppid;
        QSet<qint64> seen;
        while (walk > 1 && !seen.contains(walk)) {
            if (matched.contains(walk)) {
                hasMatchedAncestor = true;
                break;
            }
            seen.insert(walk);
            auto pit = snapshot_.constFind(walk);
            if (pit == snapshot_.constEnd())
                break;
            walk = pit->ppid;
        }
        if (hasMatchedAncestor)
            continue;

        if (onlyWithTty && p.tty.isEmpty())
            continue;

        Client c;
        c.pid = p.pid;
        c.kind = it.value();
        c.cwd = p.cwd;
        c.tty = p.tty;
        c.cmd = p.cmdline.join(QLatin1Char(' '));
        c.uptime = p.startTime > 0 ? std::max(0.0, now - p.startTime) : 0;
        c.rss = p.rss;

        if (interval > 0.05 && prevCpuSeconds_.contains(p.pid)) {
            const double delta = p.cpuSeconds - prevCpuSeconds_.value(p.pid);
            c.cpu = std::clamp(delta / interval * 100.0, 0.0, 100.0 * 512);
        }
        out.push_back(std::move(c));
    }

    prevCpuSeconds_.clear();
    for (const ProcInfo &p : procs)
        prevCpuSeconds_.insert(p.pid, p.cpuSeconds);
    prevSampleTime_ = now;

    std::sort(out.begin(), out.end(), [](const Client &a, const Client &b) {
        if (a.kind != b.kind)
            return a.kind < b.kind;
        return a.uptime > b.uptime;
    });
    return out;
}

QString formatUptime(double seconds)
{
    qint64 s = static_cast<qint64>(seconds);
    if (s < 0)
        s = 0;
    const qint64 d = s / 86400; s %= 86400;
    const qint64 h = s / 3600;  s %= 3600;
    const qint64 m = s / 60;    s %= 60;
    if (d > 0)
        return QStringLiteral("%1d %2h").arg(d).arg(h);
    if (h > 0)
        return QStringLiteral("%1:%2:%3").arg(h)
            .arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

} // namespace detect

#pragma once

#include "client.h"

#include <QHash>
#include <QString>
#include <vector>

namespace detect {

// "CODEX.EXE" -> "codex". Applied to both config names and process names so
// one config entry covers every platform's spelling.
QString stripExeSuffix(const QString &lowerName);

// Executable name out of an argv entry, unix or Windows path.
QString baseName(const QString &arg);

// Which kind a process belongs to, or an empty string. `selfMark` is the
// monitor's own binary name so it never counts itself.
QString classify(const QString &name, const QStringList &cmdline,
                 const std::vector<Target> &targets, const QString &selfMark);

// Client pid followed by its ancestors, skipping systemd/init: half the
// desktop hangs off `systemd --user`, so matching it produces false hits.
std::vector<qint64> ancestorPids(qint64 pid, const QHash<qint64, ProcInfo> &byPid);

// Lowercase window-title hints for a client's working directory, deepest
// component first: /home/me/Projects/hop-hop/cpp -> {"cpp", "hop-hop"}. IDEs
// and terminals put the project or directory name in the title, which is the
// only way to tell two windows of one process apart. Stops at $HOME so the
// hints never degrade into "home" or the user name.
QStringList captionHints(const QString &cwd);

// Holds the previous CPU sample, so cpu% is measured over the refresh
// interval instead of the process lifetime.
class Detector {
public:
    void setTargets(std::vector<Target> targets) { targets_ = std::move(targets); }
    const std::vector<Target> &targets() const { return targets_; }

    std::vector<Client> collect(bool onlyWithTty);

    // Snapshot of the last enumeration, for walking ancestor chains.
    const QHash<qint64, ProcInfo> &lastSnapshot() const { return snapshot_; }

private:
    std::vector<Target> targets_;
    QHash<qint64, ProcInfo> snapshot_;
    QHash<qint64, double> prevCpuSeconds_;
    double prevSampleTime_ = 0;
};

QString formatUptime(double seconds);

} // namespace detect

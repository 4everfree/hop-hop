// hop-hop — tray indicator + window listing the running AI coding clients.
//
//   hop-hop           GUI
//   hop-hop --list    print to stdout (check detection)
//   hop-hop --tray    start minimized to the tray
//
// Which clients are tracked lives in config/clients.toml.
#include "config.h"
#include "detect.h"
#include "main_window.h"
#include "platform.h"
#include "single_instance.h"

#include <QApplication>
#include <QIcon>
#include <QMap>
#include <QSystemTrayIcon>
#include <QTextStream>

namespace {

int printList(const std::vector<Target> &targets, const QString &loadedFrom)
{
    QTextStream out(stdout);
    detect::Detector detector;
    detector.setTargets(targets);
    // first pass only seeds the CPU baseline; one sample is enough for a list
    const std::vector<Client> clients = detector.collect(false);

    if (clients.empty()) {
        out << "Nothing found. Check "
            << (loadedFrom.isEmpty() ? QStringLiteral("the built-in defaults") : loadedFrom)
            << "\n";
        return 0;
    }

    out << "config: "
        << (loadedFrom.isEmpty() ? QStringLiteral("built-in defaults") : loadedFrom)
        << "\n";

    QMap<QString, int> counts;
    for (const Client &c : clients)
        counts[c.kind] += 1;
    QStringList parts;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        parts << QStringLiteral("%1: %2").arg(it.key()).arg(it.value());
    out << parts.join(QStringLiteral("  "))
        << QStringLiteral(" (total %1)\n\n").arg(clients.size());

    for (const Client &c : clients) {
        out << QStringLiteral("%1 pid=%2 tty=%3 %4  %5 MB  %6\n")
                   .arg(c.kind, -7)
                   .arg(c.pid, -8)
                   .arg(c.tty.isEmpty() ? QStringLiteral("-") : c.tty, -8)
                   .arg(detect::formatUptime(c.uptime), 9)
                   .arg(c.rss / 1048576, 5)
                   .arg(c.cwd);
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("hop-hop"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    // compiled into the binary, so it works regardless of install layout
    QIcon appIcon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        appIcon.addFile(QStringLiteral(":/icons/hop-hop-%1.png").arg(size));
    QApplication::setWindowIcon(appIcon);

    const QStringList args = QCoreApplication::arguments();

    QString loadedFrom;
    std::vector<Target> targets = config::loadTargets(&loadedFrom);

    if (args.contains(QStringLiteral("--list")))
        return printList(targets, loadedFrom);

    // --activate PID: raise the window owning that client, then exit. Handy
    // for scripting, and it exercises the platform layer on its own.
    const int activateAt = args.indexOf(QStringLiteral("--activate"));
    if (activateAt >= 0) {
        if (activateAt + 1 >= args.size()) {
            QTextStream(stderr) << "--activate needs a pid\n";
            return 2;
        }
        const qint64 pid = args.at(activateAt + 1).toLongLong();
        QHash<qint64, ProcInfo> byPid;
        for (const ProcInfo &p : platform::enumerateProcesses())
            byPid.insert(p.pid, p);
        const bool ok = platform::activateWindow(
            detect::ancestorPids(pid, byPid),
            detect::captionHints(byPid.value(pid).cwd));
        QTextStream(stdout) << (ok ? "activated\n" : "no window found\n");
        return ok ? 0 : 1;
    }

    // One monitor at a time; a second launch raises the first one's window.
    SingleInstance instance;
    if (!instance.acquire()) {
        if (instance.otherPid() > 0) {
            const auto snapshot = [] {
                QHash<qint64, ProcInfo> byPid;
                for (const ProcInfo &p : platform::enumerateProcesses())
                    byPid.insert(p.pid, p);
                return byPid;
            }();
            platform::activateWindow(detect::ancestorPids(instance.otherPid(), snapshot));
        }
        QTextStream(stderr) << "hop-hop is already running — raised its window.\n";
        return 1;
    }

    if (!QSystemTrayIcon::isSystemTrayAvailable())
        QTextStream(stderr) << "warning: no system tray on this desktop\n";

    QApplication::setQuitOnLastWindowClosed(false);
    MainWindow window(std::move(targets));
    if (args.contains(QStringLiteral("--tray")))
        window.hide();
    else
        window.show();

    return app.exec();
}

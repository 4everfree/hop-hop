// macOS backend: libproc + sysctl for process data, NSRunningApplication for
// window activation.
//
// NOT COMPILE-TESTED — written on Linux, where no macOS SDK is available.
// Expect to fix small things on the first build.
#include "platform.h"

#include <QDir>
#include <QStandardPaths>

#import <AppKit/AppKit.h>

#include <libproc.h>
#include <stdlib.h>              // devname
#include <sys/param.h>           // NODEV
#include <sys/proc_info.h>
#include <sys/stat.h>            // S_IFCHR
#include <sys/sysctl.h>
#include <sys/types.h>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

namespace platform {
namespace {

// KERN_PROCARGS2 hands back: int argc, the exec path, NUL padding, then argc
// NUL-separated argv strings.
QStringList commandLineOf(pid_t pid)
{
    static int argMax = 0;
    if (argMax == 0) {
        int mib[2] = {CTL_KERN, KERN_ARGMAX};
        size_t size = sizeof(argMax);
        if (sysctl(mib, 2, &argMax, &size, nullptr, 0) != 0 || argMax <= 0)
            argMax = 262144;
    }

    std::vector<char> buffer(static_cast<size_t>(argMax));
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, pid};
    size_t size = buffer.size();
    if (sysctl(mib, 3, buffer.data(), &size, nullptr, 0) != 0 || size < sizeof(int))
        return {};

    int argc = 0;
    std::memcpy(&argc, buffer.data(), sizeof(argc));

    size_t pos = sizeof(argc);
    // skip the exec path, then any NUL padding before argv[0]
    while (pos < size && buffer[pos] != '\0')
        ++pos;
    while (pos < size && buffer[pos] == '\0')
        ++pos;

    QStringList args;
    for (int i = 0; i < argc && pos < size; ++i) {
        const size_t start = pos;
        while (pos < size && buffer[pos] != '\0')
            ++pos;
        if (pos > start)
            args << QString::fromUtf8(buffer.data() + start,
                                      static_cast<int>(pos - start));
        ++pos;   // step over the terminator
    }
    return args;
}

bool kinfoFor(pid_t pid, struct kinfo_proc &out)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    size_t size = sizeof(out);
    return sysctl(mib, 4, &out, &size, nullptr, 0) == 0 && size > 0;
}

} // namespace

std::vector<ProcInfo> enumerateProcesses()
{
    std::vector<ProcInfo> out;

    const int needed = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (needed <= 0)
        return out;
    std::vector<pid_t> pids(static_cast<size_t>(needed) / sizeof(pid_t) + 64, 0);
    const int got = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                                  static_cast<int>(pids.size() * sizeof(pid_t)));
    if (got <= 0)
        return out;
    const size_t count = static_cast<size_t>(got) / sizeof(pid_t);

    for (size_t i = 0; i < count; ++i) {
        const pid_t pid = pids[i];
        if (pid <= 0)
            continue;

        ProcInfo info;
        info.pid = pid;
        info.cmdline = commandLineOf(pid);
        if (info.cmdline.isEmpty())
            continue;   // kernel task, or a process we may not inspect

        char path[PROC_PIDPATHINFO_MAXSIZE] = {0};
        if (proc_pidpath(pid, path, sizeof(path)) > 0) {
            const QString full = QString::fromUtf8(path);
            info.name = full.mid(full.lastIndexOf('/') + 1);
        }

        struct kinfo_proc kp;
        if (kinfoFor(pid, kp)) {
            info.ppid = kp.kp_eproc.e_ppid;
            info.startTime = static_cast<double>(kp.kp_proc.p_starttime.tv_sec)
                           + kp.kp_proc.p_starttime.tv_usec / 1e6;
            if (info.name.isEmpty())
                info.name = QString::fromUtf8(kp.kp_proc.p_comm);
            const dev_t tdev = kp.kp_eproc.e_tdev;
            if (tdev != NODEV) {
                if (const char *name = devname(tdev, S_IFCHR))
                    info.tty = QString::fromUtf8(name);
            }
        }

        struct proc_taskinfo task;
        if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task)) ==
            sizeof(task)) {
            // task times are nanoseconds
            info.cpuSeconds = (static_cast<double>(task.pti_total_user)
                             + static_cast<double>(task.pti_total_system)) / 1e9;
            info.rss = static_cast<qint64>(task.pti_resident_size);
        }

        struct proc_vnodepathinfo vpi;
        if (proc_pidinfo(pid, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi)) ==
            sizeof(vpi)) {
            info.cwd = QString::fromUtf8(vpi.pvi_cdir.vip_path);
        }

        out.push_back(std::move(info));
    }
    return out;
}

// captionHints are unused here: NSRunningApplication activates an application
// with all its windows, and picking one of them would need the accessibility
// API and the permission prompt that comes with it.
bool activateWindow(const std::vector<qint64> &pids, const QStringList &)
{
    @autoreleasepool {
        for (qint64 pid : pids) {
            NSRunningApplication *app = [NSRunningApplication
                runningApplicationWithProcessIdentifier:static_cast<pid_t>(pid)];
            if (!app)
                continue;   // this ancestor owns no GUI application

            // Primary path. NSRunningApplication has no -activate; that is an
            // NSApplication method. The macOS 14 replacement is
            // -activateFromApplication:options:. In GUI mode hop-hop is itself
            // the active app when a row is double-clicked, so this succeeds and
            // the Apple Event fallback below never runs.
            BOOL ok;
            if (@available(macOS 14.0, *)) {
                ok = [app activateFromApplication:[NSRunningApplication
                                                      currentApplication]
                                          options:NSApplicationActivateAllWindows];
            } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                ok = [app activateWithOptions:NSApplicationActivateAllWindows];
#pragma clang diagnostic pop
            }
            if (ok)
                return true;

            // Fallback for the standalone --activate CLI. macOS 14+ cooperative
            // activation refuses a process that is not itself frontmost — the
            // call above returns NO and nothing comes forward. An Apple Event
            // asks the target to raise itself, which the system honors whatever
            // our foreground state is. A GUI app always has a bundle id to
            // address; if it somehow lacks one there is nothing to send to.
            NSString *bundleId = app.bundleIdentifier;
            if (bundleId.length == 0)
                return false;
            NSString *source = [NSString stringWithFormat:
                @"tell application id \"%@\" to activate", bundleId];
            NSDictionary *scriptError = nil;
            [[[NSAppleScript alloc] initWithSource:source]
                executeAndReturnError:&scriptError];
            return scriptError == nil;
        }
    }
    return false;
}

QString terminateProcess(qint64 pid, bool force)
{
    if (::kill(static_cast<pid_t>(pid), force ? SIGKILL : SIGTERM) == 0)
        return {};
    return QString::fromLocal8Bit(std::strerror(errno));
}

QString userConfigDir()
{
    return QDir::homePath() + QStringLiteral("/Library/Application Support");
}

QString runtimeDir()
{
    return QDir::tempPath();
}

} // namespace platform

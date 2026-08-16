// Everything the OS has to answer that Qt won't. One implementation per
// platform: platform_linux.cpp, platform_macos.mm, platform_windows.cpp.
#pragma once

#include "client.h"

#include <QString>
#include <vector>

namespace platform {

// Every process owned by the current user that we can read. Fields the OS
// refuses to disclose are left empty/zero rather than dropping the process.
std::vector<ProcInfo> enumerateProcesses();

// Raise the window belonging to any of `pids` (client pid first, then its
// ancestors). `captionHints` disambiguates between several windows of one
// process — an IDE with two projects open is one process with one pid, so the
// pid alone would always land on whichever window came first. Hints are
// lowercase substrings tried against the window title, best first; a window
// matching none of them is still used when nothing better turns up.
// Returns false when no window matched or the request failed.
bool activateWindow(const std::vector<qint64> &pids,
                    const QStringList &captionHints = {});

// Ask a process to quit; force = kill without cleanup.
// Returns an empty string on success, an error message otherwise.
QString terminateProcess(qint64 pid, bool force);

// Where per-machine config lives: %APPDATA%, ~/Library/Application Support,
// $XDG_CONFIG_HOME. Empty if it cannot be determined.
QString userConfigDir();

// Directory for the single-instance lock file.
QString runtimeDir();

} // namespace platform

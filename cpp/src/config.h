#pragma once

#include "client.h"

#include <QString>
#include <QStringList>
#include <vector>

namespace config {

// Search order, first hit wins and fully replaces the set:
//   <userConfigDir>/hop-hop/clients.toml   — per-machine override
//   <exeDir>/clients.toml                  — local override
//   <exeDir>/../../config/clients.toml     — shared file in the repo
QStringList searchPaths();

// Falls back to built-in defaults when nothing parses. `loadedFrom` receives
// the file actually used, or an empty string for the defaults.
std::vector<Target> loadTargets(QString *loadedFrom = nullptr);

std::vector<Target> defaultTargets();

} // namespace config

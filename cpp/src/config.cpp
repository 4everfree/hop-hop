#include "config.h"
#include "detect.h"
#include "platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>

#include <toml.hpp>

namespace config {

QStringList searchPaths()
{
    QStringList paths;
    const QString userDir = platform::userConfigDir();
    if (!userDir.isEmpty())
        paths << QDir(userDir).filePath("hop-hop/clients.toml");

    const QString exeDir = QCoreApplication::applicationDirPath();
    paths << QDir(exeDir).filePath("clients.toml");
    // build tree:   <repo>/cpp/build/hop-hop -> ../../config/clients.toml
    // install tree: <prefix>/bin/hop-hop     -> ../config/clients.toml
    paths << QDir::cleanPath(QDir(exeDir).filePath("../../config/clients.toml"));
    paths << QDir::cleanPath(QDir(exeDir).filePath("../config/clients.toml"));
    return paths;
}

std::vector<Target> defaultTargets()
{
    return {
        Target{"claude", "#D97757",
               {"claude", "claude-code"},
               {"@anthropic-ai/claude-code", "/claude/cli.js", ".claude/local/"}},
        Target{"agy", "#5A9BF6", {"agy", "antigravity"}, {"antigravity"}},
    };
}

namespace {

QStringList toStringList(const toml::node *node, bool normalizePath)
{
    QStringList out;
    const auto *arr = node ? node->as_array() : nullptr;
    if (!arr)
        return out;
    for (const auto &item : *arr) {
        if (const auto val = item.value<std::string>()) {
            QString s = QString::fromStdString(*val).toLower();
            if (normalizePath)
                s.replace('\\', '/');   // markers are written unix-style
            else
                s = detect::stripExeSuffix(s);
            if (!s.isEmpty())
                out << s;
        }
    }
    return out;
}

} // namespace

std::vector<Target> loadTargets(QString *loadedFrom)
{
    QTextStream err(stderr);
    for (const QString &path : searchPaths()) {
        if (!QFileInfo::exists(path))
            continue;

        toml::table table;
        try {
            table = toml::parse_file(path.toStdString());
        } catch (const toml::parse_error &e) {
            err << "[config] " << path << ": "
                << QString::fromUtf8(e.description().data(),
                                     static_cast<int>(e.description().size()))
                << "\n";
            continue;
        }

        // toml++ stores tables sorted by key, but precedence when a process
        // matches several kinds is defined by the order they appear in the
        // file — so restore that from the source positions.
        std::vector<std::pair<uint32_t, Target>> ordered;
        for (const auto &[key, value] : table) {
            const auto *entry = value.as_table();
            if (!entry)
                continue;
            Target t;
            t.kind = QString::fromStdString(std::string(key.str()));
            t.color = QString::fromStdString(
                (*entry)["color"].value_or(std::string("#888888")));
            t.names = toStringList((*entry)["names"].node(), false);
            t.markers = toStringList((*entry)["markers"].node(), true);
            if (!t.names.isEmpty() || !t.markers.isEmpty())
                ordered.emplace_back(value.source().begin.line, std::move(t));
        }
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const auto &a, const auto &b) { return a.first < b.first; });

        std::vector<Target> targets;
        targets.reserve(ordered.size());
        for (auto &[line, target] : ordered)
            targets.push_back(std::move(target));

        if (!targets.empty()) {
            if (loadedFrom)
                *loadedFrom = QFileInfo(path).absoluteFilePath();
            return targets;
        }
    }

    if (loadedFrom)
        loadedFrom->clear();
    return defaultTargets();
}

} // namespace config

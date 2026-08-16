// Shared types. See ../../README.md#detection-contract for the behaviour
// these structures exist to implement.
#pragma once

#include <QString>
#include <QStringList>
#include <vector>

// One client kind from clients.toml.
struct Target {
    QString kind;       // label shown in the UI, e.g. "claude"
    QString color;      // hex, e.g. "#D97757"
    QStringList names;  // lowercased, extension already stripped
    QStringList markers;// lowercased, forward slashes
};

// Raw per-process data the platform layer digs out of the OS.
struct ProcInfo {
    qint64 pid = 0;
    qint64 ppid = 0;
    QString name;        // executable name
    QStringList cmdline; // argv
    QString cwd;         // may be empty where the OS won't tell us
    QString tty;         // Unix only; empty elsewhere
    double startTime = 0;   // unix epoch seconds
    double cpuSeconds = 0;  // total CPU time consumed so far
    qint64 rss = 0;         // bytes
};

// A matched client, ready for display.
struct Client {
    qint64 pid = 0;
    QString kind;
    QString cwd;
    QString tty;
    QString cmd;
    double uptime = 0;  // seconds
    double cpu = 0;     // percent, averaged over the last refresh interval
    qint64 rss = 0;     // bytes
};

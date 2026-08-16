#pragma once

#include <QString>
#include <QtGlobal>

// Keeps exactly one monitor in memory. On POSIX an flock is held for the
// process lifetime — the kernel drops it even on SIGKILL, so no stale locks.
// On Windows a named mutex does the same job, with the pid kept in a side
// file so a second launch knows whose window to raise.
class SingleInstance {
public:
    ~SingleInstance();

    // True when we are the only instance. False means another one holds the
    // lock; `otherPid` then carries its pid (0 if unreadable).
    bool acquire();
    qint64 otherPid() const { return otherPid_; }
    QString lockPath() const { return lockPath_; }

private:
    void release();

    QString lockPath_;
    qint64 otherPid_ = 0;
    int fd_ = -1;        // POSIX
    void *handle_ = nullptr; // Windows mutex
};

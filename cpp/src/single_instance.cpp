#include "single_instance.h"
#include "platform.h"

#include <QDir>
#include <QFile>

#ifdef Q_OS_WIN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <sys/file.h>
#  include <unistd.h>
#endif

SingleInstance::~SingleInstance()
{
    release();
}

bool SingleInstance::acquire()
{
    lockPath_ = QDir(platform::runtimeDir()).filePath(QStringLiteral("hop-hop.lock"));

#ifdef Q_OS_WIN
    handle_ = CreateMutexW(nullptr, TRUE, L"Local\\hop-hop-single-instance");
    const bool taken = (handle_ == nullptr) || (GetLastError() == ERROR_ALREADY_EXISTS);
    if (taken) {
        QFile f(lockPath_);
        if (f.open(QIODevice::ReadOnly))
            otherPid_ = f.readAll().trimmed().toLongLong();
        if (handle_) {
            CloseHandle(static_cast<HANDLE>(handle_));
            handle_ = nullptr;
        }
        return false;
    }
    QFile f(lockPath_);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QByteArray::number(static_cast<qint64>(GetCurrentProcessId())));
    return true;
#else
    fd_ = ::open(lockPath_.toLocal8Bit().constData(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
        return true;   // can't lock — don't block startup over it

    if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
        char buf[32] = {0};
        const ssize_t n = ::read(fd_, buf, sizeof(buf) - 1);
        if (n > 0)
            otherPid_ = QByteArray(buf, static_cast<int>(n)).trimmed().toLongLong();
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    if (::ftruncate(fd_, 0) != 0)
        return true;
    const QByteArray pid = QByteArray::number(static_cast<qint64>(::getpid()));
    const ssize_t written = ::write(fd_, pid.constData(), pid.size());
    Q_UNUSED(written);
    ::fsync(fd_);
    return true;   // fd stays open: the lock lives as long as we do
#endif
}

void SingleInstance::release()
{
#ifdef Q_OS_WIN
    if (handle_) {
        ReleaseMutex(static_cast<HANDLE>(handle_));
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
        QFile::remove(lockPath_);
    }
#else
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
        fd_ = -1;
        QFile::remove(lockPath_);
    }
#endif
}

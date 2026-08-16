// Windows backend: Toolhelp for the process tree, one WMI query for every
// command line, EnumWindows for activation.
//
// NOT COMPILE-TESTED — written on Linux, where no Windows SDK is available.
// Expect to fix small things on the first build.
#include "platform.h"

#include <QDir>
#include <QHash>

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <wbemidl.h>

namespace platform {
namespace {

double fileTimeToUnix(const FILETIME &ft)
{
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    // 100-ns ticks since 1601 -> seconds since 1970
    constexpr double kTicksPerSecond = 1e7;
    constexpr double kEpochDelta = 11644473600.0;
    return static_cast<double>(v.QuadPart) / kTicksPerSecond - kEpochDelta;
}

double fileTimeToSeconds(const FILETIME &ft)
{
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return static_cast<double>(v.QuadPart) / 1e7;
}

// One WMI round trip for all command lines; querying per process would be
// unusably slow at a 2-second refresh.
QHash<qint64, QString> commandLinesViaWmi()
{
    QHash<qint64, QString> out;

    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool weInitialized = SUCCEEDED(init);
    if (init == RPC_E_CHANGED_MODE)
        return out;

    // Security only needs to be set once per process; a repeat call is benign.
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                         RPC_C_AUTHN_LEVEL_DEFAULT, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_NONE, nullptr);

    IWbemLocator *locator = nullptr;
    IWbemServices *services = nullptr;
    IEnumWbemClassObject *rows = nullptr;

    // plain BSTRs rather than _bstr_t: that wrapper drags in comsuppw, which
    // MinGW does not ship
    BSTR nameSpace = SysAllocString(L"ROOT\\CIMV2");
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT ProcessId, CommandLine FROM Win32_Process");

    do {
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator,
                                    reinterpret_cast<LPVOID *>(&locator))))
            break;
        if (FAILED(locator->ConnectServer(nameSpace, nullptr, nullptr,
                                          nullptr, 0, nullptr, nullptr, &services)))
            break;
        if (FAILED(CoSetProxyBlanket(services, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE,
                                     nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                     RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE)))
            break;
        if (FAILED(services->ExecQuery(
                language, query,
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                nullptr, &rows)))
            break;

        IWbemClassObject *row = nullptr;
        ULONG returned = 0;
        while (rows->Next(WBEM_INFINITE, 1, &row, &returned) == S_OK && returned) {
            VARIANT pidVar;
            VariantInit(&pidVar);
            VARIANT cmdVar;
            VariantInit(&cmdVar);

            if (SUCCEEDED(row->Get(L"ProcessId", 0, &pidVar, nullptr, nullptr))
                && SUCCEEDED(row->Get(L"CommandLine", 0, &cmdVar, nullptr, nullptr))
                && cmdVar.vt == VT_BSTR && cmdVar.bstrVal) {
                out.insert(static_cast<qint64>(pidVar.uintVal),
                           QString::fromWCharArray(cmdVar.bstrVal));
            }
            VariantClear(&pidVar);
            VariantClear(&cmdVar);
            row->Release();
        }
    } while (false);

    SysFreeString(nameSpace);
    SysFreeString(language);
    SysFreeString(query);
    if (rows) rows->Release();
    if (services) services->Release();
    if (locator) locator->Release();
    if (weInitialized) CoUninitialize();
    return out;
}

// Windows hands the command line over as one string; split it the way the
// CRT would so argv[0..2] can be inspected like everywhere else.
QStringList splitCommandLine(const QString &line)
{
    QStringList args;
    if (line.isEmpty())
        return args;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(line.utf16()), &argc);
    if (!argv)
        return {line};
    for (int i = 0; i < argc; ++i)
        args << QString::fromWCharArray(argv[i]);
    LocalFree(argv);
    return args;
}

struct WindowSearch {
    const std::vector<qint64> *pids;
    const QStringList *hints;
    HWND first;      // first window of a matching process
    HWND best;       // the one whose title matched a hint
    int bestRank;    // index into hints; lower is a better match
};

BOOL CALLBACK findWindowForPid(HWND hwnd, LPARAM param)
{
    auto *search = reinterpret_cast<WindowSearch *>(param);
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr)
        return TRUE;   // skip tool windows and popups

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    bool ours = false;
    for (qint64 candidate : *search->pids) {
        if (static_cast<qint64>(pid) == candidate) {
            ours = true;
            break;
        }
    }
    if (!ours)
        return TRUE;

    if (!search->first)
        search->first = hwnd;
    if (search->hints->isEmpty())
        return FALSE;   // nothing to distinguish windows by — take the first

    constexpr int kCaptionMax = 512;
    wchar_t buffer[kCaptionMax] = {0};
    GetWindowTextW(hwnd, buffer, kCaptionMax);
    const QString caption = QString::fromWCharArray(buffer).toLower();
    for (int rank = 0; rank < search->hints->size() && rank < search->bestRank; ++rank) {
        if (caption.contains(search->hints->at(rank))) {
            search->best = hwnd;
            search->bestRank = rank;
            break;
        }
    }
    return search->bestRank == 0 ? FALSE : TRUE;   // can't do better than the first hint
}

struct CloseRequest {
    DWORD pid;
    bool posted;
};

BOOL CALLBACK postCloseToPid(HWND hwnd, LPARAM param)
{
    auto *request = reinterpret_cast<CloseRequest *>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == request->pid) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        request->posted = true;
    }
    return TRUE;
}

} // namespace

std::vector<ProcInfo> enumerateProcesses()
{
    std::vector<ProcInfo> out;

    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return out;

    const QHash<qint64, QString> commandLines = commandLinesViaWmi();

    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            ProcInfo info;
            info.pid = static_cast<qint64>(entry.th32ProcessID);
            info.ppid = static_cast<qint64>(entry.th32ParentProcessID);
            info.name = QString::fromWCharArray(entry.szExeFile);

            const QString line = commandLines.value(info.pid);
            info.cmdline = line.isEmpty() ? QStringList{info.name}
                                          : splitCommandLine(line);

            // VM_READ lets older psapi report memory; fall back to the
            // limited right, which is all a non-elevated process may get
            HANDLE process = OpenProcess(
                PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE,
                entry.th32ProcessID);
            if (!process)
                process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      entry.th32ProcessID);
            if (process) {
                FILETIME creation, exit, kernel, user;
                if (GetProcessTimes(process, &creation, &exit, &kernel, &user)) {
                    info.startTime = fileTimeToUnix(creation);
                    info.cpuSeconds = fileTimeToSeconds(kernel)
                                    + fileTimeToSeconds(user);
                }
                PROCESS_MEMORY_COUNTERS counters;
                if (GetProcessMemoryInfo(process, &counters, sizeof(counters)))
                    info.rss = static_cast<qint64>(counters.WorkingSetSize);
                CloseHandle(process);
            }

            // No TTY concept here, and the working directory would need a PEB
            // read; both stay empty and the UI falls back to the parent.
            out.push_back(std::move(info));
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return out;
}

bool activateWindow(const std::vector<qint64> &pids, const QStringList &captionHints)
{
    if (pids.empty())
        return false;

    WindowSearch search{&pids, &captionHints, nullptr, nullptr,
                        captionHints.size()};
    EnumWindows(findWindowForPid, reinterpret_cast<LPARAM>(&search));
    HWND target = search.best ? search.best : search.first;
    if (!target)
        return false;

    if (IsIconic(target))
        ShowWindow(target, SW_RESTORE);

    if (SetForegroundWindow(target))
        return true;

    // Windows refuses focus theft unless we are already the foreground app;
    // flashing the taskbar button is the sanctioned consolation prize.
    FLASHWINFO flash = {};
    flash.cbSize = sizeof(flash);
    flash.hwnd = target;
    flash.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
    flash.uCount = 3;
    FlashWindowEx(&flash);
    return false;
}

QString terminateProcess(qint64 pid, bool force)
{
    if (!force) {
        // closest thing to SIGTERM: ask the windows to close
        CloseRequest request{static_cast<DWORD>(pid), false};
        EnumWindows(postCloseToPid, reinterpret_cast<LPARAM>(&request));
        if (request.posted)
            return {};
    }

    const HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
                                       static_cast<DWORD>(pid));
    if (!process)
        return QStringLiteral("OpenProcess failed (error %1)").arg(GetLastError());

    const BOOL ok = TerminateProcess(process, 1);
    const DWORD error = GetLastError();
    CloseHandle(process);
    if (ok)
        return {};
    return QStringLiteral("TerminateProcess failed (error %1)").arg(error);
}

QString userConfigDir()
{
    const QByteArray appData = qgetenv("APPDATA");
    if (!appData.isEmpty())
        return QString::fromLocal8Bit(appData);
    return QDir::homePath();
}

QString runtimeDir()
{
    return QDir::tempPath();
}

} // namespace platform

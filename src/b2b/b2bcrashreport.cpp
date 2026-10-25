#include "b2b/b2bcrashreport.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QTextStream>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <thread>

#include "b2b/version.hpp"
#include "util/fatalhook.h"
#include "util/logging.h"
#include "util/versionstore.h"

#ifdef Q_OS_WIN
// clang-format off
#include <windows.h>
#include <dbghelp.h>
#include <winevt.h>
// clang-format on
#include <crtdbg.h>
#else
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#endif

namespace mixxx::b2b {

namespace {

// Everything the handlers need, ready before anything goes wrong (a crashing
// process can't be trusted to allocate).
char s_header[2048];
std::atomic<bool> s_handling{false};

// The main thread's heartbeat (a timer on it notes the time, twice a
// second): if it stops for long, the app has frozen.
std::atomic<long long> s_beat{0};
constexpr long long kFrozenMs = 15000;

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

// "This run is going": removed when the app ends normally. If it's still
// there at the next start, the app ended without saying why (killed while
// frozen, or a crash nothing in the app can catch).
char s_marker[4096];

void removeMarker() {
    std::remove(s_marker);
}

void stamp(char* out, size_t size, const char* suffix) {
    const std::time_t t = std::time(nullptr);
    std::tm local{};
#ifdef Q_OS_WIN
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    std::snprintf(out, size, "crash-%04d%02d%02d-%02d%02d%02d%s", local.tm_year + 1900, local.tm_mon + 1,
            local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec, suffix);
}

#ifdef Q_OS_WIN
wchar_t s_dir[MAX_PATH];
DWORD s_mainThreadId = 0;
DWORD s_watchdogThreadId = 0;
// A crash is written up by the watchdog thread: it has a stack of its own
// (the crashing thread may have none left) and isn't in the middle of
// whatever went wrong.
HANDLE s_crashRequest = nullptr;
HANDLE s_crashDone = nullptr;
EXCEPTION_POINTERS* s_pCrashPointers = nullptr;
DWORD s_crashThreadId = 0;
char s_crashWhat[256];

void writeText(HANDLE file, const char* text) {
    DWORD written = 0;
    WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

const char* exceptionName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation (used memory it shouldn't)";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "divide by zero";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error";
    case 0xC0000374: // STATUS_HEAP_CORRUPTION
        return "heap corruption";
    case 0xC0000409:
        return "fast fail / stack buffer overrun (often an abort)";
    case 0xE06D7363:
        return "a C++ exception nobody caught";
    default:
        return "exception";
    }
}

/// The call stack of `thread` from `context` (most recent first).
void writeStack(HANDLE file, CONTEXT context, HANDLE thread) {
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
    SymInitialize(process, nullptr, TRUE);
    STACKFRAME64 frame{};
    DWORD machine = 0;
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset = context.Rip;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrStack.Offset = context.Rsp;
#elif defined(_M_ARM64)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset = context.Pc;
    frame.AddrFrame.Offset = context.Fp;
    frame.AddrStack.Offset = context.Sp;
#else
    writeText(file, "(no call stack on this CPU)\r\n");
    return;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;
    alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 512];
    char line[1024];
    for (int i = 0; i < 80; ++i) {
        if (!StackWalk64(machine, process, thread, &frame, &context, nullptr,
                    SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
                frame.AddrPC.Offset == 0) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        auto* pSymbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
        std::memset(symbolBuffer, 0, sizeof(symbolBuffer));
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = 511;
        DWORD64 offset = 0;
        const bool named = SymFromAddr(process, address, &offset, pSymbol);
        IMAGEHLP_MODULE64 module{};
        module.SizeOfStruct = sizeof(module);
        const bool haveModule = SymGetModuleInfo64(process, address, &module);
        IMAGEHLP_LINE64 source{};
        source.SizeOfStruct = sizeof(source);
        DWORD column = 0;
        const bool haveLine = SymGetLineFromAddr64(process, address, &column, &source);
        std::snprintf(line, sizeof(line), "#%02d %s!%s + 0x%llx", i,
                haveModule ? module.ModuleName : "?", named ? pSymbol->Name : "?",
                static_cast<unsigned long long>(named ? offset : address));
        writeText(file, line);
        if (haveLine) {
            std::snprintf(line, sizeof(line), "   (%s:%lu)", source.FileName, source.LineNumber);
            writeText(file, line);
        }
        writeText(file, "\r\n");
    }
    SymCleanup(process);
}

/// Writes <name>.dmp (every thread, for a debugger) and <name>.txt (what
/// happened, and the call stack of `threadId` from `pContext`).
void writeReport(const char* name, const char* what, const char* stackTitle, CONTEXT* pContext,
        EXCEPTION_POINTERS* pException, DWORD threadId) {
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t path[MAX_PATH + 80];

    swprintf(path, MAX_PATH + 80, L"%ls\\%hs.dmp", s_dir, name);
    HANDLE dump = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION info{};
        info.ThreadId = threadId;
        info.ExceptionPointers = pException;
        info.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
                static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory),
                pException ? &info : nullptr, nullptr, nullptr);
        CloseHandle(dump);
    }

    swprintf(path, MAX_PATH + 80, L"%ls\\%hs.txt", s_dir, name);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char line[512];
        writeText(file, s_header);
        std::snprintf(line, sizeof(line), "Crashed: %04d-%02d-%02d %02d:%02d:%02d (local time), thread %lu%s\r\n",
                now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
                static_cast<unsigned long>(threadId), threadId == s_mainThreadId ? " (the main thread)" : "");
        writeText(file, line);
        writeText(file, "What: ");
        writeText(file, what);
        writeText(file, "\r\n\r\n");
        writeText(file, stackTitle);
        writeText(file, "\r\n");
        if (pContext) {
            HANDLE thread = threadId == GetCurrentThreadId()
                    ? GetCurrentThread()
                    : OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME,
                              FALSE, threadId);
            if (thread) {
                writeStack(file, *pContext, thread);
                if (thread != GetCurrentThread()) {
                    CloseHandle(thread);
                }
            }
        }
        writeText(file, "\r\nThe app's log up to this point is added at the next start "
                        "(mixxx.log.1 then).\r\n");
        CloseHandle(file);
    }
    mixxx::Logging::tryFlushLogFile();
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* pException) {
    if (s_handling.exchange(true)) {
        return EXCEPTION_CONTINUE_SEARCH; // crashed while writing the report: leave it
    }
    const DWORD code = pException->ExceptionRecord->ExceptionCode;
    std::snprintf(s_crashWhat, sizeof(s_crashWhat), "%s, code 0x%08lX at address %p", exceptionName(code),
            static_cast<unsigned long>(code), pException->ExceptionRecord->ExceptionAddress);
    if (s_crashRequest && GetCurrentThreadId() != s_watchdogThreadId) {
        // The watchdog writes it (and waits a minute at most).
        s_pCrashPointers = pException;
        s_crashThreadId = GetCurrentThreadId();
        SetEvent(s_crashRequest);
        WaitForSingleObject(s_crashDone, 60000);
    } else {
        char name[64];
        stamp(name, sizeof(name), "");
        writeReport(name, s_crashWhat, "Call stack of the thread that crashed (most recent first):",
                pException->ContextRecord, pException, GetCurrentThreadId());
    }
    return EXCEPTION_CONTINUE_SEARCH; // then Windows ends the app as usual
}

void reportHere(const char* what) {
    if (s_handling.exchange(true)) {
        return;
    }
    CONTEXT context{};
    RtlCaptureContext(&context);
    char name[64];
    stamp(name, sizeof(name), "");
    writeReport(name, what, "Call stack of the thread that crashed (most recent first):", &context, nullptr,
            GetCurrentThreadId());
}

char s_fatalWhat[1024];

void onFatal(const char* message) {
    std::snprintf(s_fatalWhat, sizeof(s_fatalWhat), "fatal error: %s", message);
    reportHere(s_fatalWhat);
}

void onAbort(int) {
    reportHere("abort: a failed check (ASSERT) or fatal error; the log's last lines say which");
    // Returning lets abort() end the app.
}

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    reportHere("invalid parameter passed to a C runtime function");
    std::abort();
}

void onPureCall() {
    reportHere("pure virtual function call (an object used after it was deleted)");
    std::abort();
}

/// Frozen: the main thread's call stack, and a dump of every thread.
bool writeFreezeReport(char* name, size_t size) {
    stamp(name, size, "-freeze");
    CONTEXT context{};
    context.ContextFlags = CONTEXT_FULL;
    bool haveContext = false;
    HANDLE main = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME, FALSE,
            s_mainThreadId);
    if (main) {
        if (SuspendThread(main) != static_cast<DWORD>(-1)) {
            haveContext = GetThreadContext(main, &context) != 0;
            ResumeThread(main); // it's stuck anyway: read its stack running
        }
        CloseHandle(main);
    }
    writeReport(name,
            "the app stopped responding: its main thread (the screen, the buttons) has been "
            "stuck for 15 seconds or more",
            "What the main thread is stuck in (most recent first):", haveContext ? &context : nullptr, nullptr,
            s_mainThreadId);
    return true;
}

void removeReport(const char* name) {
    wchar_t path[MAX_PATH + 80];
    swprintf(path, MAX_PATH + 80, L"%ls\\%hs.txt", s_dir, name);
    DeleteFileW(path);
    swprintf(path, MAX_PATH + 80, L"%ls\\%hs.dmp", s_dir, name);
    DeleteFileW(path);
}

/// Windows' own record of how the app ended (Application Error / Hang in
/// the event log): the faulting module and exception code, even for crashes
/// nothing inside the app can catch.
QString windowsErrorEvents() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const QString exe = QFileInfo(QString::fromWCharArray(exePath)).fileName();
    const wchar_t* query =
            L"*[System[(Provider[@Name='Application Error'] or Provider[@Name='Application Hang']"
            L" or Provider[@Name='Windows Error Reporting']) and TimeCreated[timediff(@SystemTime) <= "
            L"172800000]]]";
    EVT_HANDLE results = EvtQuery(nullptr, L"Application", query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results) {
        return QString();
    }
    QStringList found;
    EVT_HANDLE events[16];
    DWORD returned = 0;
    while (found.size() < 3 && EvtNext(results, 16, events, 1000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            DWORD used = 0;
            DWORD count = 0;
            EvtRender(nullptr, events[i], EvtRenderEventXml, 0, nullptr, &used, &count);
            std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
            if (used > 0 &&
                    EvtRender(nullptr, events[i], EvtRenderEventXml, used, xml.data(), &used, &count)) {
                const QString text = QString::fromWCharArray(xml.c_str());
                if (found.size() < 3 && text.contains(exe, Qt::CaseInsensitive)) {
                    QString line;
                    static const QRegularExpression kTime(QStringLiteral("SystemTime='([^']*)'"));
                    static const QRegularExpression kProvider(QStringLiteral("Provider Name='([^']*)'"));
                    static const QRegularExpression kData(QStringLiteral("<Data[^>]*>([^<]*)</Data>"));
                    line += kProvider.match(text).captured(1) + QStringLiteral(" at ") +
                            kTime.match(text).captured(1) + QStringLiteral(":");
                    auto it = kData.globalMatch(text);
                    while (it.hasNext()) {
                        const QString value = it.next().captured(1).trimmed();
                        if (!value.isEmpty()) {
                            line += QStringLiteral("\n  ") + value;
                        }
                    }
                    found.append(line);
                }
            }
            EvtClose(events[i]);
        }
    }
    EvtClose(results);
    return found.join(QStringLiteral("\n\n"));
}

#else // POSIX

char s_dir[4096];
pthread_t s_mainThread;
std::atomic<int> s_freezeFd{-1};
std::atomic<bool> s_freezeWritten{false};

void writeText(int fd, const char* text) {
    ssize_t ignored = ::write(fd, text, std::strlen(text));
    (void)ignored;
}

const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV:
        return "SIGSEGV (used memory it shouldn't)";
    case SIGABRT:
        return "SIGABRT (abort: a failed check (ASSERT) or fatal error; the log's last lines say which)";
    case SIGBUS:
        return "SIGBUS (bad memory access)";
    case SIGFPE:
        return "SIGFPE (arithmetic error)";
    case SIGILL:
        return "SIGILL (illegal instruction)";
    default:
        return "signal";
    }
}

void onSignal(int sig, siginfo_t* pInfo, void*) {
    if (!s_handling.exchange(true)) {
        char name[64];
        stamp(name, sizeof(name), ".txt");
        char path[4200];
        std::snprintf(path, sizeof(path), "%s/%s", s_dir, name);
        const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            char line[512];
            writeText(fd, s_header);
            std::snprintf(line, sizeof(line), "Crashed: %s\nWhat: %s, address %p\n\nCall stack of the thread that "
                    "crashed (most recent first):\n", name + 6, signalName(sig), pInfo ? pInfo->si_addr : nullptr);
            writeText(fd, line);
            void* frames[80];
            const int n = backtrace(frames, 80);
            backtrace_symbols_fd(frames, n, fd);
            writeText(fd, "\nThe app's log up to the crash is added at the next start "
                          "(mixxx.log.1 then).\n");
            ::close(fd);
        }
        mixxx::Logging::tryFlushLogFile();
    }
    // The default action (end the app, maybe a core dump).
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void onFatal(const char* message) {
    if (s_handling.exchange(true)) {
        return;
    }
    char name[64];
    stamp(name, sizeof(name), ".txt");
    char path[4200];
    std::snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        writeText(fd, s_header);
        char line[1400];
        std::snprintf(line, sizeof(line), "Crashed: %s\nWhat: fatal error: %s\n\nCall stack of the thread that "
                "crashed (most recent first):\n", name + 6, message);
        writeText(fd, line);
        void* frames[80];
        const int n = backtrace(frames, 80);
        backtrace_symbols_fd(frames, n, fd);
        writeText(fd, "\nThe app's log up to the crash is added at the next start (mixxx.log.1 then).\n");
        ::close(fd);
    }
    mixxx::Logging::tryFlushLogFile();
}

/// On the main thread, when the watchdog asks: where it's stuck.
void onFreezeSignal(int) {
    const int fd = s_freezeFd.load();
    if (fd >= 0) {
        void* frames[80];
        const int n = backtrace(frames, 80);
        backtrace_symbols_fd(frames, n, fd);
    }
    s_freezeWritten = true;
}

bool writeFreezeReport(char* name, size_t size) {
    stamp(name, size, "-freeze");
    char path[4200];
    std::snprintf(path, sizeof(path), "%s/%s.txt", s_dir, name);
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }
    writeText(fd, s_header);
    char line[512];
    std::snprintf(line, sizeof(line), "Crashed: %s\nWhat: the app stopped responding: its main thread (the "
            "screen, the buttons) has been stuck for 15 seconds or more\n\nWhat the main thread is stuck in "
            "(most recent first):\n", name + 6);
    writeText(fd, line);
    s_freezeWritten = false;
    s_freezeFd = fd;
    pthread_kill(s_mainThread, SIGUSR2);
    for (int i = 0; i < 40 && !s_freezeWritten; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    s_freezeFd = -1;
    writeText(fd, "\nThe app's log up to this point is added at the next start (mixxx.log.1 then).\n");
    ::close(fd);
    mixxx::Logging::tryFlushLogFile();
    return true;
}

void removeReport(const char* name) {
    char path[4200];
    std::snprintf(path, sizeof(path), "%s/%s.txt", s_dir, name);
    std::remove(path);
}

#endif

/// Runs all the time on its own thread: notices freezes (and, on Windows,
/// writes crash reports for the crashing thread).
void watchdog() {
#ifdef Q_OS_WIN
    s_watchdogThreadId = GetCurrentThreadId();
#endif
    char freezeName[64] = {0};
    int ticks = 0;
    while (true) {
#ifdef Q_OS_WIN
        if (WaitForSingleObject(s_crashRequest, 1000) == WAIT_OBJECT_0) {
            CONTEXT context = *s_pCrashPointers->ContextRecord;
            char name[64];
            stamp(name, sizeof(name), "");
            writeReport(name, s_crashWhat, "Call stack of the thread that crashed (most recent first):", &context,
                    s_pCrashPointers, s_crashThreadId);
            SetEvent(s_crashDone);
            return;
        }
        // Something loaded later (a driver, a library) may have put its own
        // crash handler in place of ours: ours again.
        if (++ticks % 5 == 0 && !s_handling) {
            SetUnhandledExceptionFilter(onUnhandledException);
        }
#else
        std::this_thread::sleep_for(std::chrono::seconds(1));
        (void)ticks;
#endif
        if (s_handling) {
            continue;
        }
        const long long beat = s_beat.load();
        if (beat == 0) {
            continue; // not started yet
        }
        const long long stuck = nowMs() - beat;
        if (!freezeName[0] && stuck > kFrozenMs) {
            if (!writeFreezeReport(freezeName, sizeof(freezeName))) {
                freezeName[0] = 1; // (don't try again for this freeze)
            }
        } else if (freezeName[0] && stuck < 2000) {
            // It came back: not worth a report after all.
            if (freezeName[0] != 1) {
                removeReport(freezeName);
            }
            freezeName[0] = 0;
        }
    }
}

QString markerPath(const QString& settingsPath) {
    return QDir(crashReportFolder(settingsPath)).filePath(QStringLiteral("running"));
}

/// The last run didn't end normally and left no report: say so, with what
/// Windows recorded about it.
void reportUncleanEnd(const QString& settingsPath, const QString& startedStamp) {
    const QDir dir(crashReportFolder(settingsPath));
    const QStringList reports = dir.entryList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Name);
    if (!reports.isEmpty() && reports.last() > QStringLiteral("crash-") + startedStamp) {
        return; // it has a report
    }
    char name[64];
    stamp(name, sizeof(name), "-ended.txt");
    QFile file(dir.filePath(QString::fromLatin1(name)));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }
    QTextStream out(&file);
    out << QString::fromUtf8(s_header).remove(QLatin1Char('\r'))
        << "Crashed: the run that started " << startedStamp << " (found at the next start)\n"
        << "What: the app ended without closing normally, and without a crash report: it was closed "
           "while frozen (\"not responding\"), or it crashed in a way the app can't catch.\n";
#ifdef Q_OS_WIN
    const QString events = windowsErrorEvents();
    out << "\nWhat Windows recorded (Event Viewer, Application log):\n"
        << (events.isEmpty() ? QStringLiteral("(nothing about this app in the last two days)") : events)
        << "\n";
#endif
}

} // namespace

QString crashReportFolder(const QString& settingsPath) {
    return QDir(settingsPath).filePath(QStringLiteral("crash-reports"));
}

void installCrashHandler(const QString& settingsPath) {
    const QString dir = QDir::toNativeSeparators(crashReportFolder(settingsPath));
    QDir().mkpath(dir);
    const QByteArray header = QStringLiteral(
            "B2B crash report\n"
            "App: B2B %1, Mixxx %2 (%3, %4)\n"
            "OS: %5 (%6)\n"
            "Started: %7\n")
                                      .arg(QString::fromLatin1(::b2b::kVersion),
                                              VersionStore::version(),
                                              VersionStore::gitBranch(),
                                              VersionStore::gitDescribe(),
                                              QSysInfo::prettyProductName(),
                                              QSysInfo::currentCpuArchitecture(),
                                              QDateTime::currentDateTime().toString(Qt::ISODate))
#ifdef Q_OS_WIN
                                      .replace(QLatin1Char('\n'), QStringLiteral("\r\n"))
#endif
                                      .toUtf8();
    std::snprintf(s_header, sizeof(s_header), "%s", header.constData());

    // Did the last run end normally?
    const QString marker = markerPath(settingsPath);
    {
        QFile last(marker);
        if (last.open(QIODevice::ReadOnly)) {
            const QString started = QString::fromLatin1(last.readAll()).trimmed();
            last.close();
            if (!started.isEmpty()) {
                reportUncleanEnd(settingsPath, started);
            }
        }
        QFile now(marker);
        if (now.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            now.write(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")).toLatin1());
        }
    }
    std::snprintf(s_marker, sizeof(s_marker), "%s", QFile::encodeName(marker).constData());
    mixxx::g_onFatal = onFatal;
    std::atexit(removeMarker);

#ifdef Q_OS_WIN
    const std::wstring wdir = dir.toStdWString();
    wcsncpy_s(s_dir, MAX_PATH, wdir.c_str(), _TRUNCATE);
    s_mainThreadId = GetCurrentThreadId();
    s_crashRequest = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s_crashDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetUnhandledExceptionFilter(onUnhandledException);
    // abort() (failed asserts, qFatal, std::terminate) goes to our handler
    // instead of ending the app without a word.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, onAbort);
    _set_invalid_parameter_handler(onInvalidParameter);
    _set_purecall_handler(onPureCall);
    // Keep room on the stack to report a stack overflow.
    ULONG reserve = 64 * 1024;
    SetThreadStackGuarantee(&reserve);
#else
    std::snprintf(s_dir, sizeof(s_dir), "%s", dir.toLocal8Bit().constData());
    s_mainThread = pthread_self();
    // Somewhere to run on a stack overflow.
    static char s_altStack[256 * 1024];
    stack_t ss{};
    ss.ss_sp = s_altStack;
    ss.ss_size = sizeof(s_altStack);
    sigaltstack(&ss, nullptr);
    struct sigaction action {};
    action.sa_sigaction = onSignal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
        sigaction(sig, &action, nullptr);
    }
    struct sigaction freeze {};
    freeze.sa_handler = onFreezeSignal;
    freeze.sa_flags = SA_RESTART;
    sigemptyset(&freeze.sa_mask);
    sigaction(SIGUSR2, &freeze, nullptr);
    // backtrace() loads libgcc the first time: not in a crash.
    void* frames[2];
    backtrace(frames, 2);
#endif

    // The main thread's heartbeat, and the watchdog that listens for it.
    if (QCoreApplication::instance()) {
        auto* pBeat = new QTimer(QCoreApplication::instance());
        pBeat->setInterval(500);
        QObject::connect(pBeat, &QTimer::timeout, [] {
            s_beat = nowMs();
        });
        pBeat->start();
        if (qEnvironmentVariableIsSet("B2B_FREEZE_TEST")) { // testing the freeze report
            QTimer::singleShot(20000, [] {
                std::this_thread::sleep_for(std::chrono::seconds(qEnvironmentVariableIntValue("B2B_FREEZE_TEST")));
            });
        }
    }
    std::thread(watchdog).detach();
    qInfo() << "B2B: crash reports go to" << dir;
}

QStringList unseenCrashReports(const QString& settingsPath) {
    QSettings ini(QDir(settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
    const QString seen = ini.value(QStringLiteral("crash_seen")).toString();
    QStringList out;
    const QDir dir(crashReportFolder(settingsPath));
    const QStringList names = dir.entryList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Name);
    for (const QString& name : names) {
        if (name > seen) {
            out.append(dir.filePath(name));
        }
    }
    return out;
}

void markCrashReportsSeen(const QString& settingsPath) {
    const QDir dir(crashReportFolder(settingsPath));
    const QStringList names = dir.entryList({QStringLiteral("crash-*.txt")}, QDir::Files, QDir::Name);
    if (!names.isEmpty()) {
        QSettings ini(QDir(settingsPath).filePath(QStringLiteral("b2b.ini")), QSettings::IniFormat);
        ini.setValue(QStringLiteral("crash_seen"), names.last());
    }
}

} // namespace mixxx::b2b

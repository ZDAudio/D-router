#include "Diagnostics/CrashHandler.h"
#include "Diagnostics/Logger.h"

#include <juce_core/juce_core.h>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <atomic>
#include <cstring>

#if JUCE_MAC
#import <Foundation/Foundation.h>
#endif

namespace dcr
{

namespace
{
// Re-entry guard -- if our handler itself crashes, just hand back to
// the default to avoid infinite recursion.
std::atomic<int> reentry{0};

// The log file descriptor, opened ONCE on the message thread at install()
// time.  The signal handler must never call open()/Logger/JUCE (none are
// async-signal-safe); it only write(2)s + backtrace_symbols_fd's to this fd.
std::atomic<int> gLogFd{-1};

void writeAsyncSafe(int fd, const char* s, size_t n)
{
    // write() is async-signal-safe; we ignore the return value because
    // we have no meaningful recovery in a signal handler.
    ssize_t r = ::write(fd, s, n);
    (void)r;
}
void writeAsyncSafe(int fd, const char* s)
{
    writeAsyncSafe(fd, s, std::strlen(s));
}

// Append a backtrace + signal name to whatever log file the Logger is
// currently writing.  Strict async-signal-safe path -- no allocation,
// no JUCE String, no mutex.
void writeBacktraceToLog(const char* sigName)
{
    if (reentry.fetch_add(1, std::memory_order_acq_rel) != 0)
        return;

    // Strict async-signal-safe path: the fd was opened at install() time, so
    // here we only write(2) + backtrace_symbols_fd (both async-signal-safe).
    // No Logger/JUCE/File, no open(), no allocation.
    const int fd = gLogFd.load(std::memory_order_acquire);
    if (fd < 0)
        return;

    writeAsyncSafe(fd, "\n");
    writeAsyncSafe(fd, "================================================================\n");
    writeAsyncSafe(fd, "=== HARD CRASH ");
    writeAsyncSafe(fd, sigName);
    writeAsyncSafe(fd, "\n");
    writeAsyncSafe(fd, "=== Backtrace:\n");

    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, fd);

    writeAsyncSafe(fd, "================================================================\n");
    ::fsync(fd);
    // Do NOT close: we're about to restore SIG_DFL and re-raise, so the process
    // is dying anyway and the fd needs no teardown.
}

void signalHandler(int sig)
{
    const char* name = "UNKNOWN";
    switch (sig)
    {
        case SIGSEGV:
            name = "SIGSEGV (bad memory access)";
            break;
        case SIGBUS:
            name = "SIGBUS (bus error)";
            break;
        case SIGABRT:
            name = "SIGABRT (abort / assertion)";
            break;
        case SIGILL:
            name = "SIGILL (illegal instruction)";
            break;
        case SIGFPE:
            name = "SIGFPE (arithmetic fault)";
            break;
        case SIGTERM:
            name = "SIGTERM (kill / Activity Monitor Quit)";
            break;
        default:
            break;
    }
    writeBacktraceToLog(name);

    // Restore default handler and re-raise so the OS still gets to see
    // the crash (CrashReporter, core dump, parent process notification).
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

#if JUCE_MAC
void uncaughtNSExceptionHandler(NSException* ex)
{
    // NSException uncaught-handler runs BEFORE abort() -- safe to use
    // higher-level APIs here, not just async-signal-safe ones.
    // Reuse the install()-time fd for consistency with the signal path.
    const int fd = gLogFd.load(std::memory_order_acquire);
    if (fd < 0)
        return;

    writeAsyncSafe(fd, "\n================================================================\n");
    writeAsyncSafe(fd, "=== UNCAUGHT NSException\n");
    writeAsyncSafe(fd, "=== Name:   ");
    writeAsyncSafe(fd, [[ex name] UTF8String]);
    writeAsyncSafe(fd, "\n=== Reason: ");
    writeAsyncSafe(fd, [[ex reason] UTF8String]);
    writeAsyncSafe(fd, "\n=== Call stack:\n");
    for (NSString* line in [ex callStackSymbols])
    {
        writeAsyncSafe(fd, [line UTF8String]);
        writeAsyncSafe(fd, "\n");
    }
    writeAsyncSafe(fd, "================================================================\n");
    ::fsync(fd);
    // fd is the shared pre-opened log handle -- leave it open.
}
#endif
} // namespace

void CrashHandler::install()
{
    // SA_NODEFER lets us re-raise the same signal at the end of our handler
    // -- without it, the second raise would be silently dropped.
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER | SA_RESETHAND;

    ::sigaction(SIGSEGV, &sa, nullptr);
    ::sigaction(SIGBUS, &sa, nullptr);
    ::sigaction(SIGABRT, &sa, nullptr);
    ::sigaction(SIGILL, &sa, nullptr);
    ::sigaction(SIGFPE, &sa, nullptr);
    // SIGTERM = `kill <pid>` and Activity Monitor's "Quit" (not Force
    // Quit -- that's SIGKILL which can't be caught).  We log + re-raise
    // so a graceful shutdown by external request still leaves a trail.
    ::sigaction(SIGTERM, &sa, nullptr);

    // Pre-open the log fd on the message thread so the signal handler never has
    // to.  Logger::init() runs before install() (see header), so the file exists.
    {
        const auto path = Logger::getCurrentLogFile().getFullPathName();
        if (path.isNotEmpty())
        {
            const int fd = ::open(path.toRawUTF8(), O_WRONLY | O_APPEND | O_CREAT, 0644);
            gLogFd.store(fd, std::memory_order_release);
        }
    }

#if JUCE_MAC
    NSSetUncaughtExceptionHandler(uncaughtNSExceptionHandler);
#endif

    DBG("dcr::CrashHandler installed (SIGSEGV/SIGBUS/SIGABRT/SIGILL/SIGFPE/SIGTERM + NSException)");
}

} // namespace dcr

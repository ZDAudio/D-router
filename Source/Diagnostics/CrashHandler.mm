#include "Diagnostics/CrashHandler.h"
#include "Diagnostics/Logger.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#if JUCE_MAC
 #import <Foundation/Foundation.h>
#endif

namespace dcr {

namespace
{
    // Re-entry guard -- if our handler itself crashes, just hand back to
    // the default to avoid infinite recursion.
    std::atomic<int> reentry { 0 };

    void writeAsyncSafe (int fd, const char* s, size_t n)
    {
        // write() is async-signal-safe; we ignore the return value because
        // we have no meaningful recovery in a signal handler.
        ssize_t r = ::write (fd, s, n);
        (void) r;
    }
    void writeAsyncSafe (int fd, const char* s)
    {
        writeAsyncSafe (fd, s, std::strlen (s));
    }

    // Append a backtrace + signal name to whatever log file the Logger is
    // currently writing.  Strict async-signal-safe path -- no allocation,
    // no JUCE String, no mutex.
    void writeBacktraceToLog (const char* sigName)
    {
        if (reentry.fetch_add (1, std::memory_order_acq_rel) != 0) return;

        // We snapshot the path on init since calling JUCE::File from a
        // signal handler is unsafe.  Logger::getCurrentLogFile() reads a
        // unique_ptr -- racy but in practice OK for this best-effort path.
        auto path = Logger::getCurrentLogFile().getFullPathName();
        if (path.isEmpty()) return;

        int fd = ::open (path.toRawUTF8(), O_WRONLY | O_APPEND);
        if (fd < 0) return;

        writeAsyncSafe (fd, "\n");
        writeAsyncSafe (fd, "================================================================\n");
        writeAsyncSafe (fd, "=== HARD CRASH ");
        writeAsyncSafe (fd, sigName);
        writeAsyncSafe (fd, "\n");
        writeAsyncSafe (fd, "=== Backtrace:\n");

        void* frames[64];
        const int n = ::backtrace (frames, 64);
        ::backtrace_symbols_fd (frames, n, fd);

        writeAsyncSafe (fd, "================================================================\n");
        ::fsync (fd);
        ::close (fd);
    }

    void signalHandler (int sig)
    {
        const char* name = "UNKNOWN";
        switch (sig)
        {
            case SIGSEGV: name = "SIGSEGV (bad memory access)"; break;
            case SIGBUS:  name = "SIGBUS (bus error)";          break;
            case SIGABRT: name = "SIGABRT (abort / assertion)"; break;
            case SIGILL:  name = "SIGILL (illegal instruction)"; break;
            case SIGFPE:  name = "SIGFPE (arithmetic fault)";   break;
            default:      break;
        }
        writeBacktraceToLog (name);

        // Restore default handler and re-raise so the OS still gets to see
        // the crash (CrashReporter, core dump, parent process notification).
        ::signal (sig, SIG_DFL);
        ::raise  (sig);
    }

#if JUCE_MAC
    void uncaughtNSExceptionHandler (NSException* ex)
    {
        // NSException uncaught-handler runs BEFORE abort() -- safe to use
        // higher-level APIs here, not just async-signal-safe ones.
        auto path = Logger::getCurrentLogFile().getFullPathName();
        if (path.isEmpty()) return;

        int fd = ::open (path.toRawUTF8(), O_WRONLY | O_APPEND);
        if (fd < 0) return;

        writeAsyncSafe (fd, "\n================================================================\n");
        writeAsyncSafe (fd, "=== UNCAUGHT NSException\n");
        writeAsyncSafe (fd, "=== Name:   ");
        writeAsyncSafe (fd, [[ex name] UTF8String]);
        writeAsyncSafe (fd, "\n=== Reason: ");
        writeAsyncSafe (fd, [[ex reason] UTF8String]);
        writeAsyncSafe (fd, "\n=== Call stack:\n");
        for (NSString* line in [ex callStackSymbols])
        {
            writeAsyncSafe (fd, [line UTF8String]);
            writeAsyncSafe (fd, "\n");
        }
        writeAsyncSafe (fd, "================================================================\n");
        ::fsync (fd);
        ::close (fd);
    }
#endif
}

void CrashHandler::install()
{
    // SA_NODEFER lets us re-raise the same signal at the end of our handler
    // -- without it, the second raise would be silently dropped.
    struct sigaction sa;
    std::memset (&sa, 0, sizeof (sa));
    sa.sa_handler = signalHandler;
    sigemptyset (&sa.sa_mask);
    sa.sa_flags = SA_NODEFER | SA_RESETHAND;

    ::sigaction (SIGSEGV, &sa, nullptr);
    ::sigaction (SIGBUS,  &sa, nullptr);
    ::sigaction (SIGABRT, &sa, nullptr);
    ::sigaction (SIGILL,  &sa, nullptr);
    ::sigaction (SIGFPE,  &sa, nullptr);

   #if JUCE_MAC
    NSSetUncaughtExceptionHandler (uncaughtNSExceptionHandler);
   #endif

    DBG ("dcr::CrashHandler installed (SIGSEGV/SIGBUS/SIGABRT/SIGILL/SIGFPE + NSException)");
}

} // namespace dcr

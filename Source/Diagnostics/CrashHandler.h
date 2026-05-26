#pragma once

namespace dcr {

// Installs signal handlers (SIGSEGV / SIGBUS / SIGABRT / SIGILL / SIGFPE)
// and NSException uncaught-handler that append a backtrace to the current
// session log before the process dies.  Without this, plugin-induced hard
// crashes leave no on-disk evidence (macOS CrashReporter sometimes also
// drops nothing for sandbox-related plugin terminations).
//
// Call after dcr::Logger::init().
class CrashHandler
{
public:
    static void install();
};

} // namespace dcr

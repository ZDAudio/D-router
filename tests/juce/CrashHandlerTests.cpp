#include "Diagnostics/CrashHandler.h"
#include "Diagnostics/Logger.h"

#include <juce_core/juce_core.h>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

// The crash handler runs in a signal handler, so we exercise it the only honest
// way: fork a child, raise a fatal signal in it, and confirm from the parent
// that (a) the child died from the re-raised signal (didn't hang) and (b) the
// pre-opened log fd received a backtrace.
struct CrashHandlerTests : juce::UnitTest
{
    CrashHandlerTests() : juce::UnitTest ("CrashHandler") {}

    void runTest() override
    {
        beginTest ("signal path writes a backtrace to the pre-opened log fd without hanging");

        dcr::Logger::init();
        dcr::CrashHandler::install();

        const juce::File logFile = dcr::Logger::getCurrentLogFile();
        expect (logFile.existsAsFile());

        const pid_t pid = ::fork();
        if (pid == 0)
        {
            // Child: trip the handler. SA_RESETHAND + re-raise terminates us.
            ::raise (SIGABRT);
            ::_exit (0); // unreachable if the handler re-raises
        }

        expect (pid > 0);
        int status = 0;
        ::waitpid (pid, &status, 0);
        expect (WIFSIGNALED (status)); // died from the signal, i.e. did not hang

        const juce::String contents = logFile.loadFileAsString();
        expect (contents.contains ("HARD CRASH"));
        expect (contents.contains ("SIGABRT"));

        // Don't leave this process's signal dispositions pointing at our handler
        // for the remaining tests.
        ::signal (SIGABRT, SIG_DFL);
        dcr::Logger::shutdown();
    }
};

static CrashHandlerTests crashHandlerTests;

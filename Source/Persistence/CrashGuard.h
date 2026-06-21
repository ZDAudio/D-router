#pragma once

#include <juce_core/juce_core.h>

namespace dcr
{

    // A one-bit "did the previous run exit cleanly?" indicator backed by a file.
    //
    // Lifecycle:
    //   - On startup, call wasUnclean() FIRST -- it returns true iff the marker
    //     left by the previous launch is still on disk (i.e. that launch never
    //     called markCleanExit() before terminating).
    //   - Then call markRunning() immediately so this launch is tracked too.
    //   - On graceful shutdown (after the auto-save has succeeded), call
    //     markCleanExit().  If we crash before that, the marker survives and
    //     the next launch sees wasUnclean() == true.
    class CrashGuard
    {
    public:
        static bool wasUnclean();
        static void markRunning();
        static void markCleanExit();

    private:
        static juce::File markerFile();
    };

} // namespace dcr

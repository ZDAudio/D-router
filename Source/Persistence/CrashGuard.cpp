#include "Persistence/CrashGuard.h"

namespace dcr
{

    juce::File CrashGuard::markerFile()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("dcorerouter");
        dir.createDirectory();
        return dir.getChildFile ("running.lock");
    }

    bool CrashGuard::wasUnclean()
    {
        return markerFile().existsAsFile();
    }

    void CrashGuard::markRunning()
    {
        // Re-create on every launch so even a brand-new install lands in a
        // known state.  The body is a timestamp purely for human debugging.
        markerFile().replaceWithText (juce::Time::getCurrentTime().toISO8601 (true));
    }

    void CrashGuard::markCleanExit()
    {
        markerFile().deleteFile();
    }

} // namespace dcr

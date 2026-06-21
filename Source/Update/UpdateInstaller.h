#pragma once

#include "Update/UpdateChecker.h"   // ReleaseInfo

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>

namespace dcr::update
{

// Downloads a release's D-Router.zip (background, with progress), verifies and
// unpacks it, then launches a detached shell script that waits for THIS process
// to quit, replaces the running .app in place, and relaunches.  All UI callbacks
// fire on the message thread.  macOS-only (the app is macOS).
class UpdateInstaller : private juce::Thread
{
public:
    UpdateInstaller() : juce::Thread ("dcr-update-download") {}
    ~UpdateInstaller() override { stopThread (8000); }

    using Progress = std::function<void (double fraction01)>;
    using Done     = std::function<void (bool ok, juce::String error)>;   // error empty on cancel

    // Pre-flight: can we replace the running bundle in place?  false + a
    // user-facing reason when the app is App-Translocated or its folder isn't
    // writable -- caller should then offer the download page instead.
    static bool canInstallInPlace (juce::String& reason);

    // The releases page, for the "open download page" fallback.
    static juce::URL releasesPageUrl();

    void start (ReleaseInfo info, Progress onProgress, Done onDone)
    {
        stopThread (8000);
        release   = std::move (info);
        progress  = std::move (onProgress);
        done      = std::move (onDone);
        cancelled = false;
        startThread();
    }

    void cancel()
    {
        cancelled = true;
        signalThreadShouldExit();
    }

private:
    void run() override;

    static juce::File appBundle();      // the running .app
    static juce::File cacheDir();       // ~/Library/Caches/D-Router/update

    ReleaseInfo       release;
    Progress          progress;
    Done              done;
    std::atomic<bool> cancelled { false };
};

} // namespace dcr::update

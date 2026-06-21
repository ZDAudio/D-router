#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <memory>

namespace dcr::update
{

    // A release on GitHub that carries a downloadable D-Router.zip asset.
    struct ReleaseInfo
    {
        juce::String tag; // e.g. "v0.2.0-beta"
        juce::String name; // release title (falls back to the tag)
        juce::String notes; // release body (markdown)
        juce::URL zipUrl; // browser_download_url of the D-Router.zip asset
        juce::int64 zipSize = 0; // asset size in bytes (0 if unknown)
    };

    // Checks the public ZDAudio/D-router repo for the newest release (INCLUDING
    // pre-releases) that carries a D-Router.zip asset.  Runs on a background thread
    // so the message thread is never blocked; the callback is delivered back on the
    // message thread.  Nothing here touches the audio/matrix thread.
    class UpdateChecker : private juce::Thread
    {
    public:
        UpdateChecker() : juce::Thread ("dcr-update-check") {}
        ~UpdateChecker() override { stopThread (4000); }

        // info  : a newer release, or nullptr if up-to-date / no usable release / failed.
        // ok    : false only on a hard fetch/parse failure (lets the manual check say
        //         "couldn't reach GitHub" vs. "you're up to date").
        using Callback = std::function<void (std::unique_ptr<ReleaseInfo> info, bool ok)>;

        void check (juce::String currentVersion, Callback cb)
        {
            stopThread (4000);
            current = std::move (currentVersion);
            callback = std::move (cb);
            startThread();
        }

    private:
        void run() override;

        juce::String current;
        Callback callback;
    };

} // namespace dcr::update

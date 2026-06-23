#pragma once

#include <CoreAudio/CoreAudio.h>

#include <functional>
#include <string>
#include <vector>

namespace dcr
{
    // Message-thread-only registry of tappable audio processes (macOS 14.4+).
    // Enumerates processes that can be captured, resolves a bundle id to the
    // current CoreAudio process-object id, and notifies on launch/quit so the
    // engine can auto-reattach.  No realtime involvement.
    class AppAudioProcesses
    {
    public:
        struct Entry
        {
            AudioObjectID processObject = kAudioObjectUnknown;
            int pid = -1;
            std::string bundleId;
            std::string displayName;
            bool runningOutput = false; // currently producing output audio
        };

        AppAudioProcesses();
        ~AppAudioProcesses();

        // Snapshot of processes right now (message thread).
        std::vector<Entry> enumerate() const;

        // bundle id -> current process-object id, or kAudioObjectUnknown.
        AudioObjectID resolve (const std::string& bundleId) const;

        // Called on the message thread whenever the running-app set changes.
        std::function<void()> onProcessesChanged;

    private:
        void* observer = nullptr; // NSWorkspace notification tokens (opaque here)
    };
} // namespace dcr

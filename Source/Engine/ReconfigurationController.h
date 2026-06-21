#pragma once

#include "Persistence/SnapshotStore.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <cstdint>
#include <vector>

namespace dcr
{

    // Explicit lifecycle for a device reconfigure / snapshot apply (Phase C3).
    //
    // Replaces MainComponent's bare `std::atomic<bool> isReconfiguring` with a
    // named, ordered phase machine: there is one owner of "are we
    // reconfiguring", the stages are nameable, and an illegal/out-of-order
    // transition is rejected (and assertable) instead of silently corrupting
    // state.  The actual engine + worker-thread work stays in the caller; it
    // drives this machine through the phases at the sync points it already
    // serialises.
    //
    // Thread model (unchanged from the bool it replaces): `tryBegin`, `active`
    // and `phase` are atomic and may be read from any thread -- a CoreAudio
    // device-hotplug callback checks `active()` before kicking a reconfigure.
    // The phase is advanced on the message thread (claim, restore tail) and the
    // worker thread (drain, rebuild); those points are already ordered by the
    // existing callAsync hand-off, so there is a single writer at a time.
    class ReconfigurationController
    {
    public:
        // The forward-only stages of one reconfigure.  RestoringPlugins marks
        // that async plugin re-instantiation has been kicked off; the loads
        // themselves continue past `finish()` (they are aliveToken-guarded).
        enum class Phase {
            Idle,
            Draining, // fade out + stopProcessor (worker)
            Rebuilding, // engine.stop()/start() rebuilds devices (worker)
            RestoringMatrix, // restore gains/mutes/solo/crosspoints (message)
            RestoringPlugins, // kick async plugin restore (message)
            Running // reconfigure tail complete
        };

        // Claim the machine: Idle -> Draining.  Returns true on success; false
        // if a reconfigure is already in flight (mirrors the old
        // `isReconfiguring.exchange(true)` re-entry guard -- caller backs off).
        bool tryBegin() noexcept;

        // Move to `next` iff it is the immediate forward successor of the
        // current phase.  Returns false (and leaves the phase unchanged) on any
        // out-of-order request, so callers/tests can detect a broken sequence.
        bool advance (Phase next) noexcept;

        // End the reconfigure (success or abort): any phase -> Idle.
        void finish() noexcept;

        Phase phase() const noexcept { return phase_.load (std::memory_order_acquire); }
        bool active() const noexcept { return phase() != Phase::Idle; }

        // ---- Reconfigure payload (single owner) ------------------------------
        // These were previously loose in MainComponent; the controller now owns
        // them so the whole reconfigure state lives in one place.  They are
        // touched ONLY on the message thread (the worker harvests into a local
        // and hands it over via callAsync), so no synchronisation is needed
        // beyond the discipline the existing code already keeps.

        // The matrix/plugin state restored AFTER engine.start() resizes the
        // matrix.  Bridges applyDeviceSelection's worker -> message handoff.
        struct PendingSnapshotApply
        {
            std::vector<float> inputTrim;
            std::vector<float> outputTrim;
            std::vector<unsigned char> inputMute;
            std::vector<unsigned char> outputMute;
            std::vector<unsigned char> inputSolo;
            std::vector<Snapshot::Crosspoint> crosspoints;
            std::vector<Snapshot::ChannelChain> channelChains;
            std::vector<Snapshot::GroupChain> groupChains;
            bool valid = false;
        };

        // One queued plugin re-instantiation.  AU createPluginInstanceAsync is
        // async in name only on macOS (must run on the message thread), so the
        // queue is drained strictly one-at-a-time with a callAsync yield between
        // each to keep the UI responsive.
        struct PendingPluginLoad
        {
            juce::PluginDescription desc;
            juce::MemoryBlock state;
            bool bypassed = false;
            int slotIdx = 0;

            enum class Kind {
                ChannelSlot,
                GroupSlot
            } kind = Kind::ChannelSlot;

            // For ChannelSlot.
            bool isInputChannel = false;
            int globalChannelIdx = -1;

            // For GroupSlot.
            bool isInputGroup = false;
            int groupIdx = -1;
            juce::AudioChannelSet channelSet { juce::AudioChannelSet::stereo() };
        };

        PendingSnapshotApply& snapshot() noexcept { return pendingSnapshot_; }
        std::vector<PendingPluginLoad>& pluginQueue() noexcept { return pluginLoadQueue_; }
        int& pluginCursor() noexcept { return pluginLoadCursor_; }
        uint32_t& pluginStartMs() noexcept { return pluginLoadStartMs_; }

    private:
        static Phase successorOf (Phase p) noexcept;

        std::atomic<Phase> phase_ { Phase::Idle };

        PendingSnapshotApply pendingSnapshot_;
        std::vector<PendingPluginLoad> pluginLoadQueue_;
        int pluginLoadCursor_ = 0;
        uint32_t pluginLoadStartMs_ = 0;
    };

}

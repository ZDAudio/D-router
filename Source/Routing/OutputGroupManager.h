#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <vector>

#include "Routing/OutputGroup.h"

namespace dcr
{

    class RoutingMatrix;

    // Owns the list of OutputGroups and the channel -> group lookup table.
    // All structural mutation (add/remove/assign) is serialised by a SpinLock;
    // the audio thread try-locks briefly to read the lookup.  Fader/mute atomics
    // are read directly without the lock.
    class OutputGroupManager
    {
    public:
        // Set after engine restart with the current number of global output
        // channels.  Clears the channel->group lookup; existing groups stay
        // but slots referencing now-out-of-range channels become invalid.
        void setNumOutputChannels (int n);

        int getNumOutputChannels() const noexcept { return numOutputs; }
        int getNumGroups() const noexcept { return (int) groups.size(); }

        // UI-thread group management.
        int createGroup (juce::String name, juce::AudioChannelSet cs);
        void removeGroup (int groupIdx);
        OutputGroup* getGroup (int groupIdx) noexcept;
        const OutputGroup* getGroup (int groupIdx) const noexcept;
        int getGroupIndexForChannel (int globalOutputCh) const noexcept;
        OutputGroup* getGroupForChannel (int globalOutputCh) noexcept;

        // Assign a global output channel to a slot in a group.  Auto-removes the
        // channel from any prior group.  Pass globalOutputCh = -1 to clear the
        // slot.
        void assignChannel (int groupIdx, int slotIdx, int globalOutputCh);

        // ===== Fader behaviour =====
        // Move the group fader to `newFaderDb`.
        //   VCA:    delta vs the stored value is applied to each member's outputTrim.
        //   Router: stores the overlay position and refreshes channelRouterGain[].
        void moveGroupFader (int groupIdx, float newFaderDb, RoutingMatrix& matrix);

        // Set group mute state.
        //   VCA:    propagated onto each member channel's mute.
        //   Router: folded into channelRouterGain[] (0 when muted); member matrix
        //           mutes are left untouched.
        void setGroupMute (int groupIdx, bool muted, RoutingMatrix& matrix);

        // Switch a group between VCA and Router.  Preserves the audible level and
        // resets the group fader to 0 dB (Router->VCA bakes the overlay into member
        // trims; the mute mechanism is re-asserted through the new mode).
        void setGroupFaderMode (int groupIdx, OutputGroup::FaderMode mode, RoutingMatrix& matrix);

        // Audio thread: per-channel Router overlay gain (1.0 == no contribution).
        // Lock-free atomic read; folded into the post-fader stage by MatrixProcessor.
        float getChannelRouterGain (int ch) const noexcept
        {
            return (ch >= 0 && ch < (int) channelRouterGain.size())
                       ? channelRouterGain[(size_t) ch].load (std::memory_order_relaxed)
                       : 1.0f;
        }

        // Audio thread: iterate groups for gather/process/scatter.  Reader does
        // not need the lock for fader/mute (atomic) but holds the lock to read
        // the member list - try-lock; on failure the block falls through without
        // group processing for one cycle.
        template <typename Fn>
        void forEachGroupForAudio (Fn&& fn) const
        {
            const juce::SpinLock::ScopedTryLockType lk (lock);
            if (!lk.isLocked())
                return;
            for (auto& g : groups)
                if (g != nullptr)
                    fn (*g);
        }

        // Inspection helpers (UI thread).
        juce::SpinLock& getLock() noexcept { return lock; }

        // Add each group's total insert-chain latency (sum over its plugin slots of
        // the reported plugin latency, in samples) onto every member channel's entry
        // in `perChannelLatency` (sized to the global output-channel count).
        // Message/UI thread, lock-free: it only reads structure the message thread
        // alone mutates (like getGroup()), so taking the manager lock here would just
        // risk starving the audio thread's try-lock in forEachGroupForAudio.  Feeds
        // the latency report / PDC planner so a group insert's latency is attributed
        // to its members.
        void addGroupInsertLatencySamples (std::vector<int>& perChannelLatency) const;

    private:
        void rebuildChannelLookup(); // call under lock
        void recomputeRouterGains(); // call under lock; rebuilds channelRouterGain from Router groups

        mutable juce::SpinLock lock;
        std::vector<std::unique_ptr<OutputGroup>> groups;
        std::vector<int> channelToGroupIdx; // size = numOutputs
        std::vector<std::atomic<float>> channelRouterGain; // size = numOutputs; 1.0 == no overlay
        int numOutputs = 0;
    };

} // namespace dcr

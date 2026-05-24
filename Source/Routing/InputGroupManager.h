#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <memory>
#include <vector>

#include "Routing/OutputGroup.h"   // reuses the OutputGroup struct (same shape)

namespace dcr {

class RoutingMatrix;

// Mirror of OutputGroupManager for INPUT-side groups.  Member channels are
// global INPUT channel indices.  moveGroupFader / setGroupMute touch the
// matrix's INPUT trim / mute fields.  Multi-channel plugin chains run on the
// gathered input buffer BEFORE the matrix mix.
class InputGroupManager
{
public:
    void setNumInputChannels (int n);

    int  getNumInputChannels() const noexcept { return numInputs; }
    int  getNumGroups()        const noexcept { return (int) groups.size(); }

    int          createGroup (juce::String name, juce::AudioChannelSet cs);
    void         removeGroup (int groupIdx);
    OutputGroup*       getGroup (int groupIdx)       noexcept;
    const OutputGroup* getGroup (int groupIdx) const noexcept;
    int          getGroupIndexForChannel (int globalInputCh) const noexcept;
    OutputGroup* getGroupForChannel       (int globalInputCh) noexcept;

    void assignChannel (int groupIdx, int slotIdx, int globalInputCh);

    void moveGroupFader (int groupIdx, float newFaderDb, RoutingMatrix& matrix);
    void setGroupMute   (int groupIdx, bool muted,       RoutingMatrix& matrix);

    template <typename Fn>
    void forEachGroupForAudio (Fn&& fn) const
    {
        const juce::SpinLock::ScopedTryLockType lk (lock);
        if (! lk.isLocked()) return;
        for (auto& g : groups) if (g != nullptr) fn (*g);
    }

    juce::SpinLock& getLock() noexcept { return lock; }

private:
    void rebuildChannelLookup();

    mutable juce::SpinLock                       lock;
    std::vector<std::unique_ptr<OutputGroup>>    groups;
    std::vector<int>                             channelToGroupIdx;
    int                                          numInputs = 0;
};

} // namespace dcr

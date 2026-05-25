#include "Routing/OutputGroupManager.h"

#include "Routing/RoutingMatrix.h"

#include <cmath>

namespace dcr {

namespace
{
    float dbToLin (float db) noexcept
    {
        if (db <= -60.0f) return 0.0f;
        return std::pow (10.0f, db * 0.05f);
    }
    float linToDb (float lin) noexcept
    {
        if (lin <= 1.0e-6f) return -60.0f;
        return 20.0f * std::log10 (lin);
    }
}

void OutputGroupManager::setNumOutputChannels (int n)
{
    juce::SpinLock::ScopedLockType lk (lock);
    numOutputs = juce::jmax (0, n);
    rebuildChannelLookup();
}

void OutputGroupManager::rebuildChannelLookup()
{
    channelToGroupIdx.assign ((size_t) numOutputs, -1);
    for (int gi = 0; gi < (int) groups.size(); ++gi)
    {
        if (! groups[(size_t) gi]) continue;
        for (int ch : groups[(size_t) gi]->memberChannels)
            if (ch >= 0 && ch < numOutputs)
                channelToGroupIdx[(size_t) ch] = gi;
    }
}

int OutputGroupManager::createGroup (juce::String name, juce::AudioChannelSet cs)
{
    juce::SpinLock::ScopedLockType lk (lock);
    auto g = std::make_unique<OutputGroup>();
    g->name        = std::move (name);
    g->channelSet  = cs;
    g->memberChannels.assign ((size_t) cs.size(), -1);
    groups.push_back (std::move (g));
    return (int) groups.size() - 1;
}

void OutputGroupManager::removeGroup (int groupIdx)
{
    juce::SpinLock::ScopedLockType lk (lock);
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    groups.erase (groups.begin() + groupIdx);
    rebuildChannelLookup();
}

OutputGroup* OutputGroupManager::getGroup (int groupIdx) noexcept
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return nullptr;
    return groups[(size_t) groupIdx].get();
}

const OutputGroup* OutputGroupManager::getGroup (int groupIdx) const noexcept
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return nullptr;
    return groups[(size_t) groupIdx].get();
}

int OutputGroupManager::getGroupIndexForChannel (int ch) const noexcept
{
    if (ch < 0 || ch >= (int) channelToGroupIdx.size()) return -1;
    return channelToGroupIdx[(size_t) ch];
}

OutputGroup* OutputGroupManager::getGroupForChannel (int ch) noexcept
{
    const int gi = getGroupIndexForChannel (ch);
    return gi < 0 ? nullptr : getGroup (gi);
}

void OutputGroupManager::assignChannel (int groupIdx, int slotIdx, int globalOutputCh)
{
    juce::SpinLock::ScopedLockType lk (lock);
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];
    if (slotIdx < 0 || slotIdx >= (int) g.memberChannels.size()) return;

    // If channel is in a (possibly different) group already, clear its
    // previous slot first.
    if (globalOutputCh >= 0 && globalOutputCh < numOutputs)
    {
        const int prevGroup = channelToGroupIdx[(size_t) globalOutputCh];
        if (prevGroup >= 0)
        {
            auto& prev = *groups[(size_t) prevGroup];
            for (auto& m : prev.memberChannels) if (m == globalOutputCh) m = -1;
        }
    }
    g.memberChannels[(size_t) slotIdx] = globalOutputCh;
    rebuildChannelLookup();
}

void OutputGroupManager::moveGroupFader (int groupIdx, float newFaderDb, RoutingMatrix& matrix)
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];
    const float oldDb = g.faderDb.load (std::memory_order_relaxed);
    const float delta = newFaderDb - oldDb;
    g.faderDb.store (newFaderDb, std::memory_order_relaxed);
    if (delta == 0.0f) return;

    // Copy the member list under the lock then RELEASE before touching the
    // matrix.  The audio thread's forEachGroupForAudio() try-locks on the
    // same SpinLock; while we were holding it across N matrix writes, every
    // audio block during a fader drag would fall through without group
    // processing -> audible dropouts on the group bus during a fader ride.
    std::vector<int> members;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        members.assign (g.memberChannels.begin(), g.memberChannels.end());
    }
    for (int ch : members)
    {
        if (ch < 0 || ch >= matrix.getNumOutputs()) continue;
        const float currentDb = linToDb (matrix.getOutputTrim (ch));
        const float newDb     = juce::jlimit (-60.0f, 12.0f, currentDb + delta);
        matrix.setOutputTrim (ch, dbToLin (newDb));
    }
}

void OutputGroupManager::setGroupMute (int groupIdx, bool m, RoutingMatrix& matrix)
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];
    g.muted.store (m, std::memory_order_relaxed);

    // Same lock-release pattern as moveGroupFader so group mute toggles don't
    // starve the audio thread of the manager spinlock.
    std::vector<int> members;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        members.assign (g.memberChannels.begin(), g.memberChannels.end());
    }
    for (int ch : members)
        if (ch >= 0 && ch < matrix.getNumOutputs())
            matrix.setOutputMute (ch, m);
}

} // namespace dcr

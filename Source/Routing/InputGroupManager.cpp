#include "Routing/InputGroupManager.h"

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

void InputGroupManager::setNumInputChannels (int n)
{
    juce::SpinLock::ScopedLockType lk (lock);
    numInputs = juce::jmax (0, n);
    rebuildChannelLookup();
}

void InputGroupManager::rebuildChannelLookup()
{
    channelToGroupIdx.assign ((size_t) numInputs, -1);
    for (int gi = 0; gi < (int) groups.size(); ++gi)
    {
        if (! groups[(size_t) gi]) continue;
        for (int ch : groups[(size_t) gi]->memberChannels)
            if (ch >= 0 && ch < numInputs)
                channelToGroupIdx[(size_t) ch] = gi;
    }
}

int InputGroupManager::createGroup (juce::String name, juce::AudioChannelSet cs)
{
    juce::SpinLock::ScopedLockType lk (lock);
    auto g = std::make_unique<OutputGroup>();
    g->name        = std::move (name);
    g->channelSet  = cs;
    g->memberChannels.assign ((size_t) cs.size(), -1);
    groups.push_back (std::move (g));
    return (int) groups.size() - 1;
}

void InputGroupManager::removeGroup (int groupIdx)
{
    juce::SpinLock::ScopedLockType lk (lock);
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    groups.erase (groups.begin() + groupIdx);
    rebuildChannelLookup();
}

OutputGroup* InputGroupManager::getGroup (int groupIdx) noexcept
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return nullptr;
    return groups[(size_t) groupIdx].get();
}

const OutputGroup* InputGroupManager::getGroup (int groupIdx) const noexcept
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return nullptr;
    return groups[(size_t) groupIdx].get();
}

int InputGroupManager::getGroupIndexForChannel (int ch) const noexcept
{
    if (ch < 0 || ch >= (int) channelToGroupIdx.size()) return -1;
    return channelToGroupIdx[(size_t) ch];
}

OutputGroup* InputGroupManager::getGroupForChannel (int ch) noexcept
{
    const int gi = getGroupIndexForChannel (ch);
    return gi < 0 ? nullptr : getGroup (gi);
}

void InputGroupManager::assignChannel (int groupIdx, int slotIdx, int globalInputCh)
{
    juce::SpinLock::ScopedLockType lk (lock);
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];
    if (slotIdx < 0 || slotIdx >= (int) g.memberChannels.size()) return;

    if (globalInputCh >= 0 && globalInputCh < numInputs)
    {
        const int prevGroup = channelToGroupIdx[(size_t) globalInputCh];
        if (prevGroup >= 0)
        {
            auto& prev = *groups[(size_t) prevGroup];
            for (auto& m : prev.memberChannels) if (m == globalInputCh) m = -1;
        }
    }
    g.memberChannels[(size_t) slotIdx] = globalInputCh;
    rebuildChannelLookup();
}

void InputGroupManager::moveGroupFader (int groupIdx, float newFaderDb, RoutingMatrix& matrix)
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];
    const float oldDb = g.faderDb.load (std::memory_order_relaxed);
    const float delta = newFaderDb - oldDb;
    g.faderDb.store (newFaderDb, std::memory_order_relaxed);
    if (delta == 0.0f) return;

    // Copy members under lock then release before touching matrix; see
    // OutputGroupManager::moveGroupFader for full rationale (audio thread
    // try-locks the same SpinLock for forEachGroupForAudio).
    std::vector<int> members;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        members.assign (g.memberChannels.begin(), g.memberChannels.end());
    }
    for (int ch : members)
    {
        if (ch < 0 || ch >= matrix.getNumInputs()) continue;
        const float currentDb = linToDb (matrix.getInputTrim (ch));
        const float newDb     = juce::jlimit (-60.0f, 12.0f, currentDb + delta);
        matrix.setInputTrim (ch, dbToLin (newDb));
    }
}

void InputGroupManager::setGroupMute (int groupIdx, bool m, RoutingMatrix& matrix)
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];
    g.muted.store (m, std::memory_order_relaxed);

    std::vector<int> members;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        members.assign (g.memberChannels.begin(), g.memberChannels.end());
    }
    for (int ch : members)
        if (ch >= 0 && ch < matrix.getNumInputs())
            matrix.setInputMute (ch, m);
}

} // namespace dcr

#include "Routing/InputGroupManager.h"

#include "Routing/GroupGain.h"
#include "Routing/RoutingMatrix.h"

namespace dcr {

using namespace dcr::groupgain;   // dbToLin / linToDb / clampTrimDb / routerChannelGain / bakeVcaTrimDb

void InputGroupManager::setNumInputChannels (int n)
{
    juce::SpinLock::ScopedLockType lk (lock);
    numInputs = juce::jmax (0, n);
    rebuildChannelLookup();
    recomputeRouterGains();
}

// Rebuild channelRouterGain from scratch (call under lock).  Mirror of
// OutputGroupManager::recomputeRouterGains on the input trims -- see there for
// the realloc RT-safety rationale (buffer swap only via setNumInputChannels
// while the matrix thread is stopped; all other callers preserve the size and
// only re-store into existing atomics).
void InputGroupManager::recomputeRouterGains()
{
    if ((int) channelRouterGain.size() != numInputs)
    {
        std::vector<std::atomic<float>> v ((size_t) numInputs);
        for (auto& a : v) a.store (1.0f, std::memory_order_relaxed);
        channelRouterGain = std::move (v);
    }
    else
    {
        for (auto& a : channelRouterGain) a.store (1.0f, std::memory_order_relaxed);
    }

    for (auto& gp : groups)
    {
        if (! gp || gp->faderMode.load (std::memory_order_relaxed) != OutputGroup::FaderMode::Router)
            continue;
        const float gain = routerChannelGain (gp->muted.load (std::memory_order_relaxed),
                                               gp->faderDb.load (std::memory_order_relaxed));
        for (int ch : gp->memberChannels)
            if (ch >= 0 && ch < numInputs)
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
    }
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
    recomputeRouterGains();   // a removed Router group's members must drop back to unity
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
    recomputeRouterGains();   // membership change can move a channel in/out of a Router group
}

void InputGroupManager::moveGroupFader (int groupIdx, float newFaderDb, RoutingMatrix& matrix)
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];

    // Copy members under lock then release before touching matrix / overlay
    // gains; see OutputGroupManager::moveGroupFader for full rationale (audio
    // thread try-locks the same SpinLock for forEachGroupForAudio).
    auto copyMembers = [&]
    {
        std::vector<int> members;
        juce::SpinLock::ScopedLockType lk (lock);
        members.assign (g.memberChannels.begin(), g.memberChannels.end());
        return members;
    };

    if (g.faderMode.load (std::memory_order_relaxed) == OutputGroup::FaderMode::Router)
    {
        g.faderDb.store (newFaderDb, std::memory_order_relaxed);
        const float gain = routerChannelGain (g.muted.load (std::memory_order_relaxed), newFaderDb);
        const auto members = copyMembers();
        for (int ch : members)
            if (ch >= 0 && ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
        matrix.touch();
        return;
    }

    const float oldDb = g.faderDb.load (std::memory_order_relaxed);
    const float delta = newFaderDb - oldDb;
    g.faderDb.store (newFaderDb, std::memory_order_relaxed);
    if (delta == 0.0f) return;

    const auto members = copyMembers();
    for (int ch : members)
    {
        if (ch < 0 || ch >= matrix.getNumInputs()) continue;
        const float newDb = bakeVcaTrimDb (linToDb (matrix.getInputTrim (ch)), delta);
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

    if (g.faderMode.load (std::memory_order_relaxed) == OutputGroup::FaderMode::Router)
    {
        const float gain = routerChannelGain (m, g.faderDb.load (std::memory_order_relaxed));
        for (int ch : members)
            if (ch >= 0 && ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
        matrix.touch();
        return;
    }

    for (int ch : members)
        if (ch >= 0 && ch < matrix.getNumInputs())
            matrix.setInputMute (ch, m);
}

void InputGroupManager::setGroupFaderMode (int groupIdx, OutputGroup::FaderMode mode, RoutingMatrix& matrix)
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];
    if (g.faderMode.load (std::memory_order_relaxed) == mode) return;

    const float faderDb = g.faderDb.load (std::memory_order_relaxed);
    const bool  muted   = g.muted.load   (std::memory_order_relaxed);

    std::vector<int> members;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        members.assign (g.memberChannels.begin(), g.memberChannels.end());
    }

    g.faderMode.store (mode, std::memory_order_relaxed);
    g.faderDb.store (0.0f, std::memory_order_relaxed);

    if (mode == OutputGroup::FaderMode::Router)
    {
        const float gain = routerChannelGain (muted, 0.0f);
        for (int ch : members)
        {
            if (ch < 0 || ch >= matrix.getNumInputs()) continue;
            matrix.setInputMute (ch, false);
            if (ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
        }
    }
    else
    {
        for (int ch : members)
        {
            if (ch < 0 || ch >= matrix.getNumInputs()) continue;
            const float bakedDb = bakeVcaTrimDb (linToDb (matrix.getInputTrim (ch)), faderDb);
            matrix.setInputTrim (ch, dbToLin (bakedDb));
            matrix.setInputMute (ch, muted);
            if (ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (1.0f, std::memory_order_relaxed);
        }
    }
    matrix.touch();
}

} // namespace dcr

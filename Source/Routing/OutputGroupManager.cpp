#include "Routing/OutputGroupManager.h"

#include "Routing/GroupGain.h"
#include "Routing/RoutingMatrix.h"

namespace dcr {

using namespace dcr::groupgain;   // dbToLin / linToDb / clampTrimDb / routerChannelGain / bakeVcaTrimDb

void OutputGroupManager::setNumOutputChannels (int n)
{
    juce::SpinLock::ScopedLockType lk (lock);
    numOutputs = juce::jmax (0, n);
    rebuildChannelLookup();
    recomputeRouterGains();
}

// Rebuild channelRouterGain from scratch (call under lock).  Sized to numOutputs
// and reset to unity; then every Router-mode group writes its members' overlay
// gain.  Cheap and only runs on structural changes / restart, never per audio
// block.
//
// RT-safety of the realloc: the vector's buffer is only replaced when the size
// changed, which happens ONLY via setNumOutputChannels() during an engine
// reconfigure -- and that runs before processor.configure()/start(), i.e. while
// the matrix thread (the sole reader of getChannelRouterGain()) is stopped, the
// same model the routing matrix's own resize() relies on.  Every other caller
// (assignChannel, removeGroup) preserves numOutputs, so the size matches and we
// only re-store into the existing atomics -- no buffer swap, safe to run while
// the RT thread reads.
void OutputGroupManager::recomputeRouterGains()
{
    if ((int) channelRouterGain.size() != numOutputs)
    {
        std::vector<std::atomic<float>> v ((size_t) numOutputs);
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
            if (ch >= 0 && ch < numOutputs)
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
    }
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
    recomputeRouterGains();   // a removed Router group's members must drop back to unity
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
    recomputeRouterGains();   // membership change can move a channel in/out of a Router group
}

void OutputGroupManager::moveGroupFader (int groupIdx, float newFaderDb, RoutingMatrix& matrix)
{
    if (groupIdx < 0 || groupIdx >= (int) groups.size()) return;
    auto& g = *groups[(size_t) groupIdx];

    // Copy the member list under the lock then RELEASE before touching the
    // matrix / overlay gains.  The audio thread's forEachGroupForAudio()
    // try-locks on the same SpinLock; holding it across N writes would make
    // every audio block during a fader drag fall through without group
    // processing -> audible dropouts on the group bus during a fader ride.
    auto copyMembers = [&]
    {
        std::vector<int> members;
        juce::SpinLock::ScopedLockType lk (lock);
        members.assign (g.memberChannels.begin(), g.memberChannels.end());
        return members;
    };

    if (g.faderMode.load (std::memory_order_relaxed) == OutputGroup::FaderMode::Router)
    {
        // Router: the fader is an overlay stage; just refresh the per-channel
        // gain.  Member trims are untouched.
        g.faderDb.store (newFaderDb, std::memory_order_relaxed);
        const float gain = routerChannelGain (g.muted.load (std::memory_order_relaxed), newFaderDb);
        const auto members = copyMembers();
        for (int ch : members)
            if (ch >= 0 && ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
        matrix.touch();   // no matrix cell changed -> force the RT gain refresh
        return;
    }

    // VCA: apply the dB delta to each member's own trim (linked fader).
    const float oldDb = g.faderDb.load (std::memory_order_relaxed);
    const float delta = newFaderDb - oldDb;
    g.faderDb.store (newFaderDb, std::memory_order_relaxed);
    if (delta == 0.0f) return;

    const auto members = copyMembers();
    for (int ch : members)
    {
        if (ch < 0 || ch >= matrix.getNumOutputs()) continue;
        const float newDb = bakeVcaTrimDb (linToDb (matrix.getOutputTrim (ch)), delta);
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

    if (g.faderMode.load (std::memory_order_relaxed) == OutputGroup::FaderMode::Router)
    {
        // Router: mute folds into the overlay gain; member matrix mutes stay
        // the user's own.
        const float gain = routerChannelGain (m, g.faderDb.load (std::memory_order_relaxed));
        for (int ch : members)
            if (ch >= 0 && ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
        matrix.touch();
        return;
    }

    // VCA: propagate onto each member channel's mute.
    for (int ch : members)
        if (ch >= 0 && ch < matrix.getNumOutputs())
            matrix.setOutputMute (ch, m);
}

void OutputGroupManager::setGroupFaderMode (int groupIdx, OutputGroup::FaderMode mode, RoutingMatrix& matrix)
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

    // Unified rule: preserve the audible level, reset the group fader to 0 dB.
    g.faderMode.store (mode, std::memory_order_relaxed);
    g.faderDb.store (0.0f, std::memory_order_relaxed);

    if (mode == OutputGroup::FaderMode::Router)
    {
        // Entering Router: member trims stay; the Router stage owns group mute,
        // so clear member matrix mutes and encode mute in the overlay (0 dB
        // fader -> unity unless muted).
        const float gain = routerChannelGain (muted, 0.0f);
        for (int ch : members)
        {
            if (ch < 0 || ch >= matrix.getNumOutputs()) continue;
            matrix.setOutputMute (ch, false);
            if (ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (gain, std::memory_order_relaxed);
        }
    }
    else
    {
        // Entering VCA: bake the overlay into member trims (preserve level),
        // re-assert group mute onto member mutes, drop the overlay stage.
        for (int ch : members)
        {
            if (ch < 0 || ch >= matrix.getNumOutputs()) continue;
            const float bakedDb = bakeVcaTrimDb (linToDb (matrix.getOutputTrim (ch)), faderDb);
            matrix.setOutputTrim (ch, dbToLin (bakedDb));
            matrix.setOutputMute (ch, muted);
            if (ch < (int) channelRouterGain.size())
                channelRouterGain[(size_t) ch].store (1.0f, std::memory_order_relaxed);
        }
    }
    matrix.touch();
}

void OutputGroupManager::addGroupInsertLatencySamples (std::vector<int>& perCh) const
{
    // No lock by design.  This runs on the message thread, which is the SOLE
    // mutator of the group list / member channels / slot hosts, so it can't race
    // its own writers; the audio thread only ever READS this structure (under its
    // own try-lock in forEachGroupForAudio).  Taking the manager lock here would
    // instead risk that try-lock failing and dropping a block of group inserts --
    // the very starvation moveGroupFader()/setGroupMute() go out of their way to
    // avoid.  Matches the lock-free message-thread reads in getGroup()/etc.
    for (auto& g : groups)
    {
        if (g == nullptr) continue;
        int groupLat = 0;
        for (auto& slot : g->pluginSlots)
            if (slot != nullptr) groupLat += slot->getChainLatencySamples();
        if (groupLat <= 0) continue;
        // A group insert delays the whole gathered bus, so its latency lands
        // on every member output channel equally.
        for (int ch : g->memberChannels)
            if (ch >= 0 && (size_t) ch < perCh.size())
                perCh[(size_t) ch] += groupLat;
    }
}

} // namespace dcr

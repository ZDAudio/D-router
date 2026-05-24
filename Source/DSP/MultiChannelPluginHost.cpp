#include "DSP/MultiChannelPluginHost.h"

#include <chrono>

namespace dcr {

void MultiChannelPluginHost::prepare (double sr, int bs, int nCh)
{
    sampleRate  = sr;
    blockSize   = bs;
    numChannels = juce::jmax (1, nCh);
    scratch.setSize (numChannels, blockSize, false, true, true);

    juce::AudioPluginInstance* p = current.get();
    if (p != nullptr)
    {
        p->releaseResources();
        p->prepareToPlay (sr, bs);
    }
}

static bool tryConfigureLayout (juce::AudioPluginInstance& p,
                                const juce::AudioChannelSet& desiredLayout)
{
    auto layout = p.getBusesLayout();
    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty()) return false;

    auto candidate = layout;
    candidate.inputBuses .set (0, desiredLayout);
    candidate.outputBuses.set (0, desiredLayout);
    if (p.checkBusesLayoutSupported (candidate))
        return p.setBusesLayout (candidate);

    // Fallback: discrete N-channel.
    auto disc = juce::AudioChannelSet::discreteChannels (desiredLayout.size());
    candidate.inputBuses .set (0, disc);
    candidate.outputBuses.set (0, disc);
    if (p.checkBusesLayoutSupported (candidate))
        return p.setBusesLayout (candidate);

    return false;
}

void MultiChannelPluginHost::setPlugin (std::unique_ptr<juce::AudioPluginInstance> p,
                                        const juce::AudioChannelSet& desiredLayout)
{
    if (p != nullptr)
    {
        p->releaseResources();
        tryConfigureLayout (*p, desiredLayout);
        p->prepareToPlay (sampleRate, blockSize);
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        old = std::move (current);
        current = std::move (p);
        if (current != nullptr)
            scratch.setSize (juce::jmax (numChannels,
                                         current->getTotalNumInputChannels(),
                                         current->getTotalNumOutputChannels()),
                             blockSize, false, true, true);
    }
    // old destructed outside lock
}

void MultiChannelPluginHost::swapStateWith (MultiChannelPluginHost& other)
{
    if (this == &other) return;
    // Deterministic lock order by address — avoids deadlock against another
    // concurrent swap.
    auto* lo = (this < &other) ? this  : &other;
    auto* hi = (this < &other) ? &other : this;
    const juce::SpinLock::ScopedLockType llk (lo->lock);
    const juce::SpinLock::ScopedLockType hlk (hi->lock);

    std::swap (current, other.current);

    const bool byp = bypassed.load (std::memory_order_relaxed);
    bypassed.store (other.bypassed.load (std::memory_order_relaxed), std::memory_order_relaxed);
    other.bypassed.store (byp, std::memory_order_relaxed);

    const float cpu = cpuLoadAvg.load (std::memory_order_relaxed);
    cpuLoadAvg.store (other.cpuLoadAvg.load (std::memory_order_relaxed), std::memory_order_relaxed);
    other.cpuLoadAvg.store (cpu, std::memory_order_relaxed);
}

void MultiChannelPluginHost::clearPlugin()
{
    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        old = std::move (current);
    }
}

void MultiChannelPluginHost::processBlock (float* const* channels, int numSamples)
{
    if (bypassed.load (std::memory_order_relaxed)) { cpuLoadAvg.store (cpuLoadAvg.load() * 0.99f); return; }

    juce::SpinLock::ScopedTryLockType lk (lock);
    if (! lk.isLocked() || current == nullptr) { cpuLoadAvg.store (cpuLoadAvg.load() * 0.99f); return; }
    if (numSamples > scratch.getNumSamples()) return;

    const int totalInPlugin  = juce::jmax (current->getTotalNumInputChannels(),
                                           current->getTotalNumOutputChannels());
    if (scratch.getNumChannels() < totalInPlugin)
        return;   // shouldn't happen, prepare/setPlugin sized scratch

    const auto t0 = std::chrono::steady_clock::now();

    // Copy input into scratch (zero any plugin channels beyond our N).
    for (int c = 0; c < scratch.getNumChannels(); ++c)
    {
        if (c < numChannels && channels[c] != nullptr)
            scratch.copyFrom (c, 0, channels[c], numSamples);
        else
            scratch.clear (c, 0, numSamples);
    }

    current->processBlock (scratch, dummyMidi);

    // Copy output back (only our N channels; extras are dropped).
    for (int c = 0; c < numChannels; ++c)
    {
        if (channels[c] == nullptr) continue;
        if (c < scratch.getNumChannels())
            std::memcpy (channels[c], scratch.getReadPointer (c),
                         (size_t) numSamples * sizeof (float));
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedSec     = std::chrono::duration<double> (t1 - t0).count();
    const double blockPeriodSec = numSamples / juce::jmax (1.0, sampleRate);
    const float  load           = (float) (elapsedSec / blockPeriodSec);
    const float  oldAvg         = cpuLoadAvg.load (std::memory_order_relaxed);
    cpuLoadAvg.store (oldAvg * 0.95f + load * 0.05f, std::memory_order_relaxed);
}

} // namespace dcr

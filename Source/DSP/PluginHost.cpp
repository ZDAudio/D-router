#include "DSP/PluginHost.h"

#include <chrono>

namespace dcr {

void PluginHost::prepare (double sr, int bs)
{
    sampleRate = sr;
    blockSize  = bs;

    juce::AudioPluginInstance* p = current.get();
    if (p != nullptr)
    {
        p->releaseResources();
        p->prepareToPlay (sr, bs);
        scratch.setSize (juce::jmax (p->getTotalNumInputChannels(),
                                     p->getTotalNumOutputChannels()),
                         bs, false, true, true);
    }
}

void PluginHost::setPlugin (std::unique_ptr<juce::AudioPluginInstance> p)
{
    // Prepare the incoming plugin outside the lock.
    if (p != nullptr)
    {
        p->releaseResources();
        p->prepareToPlay (sampleRate, blockSize);
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        old = std::move (current);
        current = std::move (p);
        if (current != nullptr)
            scratch.setSize (juce::jmax (current->getTotalNumInputChannels(),
                                         current->getTotalNumOutputChannels()),
                             blockSize, false, true, true);
        else
            scratch.setSize (0, 0);
    }
    // 'old' is destructed here, outside the lock.
}

void PluginHost::processBlock (float* buf, int numSamples)
{
    if (bypassed.load (std::memory_order_relaxed)) { cpuLoadAvg.store (cpuLoadAvg.load() * 0.99f); return; }

    juce::SpinLock::ScopedTryLockType lk (lock);
    if (! lk.isLocked() || current == nullptr) { cpuLoadAvg.store (cpuLoadAvg.load() * 0.99f); return; }

    if (numSamples > scratch.getNumSamples()) return;     // sized for blockSize

    const int chs = scratch.getNumChannels();
    if (chs <= 0) return;

    const auto t0 = std::chrono::steady_clock::now();

    // Copy mono input into every plugin channel.
    for (int c = 0; c < chs; ++c)
        scratch.copyFrom (c, 0, buf, numSamples);

    current->processBlock (scratch, dummyMidi);

    // Take first output channel (or average L+R for stereo plugins) back to mono.
    const int outChs = juce::jmin (current->getTotalNumOutputChannels(), chs);
    if (outChs >= 2)
    {
        const float* L = scratch.getReadPointer (0);
        const float* R = scratch.getReadPointer (1);
        for (int s = 0; s < numSamples; ++s)
            buf[s] = 0.5f * (L[s] + R[s]);
    }
    else if (outChs == 1)
    {
        const float* L = scratch.getReadPointer (0);
        for (int s = 0; s < numSamples; ++s)
            buf[s] = L[s];
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedSec     = std::chrono::duration<double> (t1 - t0).count();
    const double blockPeriodSec = numSamples / juce::jmax (1.0, sampleRate);
    const float  load           = (float) (elapsedSec / blockPeriodSec);
    const float  oldAvg         = cpuLoadAvg.load (std::memory_order_relaxed);
    cpuLoadAvg.store (oldAvg * 0.95f + load * 0.05f, std::memory_order_relaxed);
}

} // namespace dcr

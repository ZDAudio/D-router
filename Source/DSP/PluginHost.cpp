#include "DSP/PluginHost.h"

#include <chrono>

namespace dcr {

void PluginHost::prepare (double sr, int bs)
{
    sampleRate = sr;
    blockSize  = bs;
    for (auto& s : slots)
    {
        if (auto* p = s.current.get())
        {
            p->releaseResources();
            p->prepareToPlay (sr, bs);
            s.scratch.setSize (juce::jmax (p->getTotalNumInputChannels(),
                                           p->getTotalNumOutputChannels()),
                               bs, false, true, true);
        }
    }
}

void PluginHost::setPluginAt (int slotIdx, std::unique_ptr<juce::AudioPluginInstance> p)
{
    if (slotIdx < 0 || slotIdx >= kNumSlots) return;
    auto& s = slots[(size_t) slotIdx];

    if (p != nullptr)
    {
        p->releaseResources();
        p->prepareToPlay (sampleRate, blockSize);
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        juce::SpinLock::ScopedLockType lk (s.lock);
        old = std::move (s.current);
        s.current = std::move (p);
        if (s.current != nullptr)
            s.scratch.setSize (juce::jmax (s.current->getTotalNumInputChannels(),
                                           s.current->getTotalNumOutputChannels()),
                               blockSize, false, true, true);
        else
            s.scratch.setSize (0, 0);
    }
    // 'old' destructs here, outside the lock.
}

void PluginHost::swapSlots (int a, int b)
{
    if (a == b) return;
    if (a < 0 || a >= kNumSlots || b < 0 || b >= kNumSlots) return;
    // Deterministic lock order — lower index first — so two concurrent swaps
    // can never deadlock against each other.
    const int lo = juce::jmin (a, b);
    const int hi = juce::jmax (a, b);
    auto& sLo = slots[(size_t) lo];
    auto& sHi = slots[(size_t) hi];
    const juce::SpinLock::ScopedLockType llk (sLo.lock);
    const juce::SpinLock::ScopedLockType hlk (sHi.lock);

    std::swap (sLo.current, sHi.current);

    const bool bypLo = sLo.bypassed.load (std::memory_order_relaxed);
    sLo.bypassed.store (sHi.bypassed.load (std::memory_order_relaxed), std::memory_order_relaxed);
    sHi.bypassed.store (bypLo, std::memory_order_relaxed);

    const float cpuLo = sLo.cpuLoadAvg.load (std::memory_order_relaxed);
    sLo.cpuLoadAvg.store (sHi.cpuLoadAvg.load (std::memory_order_relaxed), std::memory_order_relaxed);
    sHi.cpuLoadAvg.store (cpuLo, std::memory_order_relaxed);

    // Resize each scratch to match its (now swapped) plugin's channel count.
    auto fixScratch = [this] (Slot& s)
    {
        if (s.current != nullptr)
            s.scratch.setSize (juce::jmax (s.current->getTotalNumInputChannels(),
                                           s.current->getTotalNumOutputChannels()),
                               blockSize, false, true, true);
        else
            s.scratch.setSize (0, 0);
    };
    fixScratch (sLo);
    fixScratch (sHi);
}

void PluginHost::processBlock (float* buf, int numSamples)
{
    for (auto& s : slots)
    {
        if (s.bypassed.load (std::memory_order_relaxed))
        {
            s.cpuLoadAvg.store (s.cpuLoadAvg.load() * 0.99f, std::memory_order_relaxed);
            continue;
        }

        juce::SpinLock::ScopedTryLockType lk (s.lock);
        if (! lk.isLocked() || s.current == nullptr)
        {
            s.cpuLoadAvg.store (s.cpuLoadAvg.load() * 0.99f, std::memory_order_relaxed);
            continue;
        }

        if (numSamples > s.scratch.getNumSamples()) continue;
        const int chs = s.scratch.getNumChannels();
        if (chs <= 0) continue;

        const auto t0 = std::chrono::steady_clock::now();

        // Copy mono in -> every channel
        for (int c = 0; c < chs; ++c)
            s.scratch.copyFrom (c, 0, buf, numSamples);

        s.current->processBlock (s.scratch, dummyMidi);

        // Take L (or average L+R for stereo plugins) back to mono.
        const int outChs = juce::jmin (s.current->getTotalNumOutputChannels(), chs);
        if (outChs >= 2)
        {
            const float* L = s.scratch.getReadPointer (0);
            const float* R = s.scratch.getReadPointer (1);
            for (int x = 0; x < numSamples; ++x)
                buf[x] = 0.5f * (L[x] + R[x]);
        }
        else if (outChs == 1)
        {
            const float* L = s.scratch.getReadPointer (0);
            for (int x = 0; x < numSamples; ++x)
                buf[x] = L[x];
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsedSec     = std::chrono::duration<double> (t1 - t0).count();
        const double blockPeriodSec = numSamples / juce::jmax (1.0, sampleRate);
        const float  load           = (float) (elapsedSec / blockPeriodSec);
        const float  oldAvg         = s.cpuLoadAvg.load (std::memory_order_relaxed);
        s.cpuLoadAvg.store (oldAvg * 0.95f + load * 0.05f, std::memory_order_relaxed);
    }
}

} // namespace dcr

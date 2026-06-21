#include "DSP/PluginHost.h"

#include <chrono>

#if JUCE_MAC
#import <Foundation/Foundation.h>
#endif

namespace dcr
{

namespace
{
// Wrap a plugin operation against C++ AND Objective-C exceptions.  AU
// plugins frequently throw NSException out of methods that look pure
// C++ from our side (any internal call into Cocoa can do this).  If
// we don't catch them, the unhandled NSException unwinds through C++
// frames and terminates the process.  Returns true if op completed.
inline bool runGuarded(const char* op, juce::AudioPluginInstance* p, void (^block)()) noexcept
{
#if JUCE_MAC
    @try
    {
        @try
        {
            block();
            return true;
        }
        @catch (NSException* ex)
        {
            DBG("[plugin " << (p ? p->getName() : juce::String("?"))
                           << "] NSException in " << op
                           << ": " << [[ex reason] UTF8String]);
            return false;
        }
    }
    @catch (...)
    {
        // Defensive: catch anything else, including foreign exceptions.
        DBG("[plugin " << (p ? p->getName() : juce::String("?"))
                       << "] unknown exception in " << op);
        return false;
    }
#else
    try
    {
        block();
        return true;
    }
    catch (...)
    {
        DBG("[plugin] exception in " << op);
        return false;
    }
#endif
}
} // namespace

void PluginHost::prepare(double sr, int bs)
{
    sampleRate = sr;
    blockSize = bs;
    for (auto& s : slots)
    {
        if (auto* p = s.current.get())
        {
            // releaseResources + prepareToPlay are guarded -- plugins
            // sometimes throw NSException out of these (Cocoa init paths).
            // If the prepare phase throws, mark the slot broken so the
            // audio thread skips it instead of crashing on the first
            // processBlock with an uninitialised plugin.
            const bool ok =
                runGuarded("prepare", p, ^{
                  p->releaseResources();
                  p->prepareToPlay(sr, bs);
                });
            if (!ok)
            {
                s.broken.store(true, std::memory_order_relaxed);
                continue;
            }
            s.broken.store(false, std::memory_order_relaxed);
            s.scratch.setSize(juce::jmax(p->getTotalNumInputChannels(),
                                         p->getTotalNumOutputChannels()),
                              bs, false, true, true);
        }
    }
}

void PluginHost::setPluginAt(int slotIdx, std::unique_ptr<juce::AudioPluginInstance> p, const juce::MemoryBlock* stateToRestore)
{
    if (slotIdx < 0 || slotIdx >= kNumSlots)
        return;
    auto& s = slots[(size_t)slotIdx];

    bool prepareOk = true;
    if (p != nullptr)
    {
        auto* raw = p.get();
        // Canonical AU restore order, performed while the instance is still
        // private to this (message) thread and invisible to the audio thread:
        //   1. releaseResources + prepareToPlay  (make it ready to run)
        //   2. setStateInformation               (apply saved state)
        // The OLD restore path called setStateInformation on the raw,
        // UNPREPARED instance and THEN re-prepared here -- for stateful AUs
        // that either discarded the state or threw an NSException out of
        // prepareToPlay, which marked the slot broken and left the plugin
        // loaded-but-silent until a manual reload.
        prepareOk = runGuarded("prepare", raw,
                               ^{
                                 raw->releaseResources();
                                 raw->prepareToPlay(sampleRate, blockSize);
                               });

        if (!prepareOk)
            juce::Logger::writeToLog("[plugin " + raw->getName() + "] prepareToPlay FAILED on install -- slot marked broken");

        if (prepareOk && stateToRestore != nullptr && stateToRestore->getSize() > 0)
        {
            const void* data = stateToRestore->getData();
            const int size = (int)stateToRestore->getSize();
            const bool stateOk = runGuarded("setStateInformation", raw,
                                            ^{
                                              raw->setStateInformation(data, size);
                                            });
            if (!stateOk)
                juce::Logger::writeToLog("[plugin " + raw->getName() + "] setStateInformation FAILED -- keeping default state (still active)");
            // A state-restore failure deliberately does NOT mark the slot
            // broken: a prepared plugin at default state is far better than a
            // disabled, silent slot.
        }
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        juce::SpinLock::ScopedLockType lk(s.lock);
        old = std::move(s.current);
        s.current = std::move(p);
        // New install -- reset broken flag.  If prepare itself threw, mark
        // broken so the audio thread skips this slot until the user
        // reloads with a working plugin.
        s.broken.store(!prepareOk, std::memory_order_relaxed);
        if (s.current != nullptr)
            s.scratch.setSize(juce::jmax(s.current->getTotalNumInputChannels(),
                                         s.current->getTotalNumOutputChannels()),
                              blockSize, false, true, true);
        else
            s.scratch.setSize(0, 0);
    }
    // 'old' destructs here, outside the lock.  Plugin destruction is also
    // guarded -- some AUs throw out of their dtor (rare but seen).
    if (old != nullptr)
    {
        auto* rawOld = old.release();
        runGuarded("destruct", rawOld, ^{
          delete rawOld;
        });
    }
}

void PluginHost::swapSlots(int a, int b)
{
    if (a == b)
        return;
    if (a < 0 || a >= kNumSlots || b < 0 || b >= kNumSlots)
        return;
    // Deterministic lock order — lower index first — so two concurrent swaps
    // can never deadlock against each other.
    const int lo = juce::jmin(a, b);
    const int hi = juce::jmax(a, b);
    auto& sLo = slots[(size_t)lo];
    auto& sHi = slots[(size_t)hi];
    const juce::SpinLock::ScopedLockType llk(sLo.lock);
    const juce::SpinLock::ScopedLockType hlk(sHi.lock);

    std::swap(sLo.current, sHi.current);

    const bool bypLo = sLo.bypassed.load(std::memory_order_relaxed);
    sLo.bypassed.store(sHi.bypassed.load(std::memory_order_relaxed), std::memory_order_relaxed);
    sHi.bypassed.store(bypLo, std::memory_order_relaxed);

    const float cpuLo = sLo.cpuLoadAvg.load(std::memory_order_relaxed);
    sLo.cpuLoadAvg.store(sHi.cpuLoadAvg.load(std::memory_order_relaxed), std::memory_order_relaxed);
    sHi.cpuLoadAvg.store(cpuLo, std::memory_order_relaxed);

    // Broken state travels with the plugin -- if the user drags a misbehaving
    // plugin into a fresh slot, that fresh slot inherits the "broken" mark
    // and the audio thread keeps skipping it.
    const bool brokenLo = sLo.broken.load(std::memory_order_relaxed);
    sLo.broken.store(sHi.broken.load(std::memory_order_relaxed), std::memory_order_relaxed);
    sHi.broken.store(brokenLo, std::memory_order_relaxed);

    // Resize each scratch to match its (now swapped) plugin's channel count.
    auto fixScratch = [this](Slot& s)
    {
        if (s.current != nullptr)
            s.scratch.setSize(juce::jmax(s.current->getTotalNumInputChannels(),
                                         s.current->getTotalNumOutputChannels()),
                              blockSize, false, true, true);
        else
            s.scratch.setSize(0, 0);
    };
    fixScratch(sLo);
    fixScratch(sHi);
}

void PluginHost::processBlock(float* buf, int numSamples)
{
    for (auto& s : slots)
    {
        // Skip silently if this slot's plugin ever threw -- we can't trust
        // it to behave on subsequent blocks.  The user has to manually
        // reload (which resets the broken flag in setPluginAt).
        if (s.broken.load(std::memory_order_relaxed))
            continue;

        if (s.bypassed.load(std::memory_order_relaxed))
        {
            s.cpuLoadAvg.store(s.cpuLoadAvg.load() * 0.99f, std::memory_order_relaxed);
            continue;
        }

        juce::SpinLock::ScopedTryLockType lk(s.lock);
        if (!lk.isLocked() || s.current == nullptr)
        {
            s.cpuLoadAvg.store(s.cpuLoadAvg.load() * 0.99f, std::memory_order_relaxed);
            continue;
        }

        if (numSamples > s.scratch.getNumSamples())
            continue;
        const int chs = s.scratch.getNumChannels();
        if (chs <= 0)
            continue;

        const auto t0 = std::chrono::steady_clock::now();

        // Copy mono in -> every channel
        for (int c = 0; c < chs; ++c)
            s.scratch.copyFrom(c, 0, buf, numSamples);

        // GUARDED -- if the plugin throws here (C++ or NSException), mark
        // the slot broken instead of letting the unhandled exception
        // terminate the entire audio thread.  Output gets the input copy
        // (no-op) for this block since we wrote to scratch but won't read
        // back if processBlock failed.
        auto* plug = s.current.get();
        auto& scratchRef = s.scratch;
        const bool ok = runGuarded("processBlock", plug,
                                   ^{
                                     plug->processBlock(scratchRef, dummyMidi);
                                   });
        if (!ok)
        {
            s.broken.store(true, std::memory_order_relaxed);
            // Fall through -- buf still holds the pre-plugin input, so the
            // signal continues passing through this slot rather than going
            // silent.
            continue;
        }

        // Take L (or average L+R for stereo plugins) back to mono.
        const int outChs = juce::jmin(s.current->getTotalNumOutputChannels(), chs);
        if (outChs >= 2)
        {
            const float* L = s.scratch.getReadPointer(0);
            const float* R = s.scratch.getReadPointer(1);
            for (int x = 0; x < numSamples; ++x)
                buf[x] = 0.5f * (L[x] + R[x]);
        }
        else if (outChs == 1)
        {
            const float* L = s.scratch.getReadPointer(0);
            for (int x = 0; x < numSamples; ++x)
                buf[x] = L[x];
        }

        const auto t1 = std::chrono::steady_clock::now();
        const double elapsedSec = std::chrono::duration<double>(t1 - t0).count();
        const double blockPeriodSec = numSamples / juce::jmax(1.0, sampleRate);
        const float load = (float)(elapsedSec / blockPeriodSec);
        const float oldAvg = s.cpuLoadAvg.load(std::memory_order_relaxed);
        s.cpuLoadAvg.store(oldAvg * 0.95f + load * 0.05f, std::memory_order_relaxed);
    }
}

} // namespace dcr

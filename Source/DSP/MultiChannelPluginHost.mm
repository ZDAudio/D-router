#include "DSP/MultiChannelPluginHost.h"

#include <chrono>

#if JUCE_MAC
 #import <Foundation/Foundation.h>
#endif

namespace dcr {

namespace
{
    // See PluginHost.mm runGuarded for rationale.  AU plugins can throw
    // NSException out of any method that crosses the Cocoa boundary; an
    // uncaught one kills the audio thread (and the process).
    inline bool runGuarded (const char* op, juce::AudioPluginInstance* p,
                            void (^block)()) noexcept
    {
       #if JUCE_MAC
        @try {
            @try { block(); return true; }
            @catch (NSException* ex) {
                DBG ("[group plugin " << (p ? p->getName() : juce::String ("?"))
                     << "] NSException in " << op << ": " << [[ex reason] UTF8String]);
                return false;
            }
        }
        @catch (...) {
            DBG ("[group plugin " << (p ? p->getName() : juce::String ("?"))
                 << "] unknown exception in " << op);
            return false;
        }
       #else
        try { block(); return true; }
        catch (...) { DBG ("[group plugin] exception in " << op); return false; }
       #endif
    }
}

void MultiChannelPluginHost::prepare (double sr, int bs, int nCh)
{
    sampleRate  = sr;
    blockSize   = bs;
    numChannels = juce::jmax (1, nCh);
    scratch.setSize (numChannels, blockSize, false, true, true);

    juce::AudioPluginInstance* p = current.get();
    if (p != nullptr)
    {
        const bool ok = runGuarded ("prepare", p,
            ^{ p->releaseResources(); p->prepareToPlay (sr, bs); });
        broken.store (! ok, std::memory_order_relaxed);
    }
}

static bool tryConfigureLayout (juce::AudioPluginInstance& p,
                                const juce::AudioChannelSet& desiredLayout)
{
    // Every getBusesLayout / checkBusesLayoutSupported / setBusesLayout call
    // is a potential exception source (the plugin can throw out of any of
    // them; some AUs do, especially when their internal layout state is
    // half-initialised).  Guard the entire routine so a bad layout probe
    // returns false instead of taking down the load.
    __block bool result = false;
    runGuarded ("configureLayout", &p, ^{
        auto layout = p.getBusesLayout();
        if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty()) { result = false; return; }

        auto candidate = layout;
        candidate.inputBuses .set (0, desiredLayout);
        candidate.outputBuses.set (0, desiredLayout);
        if (p.checkBusesLayoutSupported (candidate))
        { result = p.setBusesLayout (candidate); return; }

        // Fallback: discrete N-channel.
        auto disc = juce::AudioChannelSet::discreteChannels (desiredLayout.size());
        candidate.inputBuses .set (0, disc);
        candidate.outputBuses.set (0, disc);
        if (p.checkBusesLayoutSupported (candidate))
        { result = p.setBusesLayout (candidate); return; }

        result = false;
    });
    return result;
}

void MultiChannelPluginHost::setPlugin (std::unique_ptr<juce::AudioPluginInstance> p,
                                        const juce::AudioChannelSet& desiredLayout)
{
    bool prepareOk = true;
    if (p != nullptr)
    {
        auto* raw = p.get();
        const auto layoutCopy = desiredLayout;
        prepareOk = runGuarded ("setPlugin.prepare", raw, ^{
            raw->releaseResources();
            tryConfigureLayout (*raw, layoutCopy);
            raw->prepareToPlay (sampleRate, blockSize);
        });
    }

    std::unique_ptr<juce::AudioPluginInstance> old;
    {
        juce::SpinLock::ScopedLockType lk (lock);
        old = std::move (current);
        current = std::move (p);
        // Fresh install -- clear or set broken according to prepare result.
        broken.store (! prepareOk, std::memory_order_relaxed);
        if (current != nullptr)
            scratch.setSize (juce::jmax (numChannels,
                                         current->getTotalNumInputChannels(),
                                         current->getTotalNumOutputChannels()),
                             blockSize, false, true, true);
    }
    // Guarded destruct -- some AU dtors throw NSException on Cocoa cleanup.
    if (old != nullptr)
    {
        auto* rawOld = old.release();
        runGuarded ("destruct", rawOld, ^{ delete rawOld; });
    }
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

    // Broken state travels with the plugin.
    const bool br = broken.load (std::memory_order_relaxed);
    broken.store (other.broken.load (std::memory_order_relaxed), std::memory_order_relaxed);
    other.broken.store (br, std::memory_order_relaxed);
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
    if (broken.load (std::memory_order_relaxed)) return;

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

    // Guarded plugin call -- if processBlock throws, mark broken and bail.
    // The input was copied to scratch but never read back; the original
    // channel buffers remain whatever the matrix gave us, so audio
    // continues passing through unchanged for this group bus.
    auto* plug = current.get();
    auto& scratchRef = scratch;
    const bool ok = runGuarded ("processBlock", plug,
                                ^{ plug->processBlock (scratchRef, dummyMidi); });
    if (! ok)
    {
        broken.store (true, std::memory_order_relaxed);
        return;
    }

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

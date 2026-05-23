#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace dcr {

// Per-channel plugin slot. Holds at most one juce::AudioPluginInstance.
// The audio thread calls processBlock(); the UI thread can swap plugins via
// setPlugin(). A SpinLock serialises the rare swap; audio thread try-locks and
// skips processing if a swap is in progress (one block of "dry" pass-through).
class PluginHost
{
public:
    // Engine-side configuration (sample rate / block size). Call once per
    // host before processBlock is invoked.
    void prepare (double sr, int blockSize);

    // Swap in a new plugin instance (or nullptr to remove). prepareToPlay is
    // called on the incoming plugin *before* taking the lock so we don't hold
    // it across allocation. UI thread only.
    void setPlugin (std::unique_ptr<juce::AudioPluginInstance> p);

    // Convenience.
    void clearPlugin() { setPlugin (nullptr); }

    // Returns the currently loaded plugin, or nullptr. Pointer is owned by
    // PluginHost; do not delete. UI thread only.
    juce::AudioPluginInstance* getPlugin() const noexcept { return current.get(); }

    void setBypassed (bool b) noexcept { bypassed.store (b, std::memory_order_relaxed); }
    bool isBypassed()  const noexcept   { return bypassed.load (std::memory_order_relaxed); }

    // Audio thread: process numSamples of mono audio in-place. If the slot is
    // empty, bypassed, or the lock can't be acquired, the buffer is left
    // unchanged.
    void processBlock (float* buf, int numSamples);

    // EMA load 0..1 of the plugin's per-block CPU time relative to block
    // period.  Zero when bypassed/empty.
    float getCpuLoadAvg() const noexcept { return cpuLoadAvg.load (std::memory_order_relaxed); }

private:
    juce::SpinLock                              lock;
    std::unique_ptr<juce::AudioPluginInstance>  current;
    juce::AudioBuffer<float>                    scratch;
    juce::MidiBuffer                            dummyMidi;
    std::atomic<bool>                           bypassed { false };
    std::atomic<float>                          cpuLoadAvg { 0.0f };
    double                                      sampleRate = 48000.0;
    int                                         blockSize  = 128;
};

} // namespace dcr

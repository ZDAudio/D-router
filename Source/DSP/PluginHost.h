#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

namespace dcr {

// Per-channel mono plugin chain.  Holds up to kNumSlots plugins processed
// sequentially in slot order; each slot has its own bypass + CPU readout.
// The audio thread calls processBlock(); the UI thread mutates a single slot
// via setPluginAt() / clearSlot().  A SpinLock per slot serialises the rare
// swap so the audio thread only stalls on the slot currently being changed.
class PluginHost
{
public:
    static constexpr int kNumSlots = 3;

    void prepare (double sr, int blockSize);

    void setPluginAt (int slotIdx, std::unique_ptr<juce::AudioPluginInstance> p);
    void clearSlot   (int slotIdx) { setPluginAt (slotIdx, nullptr); }

    // Swap two slots' plugin + bypass + cpu-load state.  Both slot locks are
    // taken in a deterministic order to avoid deadlock with audio thread.
    // Scratch buffers stay in place (same size/prepare state).
    void swapSlots (int a, int b);

    juce::AudioPluginInstance* getPluginAt (int slotIdx) const noexcept
    {
        return (slotIdx >= 0 && slotIdx < kNumSlots) ? slots[(size_t) slotIdx].current.get() : nullptr;
    }

    void setBypassedAt (int slotIdx, bool b) noexcept
    {
        if (slotIdx >= 0 && slotIdx < kNumSlots)
            slots[(size_t) slotIdx].bypassed.store (b, std::memory_order_relaxed);
    }
    bool isBypassedAt (int slotIdx) const noexcept
    {
        return slotIdx >= 0 && slotIdx < kNumSlots
            && slots[(size_t) slotIdx].bypassed.load (std::memory_order_relaxed);
    }

    float getCpuLoadAt (int slotIdx) const noexcept
    {
        return (slotIdx >= 0 && slotIdx < kNumSlots)
            ? slots[(size_t) slotIdx].cpuLoadAvg.load (std::memory_order_relaxed) : 0.0f;
    }

    // True if any slot is currently loaded (regardless of bypass).
    bool anyLoaded() const noexcept
    {
        for (auto const& s : slots) if (s.current) return true;
        return false;
    }

    // True if any loaded-and-not-bypassed slot exists.
    bool anyActive() const noexcept
    {
        for (auto const& s : slots)
            if (s.current && ! s.bypassed.load (std::memory_order_relaxed)) return true;
        return false;
    }

    // True if any slot is bypassed.
    bool anyBypassed() const noexcept
    {
        for (auto const& s : slots)
            if (s.current && s.bypassed.load (std::memory_order_relaxed)) return true;
        return false;
    }

    // Audio thread: run the whole chain in-place on a mono buffer.
    void processBlock (float* buf, int numSamples);

    // Total CPU across all slots.
    float getCpuLoadAvg() const noexcept
    {
        float s = 0.0f;
        for (int i = 0; i < kNumSlots; ++i) s += getCpuLoadAt (i);
        return s;
    }

private:
    struct Slot
    {
        juce::SpinLock                              lock;
        std::unique_ptr<juce::AudioPluginInstance>  current;
        juce::AudioBuffer<float>                    scratch;
        std::atomic<bool>                           bypassed { false };
        std::atomic<float>                          cpuLoadAvg { 0.0f };
        // Set true when the plugin throws (C++ or NSException) during
        // processBlock / prepare.  Subsequent blocks skip the slot so a
        // misbehaving plugin can't crash the audio thread repeatedly.
        // Cleared whenever a new plugin is installed.
        std::atomic<bool>                           broken { false };
    };

    std::array<Slot, kNumSlots>                 slots;
    juce::MidiBuffer                            dummyMidi;
    double                                      sampleRate = 48000.0;
    int                                         blockSize  = 128;
};

} // namespace dcr

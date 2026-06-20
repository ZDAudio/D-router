#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace dcr {

// N-channel plugin slot for output groups (stereo, 5.1, 7.1.4, etc).
// Same swap-under-spinlock pattern as PluginHost; processBlock takes an array
// of channel pointers so we can gather from / scatter to the matrix output bus
// without an intermediate copy.
class MultiChannelPluginHost
{
public:
    void prepare (double sr, int blockSize, int numChannels);

    // Loads the new plugin off the audio thread.  Attempts to configure the
    // plugin's buses to a discrete N-channel layout matching `numChannels`.
    // If stateToRestore is non-null/non-empty its bytes are applied via
    // setStateInformation to the freshly-PREPARED instance before it goes
    // live -- canonical AU restore order (configure+prepareToPlay THEN
    // setStateInformation).  See PluginHost::setPluginAt for the rationale.
    void setPlugin (std::unique_ptr<juce::AudioPluginInstance> p,
                    const juce::AudioChannelSet& desiredLayout,
                    const juce::MemoryBlock* stateToRestore = nullptr);

    void clearPlugin();

    // Swap plugin + bypass + cpu-load state with another host of the SAME
    // channel layout (both must already be prepared with matching numChannels
    // / blockSize).  Used by the UI's drag-to-reorder.
    void swapStateWith (MultiChannelPluginHost& other);

    juce::AudioPluginInstance* getPlugin() const noexcept { return current.get(); }

    void setBypassed (bool b) noexcept { bypassed.store (b, std::memory_order_relaxed); }
    bool isBypassed()  const noexcept   { return bypassed.load (std::memory_order_relaxed); }

    // Audio thread:  in-place N-channel processing.  `channels[i]` points to
    // `numSamples` floats for channel i (i < numChannels).
    void processBlock (float* const* channels, int numSamples);

    int getNumChannels() const noexcept { return numChannels; }

    // EMA CPU load 0..1.
    float getCpuLoadAvg() const noexcept { return cpuLoadAvg.load (std::memory_order_relaxed); }

    // Reported plugin latency (samples), or 0 when empty / bypassed / broken --
    // those are skipped on the audio thread so they add no real delay.
    // Message-thread only: reads `current` without the lock, same contract as
    // getPlugin() (the message thread is the sole mutator of `current`).
    int getChainLatencySamples() const noexcept
    {
        if (current != nullptr
            && ! bypassed.load (std::memory_order_relaxed)
            && ! broken.load   (std::memory_order_relaxed))
            return juce::jmax (0, current->getLatencySamples());
        return 0;
    }

private:
    juce::SpinLock                              lock;
    std::unique_ptr<juce::AudioPluginInstance>  current;
    juce::AudioBuffer<float>                    scratch;
    juce::MidiBuffer                            dummyMidi;
    std::atomic<bool>                           bypassed { false };
    std::atomic<float>                          cpuLoadAvg { 0.0f };
    // Set when the plugin throws (C++ or NSException) during processBlock /
    // prepare / setPlugin.  Audio thread skips broken hosts entirely.
    std::atomic<bool>                           broken     { false };
    double                                      sampleRate = 48000.0;
    int                                         blockSize = 128;
    int                                         numChannels = 2;
    // The bus layout last requested via setPlugin().  prepare() re-applies it
    // on an engine restart so a multichannel AU isn't left re-prepared with a
    // default (often mono/stereo) layout -> silent / inert.
    juce::AudioChannelSet                       lastLayout = juce::AudioChannelSet::stereo();
};

} // namespace dcr

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
    void setPlugin (std::unique_ptr<juce::AudioPluginInstance> p,
                    const juce::AudioChannelSet& desiredLayout);

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

private:
    juce::SpinLock                              lock;
    std::unique_ptr<juce::AudioPluginInstance>  current;
    juce::AudioBuffer<float>                    scratch;
    juce::MidiBuffer                            dummyMidi;
    std::atomic<bool>                           bypassed { false };
    std::atomic<float>                          cpuLoadAvg { 0.0f };
    double                                      sampleRate = 48000.0;
    int                                         blockSize = 128;
    int                                         numChannels = 2;
};

} // namespace dcr

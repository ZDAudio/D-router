#pragma once

#include <AudioToolbox/AudioToolbox.h>
#include <vector>

namespace dcr
{

    // Streaming mono sample-rate converter built on Apple AudioConverter.
    // Push input samples; pull output samples on demand. Internal buffering keeps
    // any leftover input until the next pull.
    class SampleRateConverter
    {
    public:
        SampleRateConverter() = default;
        ~SampleRateConverter() { reset(); }

        SampleRateConverter (const SampleRateConverter&) = delete;
        SampleRateConverter& operator= (const SampleRateConverter&) = delete;

        // Configure or re-configure. Safe to call repeatedly.
        //  quality:    kAudioConverterQuality_* (Min..Max)
        //  complexity: kAudioConverterSampleRateConverterComplexity_* (Linear/Normal/Mastering/MinPhase)
        bool prepare (double inputSampleRate,
            double outputSampleRate,
            unsigned int quality,
            unsigned int complexity);

        // Append input samples to be consumed on subsequent pulls.
        void pushInput (const float* samples, int numSamples);

        // Produce up to numSamples output samples. Returns how many were produced.
        // Will produce as many as possible from the currently-buffered input.
        int pullOutput (float* dst, int numSamples);

        // Drop all buffered audio.
        void reset();

        double getInputSampleRate() const noexcept { return inRate; }
        double getOutputSampleRate() const noexcept { return outRate; }
        bool isBypass() const noexcept { return bypass; }

        // Internal filter latency in *output*-rate samples.  Zero when bypassed.
        int getOutputLatencySamples() const;

    private:
        static OSStatus inputProc (AudioConverterRef converter,
            UInt32* ioNumberDataPackets,
            AudioBufferList* ioData,
            AudioStreamPacketDescription** outDataPacketDescription,
            void* inUserData);

        AudioConverterRef converter = nullptr;
        double inRate = 0.0;
        double outRate = 0.0;
        bool bypass = false;

        // Pending input (mono, float32).
        std::vector<float> pending;
        size_t pendingPos = 0;

        // Scratch buffer the AudioConverter hands us packets from.
        std::vector<float> scratch;
    };

} // namespace dcr

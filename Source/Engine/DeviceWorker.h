#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "Engine/EngineSettings.h"
#include "Engine/RingBuffer.h"
#include "Engine/SampleRateConverter.h"

#include <memory>
#include <vector>

namespace dcr
{

    // Owns a single juce::AudioIODevice (input-only, output-only, or both).
    // On the audio thread:
    //   - on input: SRC each enabled input channel from deviceRate -> engineRate, push into inputRing[ch].
    //   - on output: pull engineRate samples from outputRing[ch], SRC -> deviceRate, write to device.
    // Ring buffers live in engine rate.
    class DeviceWorker : private juce::AudioIODeviceCallback
    {
    public:
        DeviceWorker (juce::AudioIODeviceType& type,
            const juce::String& deviceName,
            bool wantInput,
            bool wantOutput);
        ~DeviceWorker() override;

        bool open (const EngineSettings& settings);
        void close();

        bool isOpen() const noexcept { return device != nullptr && device->isOpen(); }
        juce::String getName() const { return device ? device->getName() : juce::String {}; }
        double getDeviceSampleRate() const noexcept { return device ? device->getCurrentSampleRate() : 0.0; }
        int getDeviceBufferSize() const noexcept { return device ? device->getCurrentBufferSizeSamples() : 0; }

        int getNumInputChannels() const noexcept { return numInputChannels; }
        int getNumOutputChannels() const noexcept { return numOutputChannels; }

        FloatRingBuffer* getInputRing (int ch) noexcept { return ch < (int) inputRings.size() ? &inputRings[(size_t) ch] : nullptr; }
        FloatRingBuffer* getOutputRing (int ch) noexcept { return ch < (int) outputRings.size() ? &outputRings[(size_t) ch] : nullptr; }

        juce::String getLastError() const { return lastError; }

        // Diagnostics (UI-thread safe).
        uint64_t getInputOverruns() const noexcept { return inputOverruns.load (std::memory_order_relaxed); }
        uint64_t getOutputUnderruns() const noexcept { return outputUnderruns.load (std::memory_order_relaxed); }

        // Hi-res ms timestamp of the most recent underrun, or 0 if none yet.
        double getLastUnderrunMs() const noexcept { return lastUnderrunMs.load (std::memory_order_relaxed); }

        // True once CoreAudio has renegotiated this device's sample rate or
        // buffer size out from under us (typically because another app — Music,
        // a browser, etc. — opened the same shared device and the OS switched
        // its nominal rate).  Our SRCs / rings were configured for the rate at
        // open() time, so a silent drift starts → crackling.  The engine polls
        // this from the message thread and does a preserve-state restart to
        // re-sync.  See audioDeviceAboutToStart().
        bool hasFormatChanged() const noexcept { return formatChanged.load (std::memory_order_acquire); }

        // Zero the diagnostic counters.  Called from the UI thread when the
        // user clicks "Reset" on the status panel; the audio callback may
        // race and re-increment, but that just means the new sample window
        // starts from ~0 instead of exactly 0.
        void resetXrunCounters() noexcept
        {
            inputOverruns.store (0, std::memory_order_relaxed);
            outputUnderruns.store (0, std::memory_order_relaxed);
            lastUnderrunMs.store (0.0, std::memory_order_relaxed);
        }

        // Hardware/driver latency in *device-rate* samples.
        int getDeviceInputLatencySamples() const noexcept;
        int getDeviceOutputLatencySamples() const noexcept;

        // Worst-case per-channel SRC latency (in samples) - taking max across
        // channels since they all share the same SR config.
        int getInputSrcLatencyEngineSamples() const;
        int getOutputSrcLatencyDeviceSamples() const;

        // Event the input callback signals after writing a fresh block, so the
        // matrix thread can wake immediately (set by AudioEngine before start).
        // Raw non-owning pointer; lifetime outlives every DeviceWorker (it lives
        // in the MatrixProcessor, which the engine destroys after the workers).
        void setInputReadyEvent (juce::WaitableEvent* e) noexcept { inputReadyEvent.store (e, std::memory_order_release); }

    private:
        void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
            int numInputChannelsInCallback,
            float* const* outputChannelData,
            int numOutputChannelsInCallback,
            int numSamples,
            const juce::AudioIODeviceCallbackContext& context) override;
        void audioDeviceAboutToStart (juce::AudioIODevice*) override;
        void audioDeviceStopped() override;
        void audioDeviceError (const juce::String& errorMessage) override;

        juce::AudioIODeviceType& deviceType;
        juce::String requestedName;
        bool wantsInput = false;
        bool wantsOutput = false;

        std::unique_ptr<juce::AudioIODevice> device;

        int numInputChannels = 0;
        int numOutputChannels = 0;
        double engineRate = 48000.0;
        int engineBlockSamples = 128;

        std::vector<FloatRingBuffer> inputRings;
        std::vector<FloatRingBuffer> outputRings;

        std::vector<std::unique_ptr<SampleRateConverter>> inputSRCs; // per input channel: device->engine
        std::vector<std::unique_ptr<SampleRateConverter>> outputSRCs; // per output channel: engine->device

        // Temp scratch (audio thread)
        std::vector<float> scratchEngine; // size ~ engineBlockSamples worst case
        std::vector<float> scratchEngineForOut; // engine-rate samples destined for output SRC
        std::vector<float> silenceScratch; // zeros, used when driver gives null channels

        juce::String lastError;

        std::atomic<uint64_t> inputOverruns { 0 };
        std::atomic<uint64_t> outputUnderruns { 0 };
        std::atomic<double> lastUnderrunMs { 0.0 };

        // Signalled (if set) at the end of the input half of the callback to wake
        // the matrix thread.  Non-owning; nullptr until AudioEngine wires it.
        std::atomic<juce::WaitableEvent*> inputReadyEvent { nullptr };

        // The device sample rate / buffer size we actually configured our SRCs
        // and rings for, captured at open().  audioDeviceAboutToStart compares
        // the device's live values against these to detect an OS-driven
        // renegotiation (another app changed the shared device's nominal rate).
        double configuredDeviceRate = 0.0;
        int configuredBufferSize = 0;
        std::atomic<bool> formatChanged { false };

        // Set true after the first audioDeviceIOCallback fires on the audio
        // thread.  The message thread can poll this from a UI timer to surface
        // "device opened but never delivered a callback" symptoms (a common
        // CoreAudio failure mode that otherwise looks like silent stall).
        std::atomic<bool> firstCallbackFired { false };

    public:
        bool hasFiredFirstCallback() const noexcept { return firstCallbackFired.load (std::memory_order_acquire); }
    };

} // namespace dcr

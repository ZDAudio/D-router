#pragma once

#include <juce_core/juce_core.h>

#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <memory>
#include <vector>

#include "Engine/EngineSettings.h"
#include "Engine/InputSource.h"
#include "Engine/RingBuffer.h"
#include "Engine/SampleRateConverter.h"

namespace dcr
{
    // One app-audio capture slot.  Mirrors DeviceWorker's outward surface so the
    // matrix can't tell them apart: getInputRing / getNumInputChannels /
    // setInputReadyEvent.  open() allocates the rings (the slot); attach()/detach()
    // bind/unbind the live process tap (spec §6).  The IOProc runs on a CoreAudio
    // HAL thread under the same RT rules as DeviceWorker's callback.
    class AppAudioWorker : public InputSource
    {
    public:
        AppAudioWorker (bool muteOriginalOutput, int numChannels);
        ~AppAudioWorker() override;

        // Allocate rings + scratch for `numChannels`.  Detached (no tap yet).
        bool open (const EngineSettings& settings);
        void close();

        // Bind a live process tap (message thread).  Reads the tap format, prepares
        // the per-channel SRC, builds the private aggregate device, starts the IOProc.
        bool attach (AudioObjectID processObject);
        void detach();
        bool isAppInput() const noexcept override { return true; }
        bool isAttached() const noexcept override { return attached.load (std::memory_order_acquire); }

        int getNumInputChannels() const noexcept { return numChannels; }
        FloatRingBuffer* getInputRing (int ch) noexcept override
        {
            return ch >= 0 && ch < (int) inputRings.size() ? &inputRings[(size_t) ch] : nullptr;
        }
        void setInputReadyEvent (juce::WaitableEvent* e) noexcept
        {
            inputReadyEvent.store (e, std::memory_order_release);
        }

        // The names of the OUTPUT devices D-Router itself is opening this session.
        // attach() anchors the tap's private aggregate to the system DEFAULT output
        // for clocking; if that default is a NON-built-in device D-Router also opens
        // (e.g. a Bluetooth out the user added), anchoring makes the tap aggregate and
        // D-Router both drive the same physical device -- two HAL clients on one
        // device corrupts the tap's capture clock (pitch + crackle on ALL app-tapped
        // audio).  Given this list, attach() drops the anchor in that case (tap-only
        // aggregate) so the two never share a device.  Set before the watcher attaches.
        void setRouterOutputDeviceNames (std::vector<juce::String> names)
        {
            routerOutputNames = std::move (names);
        }
        uint64_t getInputOverruns() const noexcept { return inputOverruns.load (std::memory_order_relaxed); }
        // Diagnostic: total engine-rate samples the IOProc has written to the rings.
        // 0 while detached or if the tap delivers no audio.
        uint64_t getFramesProduced() const noexcept { return framesProduced.load (std::memory_order_relaxed); }

    private:
        void ioBlock (const AudioBufferList* input, int numFrames); // HAL thread

        // C IOProc entry point (real-time I/O thread); forwards to ioBlock via
        // clientData == this.
        static OSStatus ioProcTrampoline (AudioObjectID, const AudioTimeStamp*, const AudioBufferList* inInputData, const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* clientData);

        const bool muteOriginalOutput;
        const int numChannels;
        EngineSettings settings {};

        std::vector<FloatRingBuffer> inputRings;
        std::vector<std::unique_ptr<SampleRateConverter>> inputSRCs;
        std::vector<float> scratchEngine;
        std::vector<float> deinterleave; // tap delivers interleaved stereo

        std::vector<juce::String> routerOutputNames; // D-Router's own outputs (anchor-conflict guard)

        std::atomic<juce::WaitableEvent*> inputReadyEvent { nullptr };
        std::atomic<bool> attached { false };
        std::atomic<uint64_t> inputOverruns { 0 };
        std::atomic<uint64_t> framesProduced { 0 };

        // CoreAudio handles (message thread owns lifecycle; HAL thread reads).
        AudioObjectID tapId = kAudioObjectUnknown;
        AudioObjectID aggregateId = kAudioObjectUnknown;
        AudioDeviceIOProcID procId = nullptr;
        void* captureQueue = nullptr; // dispatch_queue_t (CFBridgingRetain'd) for the IOProc block
        double tapSampleRate = 0.0;
    };
} // namespace dcr

#include "Engine/DeviceWorker.h"

#include "Engine/DeviceRateChoice.h"
#include "Engine/RingAutoSize.h"

#include <algorithm>
#include <cmath>
#include <new>
#include <vector>

namespace dcr
{

    DeviceWorker::DeviceWorker (juce::AudioIODeviceType& type,
        const juce::String& deviceName,
        bool wantInput,
        bool wantOutput)
        : deviceType (type),
          requestedName (deviceName),
          wantsInput (wantInput),
          wantsOutput (wantOutput)
    {
    }

    DeviceWorker::~DeviceWorker() { close(); }

    bool DeviceWorker::createDevice()
    {
        // MESSAGE THREAD ONLY (see header).  CoreAudioIODeviceType::createDevice()
        // reads the device-type's shared, non-thread-safe device arrays -- the same
        // ones JUCE rescans on the message thread on a hardware change.  Confining
        // this to the message thread is what serialises it against that scan.
        JUCE_ASSERT_MESSAGE_THREAD;
        close();
        const juce::String inName = wantsInput ? requestedName : juce::String {};
        const juce::String outName = wantsOutput ? requestedName : juce::String {};
        device.reset (deviceType.createDevice (outName, inName));
        if (device == nullptr)
        {
            lastError = "createDevice failed for " + requestedName;
            return false;
        }
        return true;
    }

    bool DeviceWorker::open (const EngineSettings& settings)
    {
        // The device must already exist -- createDevice() ran on the message thread.
        // open() (worker thread) never touches the shared deviceType, so it can't
        // race the message-thread device-list scan that used to corrupt the heap.
        if (device == nullptr)
        {
            lastError = "open() before createDevice() for " + requestedName;
            return false;
        }
        engineRate = settings.engineSampleRate;
        engineBlockSamples = settings.engineBlockSize;
        const double engineSampleRate = settings.engineSampleRate;
        const int engineBlockSize = settings.engineBlockSize;

        // Build channel bitmasks: all-on for whichever direction we want.
        auto inputChannels = device->getInputChannelNames();
        auto outputChannels = device->getOutputChannelNames();

        juce::BigInteger inMask, outMask;
        if (wantsInput && inputChannels.size() > 0)
            inMask.setRange (0, inputChannels.size(), true);
        if (wantsOutput && outputChannels.size() > 0)
            outMask.setRange (0, outputChannels.size(), true);

        // Pick the device's preferred buffer size near our engine block.
        auto bufferSizes = device->getAvailableBufferSizes();
        int chosenBuf = device->getDefaultBufferSize();
        if (!bufferSizes.isEmpty())
        {
            int best = bufferSizes[0];
            for (int b : bufferSizes)
                if (std::abs (b - engineBlockSize) < std::abs (best - engineBlockSize))
                    best = b;
            chosenBuf = best;
        }

        // Pick the device sample rate.  We ADOPT the device's current nominal
        // rate rather than forcing the engine rate, so we never yank a shared
        // device away from another app that just set it (e.g. a voice/comms app
        // starting a call) -- forcing it back triggered a restart tug-of-war
        // that broke the other app's stream and thrashed CoreAudio.  The
        // per-channel SRC bridges device rate <-> engine rate.  A freshly
        // created (not-yet-opened) CoreAudio device already reports the OS's
        // current nominal rate via getCurrentSampleRate().  See DeviceRateChoice.h.
        const auto availRatesArr = device->getAvailableSampleRates();
        const std::vector<double> availRates (availRatesArr.begin(), availRatesArr.end());
        const double currentDeviceRate = device->getCurrentSampleRate();
        const double chosenSr = chooseDeviceSampleRate (availRates, engineSampleRate, currentDeviceRate);

        auto err = device->open (inMask, outMask, chosenSr, chosenBuf);
        if (err.isNotEmpty())
        {
            lastError = err;
            // Keep the (created) wrapper alive: this is the transient "HAL still
            // releasing the physical device" failure the caller retries with a
            // bounded sleep (see AudioEngine::start).  Recreating the device would
            // need the message thread, so we re-attempt device->open() on the same
            // wrapper instead.  No rings were allocated yet (that's below), so
            // there's nothing to tear down here.
            return false;
        }

        // Use the channels the device ACTUALLY activated, not what we requested.
        // Virtual devices sometimes silently reduce channel count.
        numInputChannels = device->getActiveInputChannels().countNumberOfSetBits();
        numOutputChannels = device->getActiveOutputChannels().countNumberOfSetBits();
        const double devSr = device->getCurrentSampleRate();
        const int devBuf = device->getCurrentBufferSizeSamples();

        // Remember the format we configure SRCs/rings for, so audioDeviceAboutToStart
        // can detect an OS-driven renegotiation later and ask for a restart.
        configuredDeviceRate = devSr;
        configuredBufferSize = devBuf;
        formatChanged.store (false, std::memory_order_release);

        // Ring sizing.  Two modes:
        //   - fixed multipliers from settings (default), or
        //   - auto: derived per-device from the hardware latency THIS device
        //     reports (settings.autoRingSize) -- see RingAutoSize.h.
        // Per-channel cap so a misconfigured multiplier / huge rate ratio can't
        // request a gigabyte ring and crash the app with std::bad_alloc.
        static constexpr size_t kMaxRingSamplesPerChannel = 256 * 1024; // ~1 MB / ch
        size_t inRingSize, outRingSize;
        int effPrefillBlocks = settings.outputPreFillBlocks;
        if (settings.autoRingSize)
        {
            const int hwIn = device->getInputLatencyInSamples();
            const int hwOut = device->getOutputLatencyInSamples();
            const auto plan = dcr::ringauto::computeAutoRingPlan (
                engineBlockSize, engineSampleRate, devBuf, devSr, hwIn, hwOut);
            inRingSize = (size_t) plan.inRingSamples;
            outRingSize = (size_t) plan.outRingSamples;
            effPrefillBlocks = plan.prefillBlocks;
            juce::Logger::writeToLog ("DeviceWorker '" + requestedName
                                      + "': AUTO ring size from latency (hwIn=" + juce::String (hwIn)
                                      + " hwOut=" + juce::String (hwOut) + " spl @ " + juce::String ((int) devSr)
                                      + " Hz, buf=" + juce::String (devBuf) + ") -> in="
                                      + juce::String ((juce::int64) inRingSize) + " out="
                                      + juce::String ((juce::int64) outRingSize)
                                      + " spl, prefill=" + juce::String (effPrefillBlocks) + " blocks");
        }
        else
        {
            inRingSize = (size_t) std::max (
                settings.inputRingMultEng * engineBlockSize,
                (int) std::ceil ((double) settings.inputRingMultDev * devBuf * engineSampleRate / devSr));
            outRingSize = (size_t) std::max (
                settings.outputRingMultEng * engineBlockSize,
                (int) std::ceil ((double) settings.outputRingMultDev * devBuf * engineSampleRate / devSr));
        }
        inRingSize = std::min (inRingSize, kMaxRingSamplesPerChannel);
        outRingSize = std::min (outRingSize, kMaxRingSamplesPerChannel);

        try
        {
            inputRings.clear();
            inputRings.resize ((size_t) numInputChannels);
            for (auto& r : inputRings)
                r.resize (inRingSize);

            outputRings.clear();
            outputRings.resize ((size_t) numOutputChannels);
            for (auto& r : outputRings)
                r.resize (outRingSize);
        } catch (const std::bad_alloc&)
        {
            lastError = "Out of memory allocating ring buffers for '" + requestedName
                        + "' (in=" + juce::String ((juce::int64) inRingSize)
                        + " out=" + juce::String ((juce::int64) outRingSize)
                        + " spl x " + juce::String (numInputChannels + numOutputChannels) + " ch).  "
                        + "Lower ring multipliers in Settings.";
            close();
            return false;
        }

        inputSRCs.clear();
        for (int i = 0; i < numInputChannels; ++i)
        {
            auto src = std::make_unique<SampleRateConverter>();
            src->prepare (devSr, engineSampleRate, settings.srcQuality, settings.srcComplexity);
            inputSRCs.push_back (std::move (src));
        }
        outputSRCs.clear();
        for (int i = 0; i < numOutputChannels; ++i)
        {
            auto src = std::make_unique<SampleRateConverter>();
            src->prepare (engineSampleRate, devSr, settings.srcQuality, settings.srcComplexity);
            outputSRCs.push_back (std::move (src));
        }

        // Scratch sized to one device callback's worth of engine-rate samples,
        // plus 25% headroom for SRC overshoot. Per-worker, not per-channel.
        const int scratchSize = (int) std::ceil (1.25 * std::max (engineBlockSize, devBuf)
                                                 * engineSampleRate / devSr);
        scratchEngine.assign ((size_t) scratchSize, 0.0f);
        scratchEngineForOut.assign ((size_t) scratchSize, 0.0f);

        // Pre-size the input-padding silence buffer to the device buffer size so
        // the IO callback never has to grow it (a heap allocation on the audio
        // thread).  The callback keeps a >= guard as a fallback for the rare case
        // the OS hands us a larger block than it advertised.
        silenceScratch.assign ((size_t) juce::jmax (devBuf, engineBlockSize), 0.0f);

        // Pre-fill output rings with effPrefillBlocks engine blocks of silence
        // (clamped to ring capacity).  effPrefillBlocks is the auto-derived value
        // in auto mode, else settings.outputPreFillBlocks.
        const size_t preroll = juce::jmin ((size_t) (effPrefillBlocks * engineBlockSize),
            outRingSize);
        std::vector<float> silence (preroll, 0.0f);
        for (auto& r : outputRings)
            r.write (silence.data(), silence.size());

        // NOTE: the IO callback is NOT started here -- the engine calls startIO()
        // later, once the matrix thread is running.  Starting it here let input
        // accumulate in the rings during the rest of engine setup and overrun them
        // (a one-time "xrun in" burst at every restart).
        return true;
    }

    void DeviceWorker::startIO()
    {
        if (device != nullptr)
            device->start (this);
    }

    void DeviceWorker::close()
    {
        if (device != nullptr)
        {
            device->stop();
            device->close();
            device.reset();
        }
        numInputChannels = 0;
        numOutputChannels = 0;
        inputRings.clear();
        outputRings.clear();
        inputSRCs.clear();
        outputSRCs.clear();
    }

    int DeviceWorker::getDeviceInputLatencySamples() const noexcept
    {
        return (device != nullptr) ? device->getInputLatencyInSamples() : 0;
    }

    int DeviceWorker::getDeviceOutputLatencySamples() const noexcept
    {
        return (device != nullptr) ? device->getOutputLatencyInSamples() : 0;
    }

    int DeviceWorker::getInputSrcLatencyEngineSamples() const
    {
        int worst = 0;
        for (auto& s : inputSRCs)
            if (s)
                worst = std::max (worst, s->getOutputLatencySamples());
        return worst;
    }

    int DeviceWorker::getOutputSrcLatencyDeviceSamples() const
    {
        int worst = 0;
        for (auto& s : outputSRCs)
            if (s)
                worst = std::max (worst, s->getOutputLatencySamples());
        return worst;
    }

    void DeviceWorker::audioDeviceAboutToStart (juce::AudioIODevice* dev)
    {
        // CoreAudio calls this on the very first start (where the live format
        // matches what open() configured -> no-op) AND whenever it restarts the
        // stream after a format renegotiation -- e.g. another app opened this
        // shared device and the OS flipped its nominal sample rate.  In that
        // second case our SRCs/rings are still set for the OLD rate, so the
        // signal silently drifts (crackling).  Detect it and flag the engine
        // to do a preserve-state restart, which re-reads the new format.
        if (dev == nullptr || configuredDeviceRate <= 0.0)
            return;

        const double liveSr = dev->getCurrentSampleRate();
        const int liveBuf = dev->getCurrentBufferSizeSamples();
        const bool srMoved = std::abs (liveSr - configuredDeviceRate) > 1.0;
        const bool bufMoved = liveBuf > 0 && liveBuf != configuredBufferSize;

        if (srMoved || bufMoved)
        {
            formatChanged.store (true, std::memory_order_release);
            juce::Logger::writeToLog ("DeviceWorker '" + requestedName
                                      + "': format renegotiated by the OS (configured "
                                      + juce::String (configuredDeviceRate, 0) + " Hz / "
                                      + juce::String (configuredBufferSize) + " spl  ->  live "
                                      + juce::String (liveSr, 0) + " Hz / " + juce::String (liveBuf)
                                      + " spl).  Requesting preserve-state restart to re-sync SRC.");
        }
    }
    void DeviceWorker::audioDeviceStopped() {}
    void DeviceWorker::audioDeviceError (const juce::String& msg) { lastError = msg; }

    void DeviceWorker::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
        int numInputChannelsInCallback,
        float* const* outputChannelData,
        int numOutputChannelsInCallback,
        int numSamples,
        const juce::AudioIODeviceCallbackContext&)
    {
        // Mark this device as "alive" on its very first IO callback so the UI
        // can tell "device opened but driver never delivered samples" apart
        // from "device working but routing silent".  Single atomic store --
        // RT-safe.  The juce::Logger::writeToLog gets called from the message
        // thread once it notices this transitioned.
        firstCallbackFired.store (true, std::memory_order_release);
        // --- INPUT: device-rate samples -> SRC -> engine-rate -> inputRings ---
        // Important: feed EVERY input ring (even if the driver gave us null or
        // fewer channels), zero-padding when needed. Otherwise the MatrixProcessor
        // would wait forever for the empty rings to fill.
        if ((int) silenceScratch.size() < numSamples)
            silenceScratch.assign ((size_t) numSamples, 0.0f);

        for (int ch = 0; ch < numInputChannels; ++ch)
        {
            auto& src = *inputSRCs[(size_t) ch];
            const float* data = nullptr;
            if (ch < numInputChannelsInCallback && inputChannelData[ch] != nullptr)
                data = inputChannelData[ch];
            else
                data = silenceScratch.data();

            src.pushInput (data, numSamples);

            // Pull until SRC is drained.
            while (true)
            {
                int produced = src.pullOutput (scratchEngine.data(), (int) scratchEngine.size());
                if (produced <= 0)
                    break;
                const size_t written = inputRings[(size_t) ch].write (scratchEngine.data(), (size_t) produced);
                if (written < (size_t) produced)
                    inputOverruns.fetch_add (1, std::memory_order_relaxed);
                if (produced < (int) scratchEngine.size())
                    break;
            }
        }

        // Fresh input is now in the rings -- wake the matrix thread so it can
        // process this block immediately instead of waiting for its next poll
        // tick.  signal() on an already-signalled auto-reset event is a cheap
        // no-op, so duplicate signals (multiple input devices) just coalesce.
        if (numInputChannels > 0)
            if (auto* ev = inputReadyEvent.load (std::memory_order_acquire))
                ev->signal();

        // --- OUTPUT: outputRings -> SRC -> device-rate samples ---
        const int nOut = std::min (numOutputChannelsInCallback, numOutputChannels);
        for (int ch = 0; ch < nOut; ++ch)
        {
            if (outputChannelData[ch] == nullptr)
                continue;
            auto& src = *outputSRCs[(size_t) ch];

            // First try to pull from SRC (it may have leftovers).
            int produced = src.pullOutput (outputChannelData[ch], numSamples);
            int needed = numSamples - produced;
            while (needed > 0)
            {
                // Estimate how many engine-rate samples are needed to produce `needed` device-rate samples.
                const double ratio = src.getInputSampleRate() / src.getOutputSampleRate();
                int wantEng = (int) std::ceil (needed * ratio) + 16; // safety margin
                wantEng = std::min (wantEng, (int) scratchEngineForOut.size());
                size_t got = outputRings[(size_t) ch].read (scratchEngineForOut.data(), (size_t) wantEng);
                if (got == 0)
                {
                    outputUnderruns.fetch_add (1, std::memory_order_relaxed);
                    lastUnderrunMs.store (juce::Time::getMillisecondCounterHiRes(),
                        std::memory_order_relaxed);
                    std::fill (outputChannelData[ch] + produced,
                        outputChannelData[ch] + numSamples,
                        0.0f);
                    break;
                }
                src.pushInput (scratchEngineForOut.data(), (int) got);
                int p2 = src.pullOutput (outputChannelData[ch] + produced, needed);
                if (p2 <= 0)
                {
                    outputUnderruns.fetch_add (1, std::memory_order_relaxed);
                    lastUnderrunMs.store (juce::Time::getMillisecondCounterHiRes(),
                        std::memory_order_relaxed);
                    std::fill (outputChannelData[ch] + produced,
                        outputChannelData[ch] + numSamples,
                        0.0f);
                    break;
                }
                produced += p2;
                needed -= p2;
            }
        }

        // Zero any unused output channels.
        for (int ch = nOut; ch < numOutputChannelsInCallback; ++ch)
            if (outputChannelData[ch] != nullptr)
                std::fill (outputChannelData[ch], outputChannelData[ch] + numSamples, 0.0f);
    }

} // namespace dcr

#include "Engine/AudioEngine.h"

#include "DSP/Builtin/InternalPluginFormat.h"
#include "Engine/PdcPlan.h"

#include <limits>

namespace dcr
{

    AudioEngine::AudioEngine()
    {
        deviceType.reset (juce::AudioIODeviceType::createAudioIODeviceType_CoreAudio());
        if (deviceType != nullptr)
        {
            deviceType->scanForDevices();
            deviceType->addListener (this);
        }

        pluginFormatManager.addFormat (new juce::AudioUnitPluginFormat());
        // Built-in DSP modules (gain, filter, EQ, compressor) ride the same
        // format-manager pipeline as AUs, so they instantiate / serialize /
        // restore through identical code paths.
        pluginFormatManager.addFormat (new dcr::builtin::InternalPluginFormat());
    }

    AudioEngine::~AudioEngine()
    {
        if (deviceType != nullptr)
            deviceType->removeListener (this);
        stop();
    }

    void AudioEngine::audioDeviceListChanged()
    {
        if (deviceType != nullptr)
            deviceType->scanForDevices();
        if (onDeviceListChanged)
            onDeviceListChanged();
    }

    juce::StringArray AudioEngine::getAvailableInputDevices() const
    {
        if (deviceType == nullptr)
            return {};
        return deviceType->getDeviceNames (true);
    }
    juce::StringArray AudioEngine::getAvailableOutputDevices() const
    {
        if (deviceType == nullptr)
            return {};
        return deviceType->getDeviceNames (false);
    }

    juce::String AudioEngine::getDefaultInputDeviceName() const
    {
        if (deviceType == nullptr)
            return {};
        auto names = deviceType->getDeviceNames (true);
        int idx = deviceType->getDefaultDeviceIndex (true);
        return juce::isPositiveAndBelow (idx, names.size()) ? names[idx] : juce::String {};
    }

    juce::String AudioEngine::getDefaultOutputDeviceName() const
    {
        if (deviceType == nullptr)
            return {};
        auto names = deviceType->getDeviceNames (false);
        int idx = deviceType->getDefaultDeviceIndex (false);
        return juce::isPositiveAndBelow (idx, names.size()) ? names[idx] : juce::String {};
    }

    bool AudioEngine::isLikelyVirtualDevice (const juce::String& name)
    {
        const auto n = name.toLowerCase();
        // Common macOS virtual / loopback audio devices.  The device dialog uses
        // this only to DEFAULT the "no self-loop" checkbox -- the user can always
        // override it, so a missed/extra match is recoverable.
        static const char* patterns[] = {
            "blackhole", "loopback", "soundflower", "vb-cable", "vb-audio", "cable", "audio bridge", "dante", "soundsiphon", "sound siphon", "ishowu", "wavtap", "existential", "ground control", "audio hijack", "rogue amoeba", "multi-output", "aggregate", "virtual"
        };
        for (auto* p : patterns)
            if (n.contains (p))
                return true;
        return false;
    }

    bool AudioEngine::start (const std::vector<DeviceSpec>& devices)
    {
        stop();
        if (deviceType == nullptr)
            return false;
        deviceType->scanForDevices();

        // ---- Diagnostic preamble --------------------------------------------
        {
            juce::String line;
            line << "engine.start: " << juce::String ((int) devices.size())
                 << " device(s) requested, engine SR=" << (int) settings.engineSampleRate
                 << " blockSize=" << settings.engineBlockSize
                 << " preFill=" << settings.outputPreFillBlocks << " blocks"
                 << " ringInMult=" << settings.inputRingMultEng << "/" << settings.inputRingMultDev
                 << " ringOutMult=" << settings.outputRingMultEng << "/" << settings.outputRingMultDev;
            juce::Logger::writeToLog (line);
            for (const auto& s : devices)
                juce::Logger::writeToLog (juce::String ("  requested: ") + s.name
                                          + " in=" + (s.wantInput ? "Y" : "N")
                                          + " out=" + (s.wantOutput ? "Y" : "N"));
        }

        workers.clear();
        deviceInfo.clear();

        int totalIns = 0, totalOuts = 0;
        for (const auto& spec : devices)
        {
            auto w = std::make_unique<DeviceWorker> (*deviceType, spec.name, spec.wantInput, spec.wantOutput);
            if (!w->open (settings))
            {
                juce::Logger::writeToLog ("engine.start: device OPEN FAILED for '"
                                          + spec.name + "': " + w->getLastError());
                continue;
            }
            const int nIn = w->getNumInputChannels();
            const int nOut = w->getNumOutputChannels();
            juce::Logger::writeToLog ("engine.start: device opened '" + spec.name
                                      + "'  SR=" + juce::String ((int) w->getDeviceSampleRate())
                                      + " buf=" + juce::String (w->getDeviceBufferSize())
                                      + " in=" + juce::String (nIn)
                                      + " out=" + juce::String (nOut));
            DeviceInfo info;
            info.name = w->getName();
            info.deviceSampleRate = w->getDeviceSampleRate();
            info.numInputChannels = nIn;
            info.numOutputChannels = nOut;
            info.blockSelfLoop = spec.blockSelfLoop;
            if (info.numInputChannels > 0)
            {
                info.globalInputBase = totalIns;
                totalIns += info.numInputChannels;
            }
            if (info.numOutputChannels > 0)
            {
                info.globalOutputBase = totalOuts;
                totalOuts += info.numOutputChannels;
            }
            deviceInfo.push_back (info);
            workers.push_back (std::move (w));
        }

        if (totalIns == 0 || totalOuts == 0)
        {
            juce::Logger::writeToLog ("engine.start: ABORTED -- need at least 1 input AND 1 output channel"
                                      " (got "
                                      + juce::String (totalIns) + " in / "
                                      + juce::String (totalOuts) + " out)");
            return false;
        }
        juce::Logger::writeToLog ("engine.start: matrix " + juce::String (totalIns)
                                  + " in x " + juce::String (totalOuts) + " out, processor starting");

        matrix.resize (totalIns, totalOuts);

        // Block self-loop crosspoints for virtual/loopback devices: this device's
        // input ch N must not feed its OWN output ch N (instant feedback).  resize()
        // cleared all blocks, so re-apply here from the device topology.
        for (const auto& d : deviceInfo)
        {
            if (!d.blockSelfLoop)
                continue;
            if (d.globalInputBase < 0 || d.globalOutputBase < 0)
                continue;
            const int n = juce::jmin (d.numInputChannels, d.numOutputChannels);
            for (int c = 0; c < n; ++c)
                matrix.setBlocked (d.globalOutputBase + c, d.globalInputBase + c, true);
            if (n > 0)
                juce::Logger::writeToLog ("engine.start: blocked " + juce::String (n)
                                          + " self-loop crosspoint(s) on virtual device '" + d.name + "'");
        }

        // Create per-output and per-input plugin hosts.
        pluginHosts.clear();
        pluginHosts.reserve ((size_t) totalOuts);
        for (int i = 0; i < totalOuts; ++i)
        {
            auto ph = std::make_unique<PluginHost>();
            ph->prepare (settings.engineSampleRate, settings.engineBlockSize);
            pluginHosts.push_back (std::move (ph));
        }
        inputPluginHosts.clear();
        inputPluginHosts.reserve ((size_t) totalIns);
        for (int i = 0; i < totalIns; ++i)
        {
            auto ph = std::make_unique<PluginHost>();
            ph->prepare (settings.engineSampleRate, settings.engineBlockSize);
            inputPluginHosts.push_back (std::move (ph));
        }

        // Tell the group managers about the new channel counts, and re-prepare
        // every plugin slot of every group on both sides.
        groupManager.setNumOutputChannels (totalOuts);
        inputGroupManager.setNumInputChannels (totalIns);
        for (int gi = 0; gi < groupManager.getNumGroups(); ++gi)
        {
            if (auto* g = groupManager.getGroup (gi))
                for (auto& slot : g->pluginSlots)
                    if (slot)
                        slot->prepare (settings.engineSampleRate,
                            settings.engineBlockSize,
                            g->channelSet.size());
        }
        for (int gi = 0; gi < inputGroupManager.getNumGroups(); ++gi)
        {
            if (auto* g = inputGroupManager.getGroup (gi))
                for (auto& slot : g->pluginSlots)
                    if (slot)
                        slot->prepare (settings.engineSampleRate,
                            settings.engineBlockSize,
                            g->channelSet.size());
        }

        std::vector<MatrixProcessor::GlobalInput> globalIns;
        std::vector<MatrixProcessor::GlobalOutput> globalOuts;
        globalIns.reserve ((size_t) totalIns);
        globalOuts.reserve ((size_t) totalOuts);
        int outIdx = 0, inIdx = 0;
        for (size_t i = 0; i < workers.size(); ++i)
        {
            auto* w = workers[i].get();
            for (int ch = 0; ch < w->getNumInputChannels(); ++ch)
            {
                globalIns.push_back ({ w, ch, inputPluginHosts[(size_t) inIdx].get() });
                ++inIdx;
            }
            for (int ch = 0; ch < w->getNumOutputChannels(); ++ch)
            {
                globalOuts.push_back ({ w, ch, pluginHosts[(size_t) outIdx].get() });
                ++outIdx;
            }
        }
        processor.configure (std::move (globalIns), std::move (globalOuts), &matrix, &groupManager, &inputGroupManager, settings);

        // Wire each device's input callback to wake the matrix thread directly
        // (event-driven) instead of leaving it to sleep-poll.  The event lives in
        // `processor`, a member that outlives the workers: stop() halts the matrix
        // thread then clears `workers` while `processor` is still alive, and
        // ~AudioEngine calls stop() before any member is destroyed.  A callback
        // that fires during teardown therefore signals a still-valid event (and
        // signalling an auto-reset event with no waiter is a harmless no-op).
        for (auto& w : workers)
            w->setInputReadyEvent (&processor.getInputReadyEvent());

        processor.start();
        runningFlag = true;

        // Initial PDC plan now the matrix knows its output count (a no-op when PDC
        // is off or no plugin reports latency).  The status timer keeps it current
        // thereafter, and plugin/toggle changes replan on demand.
        replanPdc();

        return true;
    }

    void AudioEngine::stop()
    {
        if (!runningFlag)
            return;
        juce::Logger::writeToLog ("engine.stop: tearing down " + juce::String ((int) workers.size())
                                  + " worker(s), " + juce::String ((juce::int64) processor.getBlocksProcessed())
                                  + " blocks processed lifetime, "
                                  + juce::String ((juce::int64) processor.getBlocksStalled()) + " stalled");
        processor.stop();
        pluginHosts.clear();
        inputPluginHosts.clear();
        workers.clear();
        deviceInfo.clear();
        runningFlag = false;
    }

    size_t AudioEngine::getInputRingFill (int globalCh) const
    {
        int acc = 0;
        for (auto& w : workers)
        {
            for (int c = 0; c < w->getNumInputChannels(); ++c, ++acc)
            {
                if (acc == globalCh)
                {
                    auto* ring = const_cast<DeviceWorker&> (*w).getInputRing (c);
                    return ring ? ring->readAvailable() : 0;
                }
            }
        }
        return 0;
    }

    uint64_t AudioEngine::getTotalInputOverruns() const noexcept
    {
        uint64_t s = 0;
        for (auto& w : workers)
            s += w->getInputOverruns();
        return s;
    }

    uint64_t AudioEngine::getTotalOutputUnderruns() const noexcept
    {
        uint64_t s = 0;
        for (auto& w : workers)
            s += w->getOutputUnderruns();
        return s;
    }

    double AudioEngine::getMostRecentUnderrunMs() const noexcept
    {
        double m = 0.0;
        for (auto& w : workers)
            m = juce::jmax (m, w->getLastUnderrunMs());
        return m;
    }

    bool AudioEngine::anyDeviceFormatChanged() const noexcept
    {
        for (auto& w : workers)
            if (w->hasFormatChanged())
                return true;
        return false;
    }

    void AudioEngine::resetXrunCounters() noexcept
    {
        for (auto& w : workers)
            w->resetXrunCounters();
    }

    void AudioEngine::stopProcessor() noexcept
    {
        // Just halt the matrix-processing thread.  Workers, plugin hosts, and
        // device callbacks stay alive -- needed so MainComponent's shutdown
        // path can still call plugin->getStateInformation() safely without a
        // concurrent processBlock racing the read.
        processor.stop();
    }

    double AudioEngine::LatencyReport::DeviceItem::getInputLatencyMs (double engineSr) const
    {
        if (!hasInput || deviceSampleRate <= 0.0 || engineSr <= 0.0)
            return 0.0;
        const double devMs = 1000.0 * hwInputSamples / deviceSampleRate;
        const double srcMs = 1000.0 * srcInLatencyEng / engineSr;
        return devMs + srcMs;
    }

    double AudioEngine::LatencyReport::DeviceItem::getOutputLatencyMs (double engineSr) const
    {
        if (!hasOutput || deviceSampleRate <= 0.0 || engineSr <= 0.0)
            return 0.0;
        const double devMs = 1000.0 * hwOutputSamples / deviceSampleRate;
        const double srcMs = 1000.0 * srcOutLatencyDev / deviceSampleRate;
        return devMs + srcMs;
    }

    double AudioEngine::LatencyReport::getEngineContributionMs() const
    {
        if (engineSampleRate <= 0.0)
            return 0.0;
        const int blocks = 1 + outputPreFillBlocks;
        return 1000.0 * blocks * engineBlockSize / engineSampleRate;
    }

    double AudioEngine::LatencyReport::getPluginLatencyMsWorst() const
    {
        if (engineSampleRate <= 0.0)
            return 0.0;
        return 1000.0 * pluginMaxLatencyEng / engineSampleRate;
    }

    double AudioEngine::LatencyReport::getRoundTripMsWorst() const
    {
        double inMax = 0.0, outMax = 0.0;
        for (auto& d : devices)
        {
            inMax = juce::jmax (inMax, d.getInputLatencyMs (engineSampleRate));
            outMax = juce::jmax (outMax, d.getOutputLatencyMs (engineSampleRate));
        }
        // Plugin latency sits in the engine domain, upstream of the device output
        // path, so it adds to the worst output rather than overlapping it.
        return inMax + getEngineContributionMs() + getPluginLatencyMsWorst() + outMax;
    }

    std::vector<AudioEngine::DeviceLiveness> AudioEngine::getDeviceLiveness() const
    {
        std::vector<DeviceLiveness> out;
        out.reserve (workers.size());
        for (auto& w : workers)
        {
            DeviceLiveness l;
            l.name = w->getName();
            l.firstCallbackFired = w->hasFiredFirstCallback();
            l.hasInput = w->getNumInputChannels() > 0;
            l.hasOutput = w->getNumOutputChannels() > 0;
            out.push_back (std::move (l));
        }
        return out;
    }

    AudioEngine::LatencyReport AudioEngine::getLatencyReport() const
    {
        LatencyReport r;
        r.engineSampleRate = settings.engineSampleRate;
        r.engineBlockSize = settings.engineBlockSize;
        r.outputPreFillBlocks = settings.outputPreFillBlocks;

        for (auto& w : workers)
        {
            LatencyReport::DeviceItem item;
            item.name = w->getName();
            item.deviceSampleRate = w->getDeviceSampleRate();
            item.devBufSamples = w->getDeviceBufferSize();
            item.hasInput = w->getNumInputChannels() > 0;
            item.hasOutput = w->getNumOutputChannels() > 0;
            item.hwInputSamples = item.hasInput ? w->getDeviceInputLatencySamples() : 0;
            item.hwOutputSamples = item.hasOutput ? w->getDeviceOutputLatencySamples() : 0;
            item.srcInLatencyEng = item.hasInput ? w->getInputSrcLatencyEngineSamples() : 0;
            item.srcOutLatencyDev = item.hasOutput ? w->getOutputSrcLatencyDeviceSamples() : 0;
            r.devices.push_back (item);
        }

        // Fold in plugin delay: the worst per-output plugin-chain latency across
        // all outputs.  The slowest output already incurs this today; surfacing it
        // is the "minimum viable" half of PDC (alignment of the rest follows).
        const auto perOut = computePerOutputPluginLatencySamples();
        int maxPlug = 0;
        for (int v : perOut)
            maxPlug = juce::jmax (maxPlug, v);
        r.pluginMaxLatencyEng = maxPlug;

        return r;
    }

    std::vector<int> AudioEngine::computePerOutputPluginLatencySamples() const
    {
        std::vector<int> lat (pluginHosts.size(), 0);
        for (size_t o = 0; o < pluginHosts.size(); ++o)
            if (pluginHosts[o] != nullptr)
                lat[o] = pluginHosts[o]->getChainLatencySamples();
        // Fold in each output's group-insert latency (attributed to its members).
        groupManager.addGroupInsertLatencySamples (lat);
        return lat;
    }

    void AudioEngine::setPdcEnabled (bool enabled)
    {
        settings.pdcEnabled = enabled;
        replanPdc();
    }

    void AudioEngine::replanPdc()
    {
        const auto perOut = computePerOutputPluginLatencySamples();
        const auto plan = computePdcPlan (perOut, settings.pdcEnabled, kMaxPdcSamples);
        if (plan.clamped)
            juce::Logger::writeToLog ("[PDC] an output's plugin latency exceeded the "
                                      + juce::String (kMaxPdcSamples) + "-sample cap -- clamped");
        processor.setPdcTargets (plan.compDelay);
    }

    size_t AudioEngine::getOutputRingFill (int globalCh) const
    {
        int acc = 0;
        for (auto& w : workers)
        {
            for (int c = 0; c < w->getNumOutputChannels(); ++c, ++acc)
            {
                if (acc == globalCh)
                {
                    auto* ring = const_cast<DeviceWorker&> (*w).getOutputRing (c);
                    return ring ? ring->readAvailable() : 0;
                }
            }
        }
        return 0;
    }

    float AudioEngine::getOutputRingFillFraction (int globalCh) const
    {
        int acc = 0;
        for (auto& w : workers)
        {
            for (int c = 0; c < w->getNumOutputChannels(); ++c, ++acc)
            {
                if (acc == globalCh)
                {
                    auto* ring = const_cast<DeviceWorker&> (*w).getOutputRing (c);
                    if (ring == nullptr)
                        return 0.0f;
                    const auto cap = ring->capacity();
                    return cap > 0 ? (float) ring->readAvailable() / (float) cap : 0.0f;
                }
            }
        }
        return 0.0f;
    }

    float AudioEngine::getMinOutputRingFillFraction() const
    {
        float minFrac = 1.0f;
        bool any = false;
        int globalCh = 0;
        for (auto& w : workers)
        {
            for (int c = 0; c < w->getNumOutputChannels(); ++c, ++globalCh)
            {
                auto* ring = const_cast<DeviceWorker&> (*w).getOutputRing (c);
                if (ring == nullptr)
                    continue;
                const auto cap = ring->capacity();
                if (cap == 0)
                    continue;
                const float f = (float) ring->readAvailable() / (float) cap;
                if (!any || f < minFrac)
                {
                    minFrac = f;
                    any = true;
                }
            }
        }
        return any ? minFrac : 0.0f;
    }

    double AudioEngine::getMinOutputRingHeadroomMs() const
    {
        double minMs = std::numeric_limits<double>::infinity();
        bool any = false;
        for (auto& w : workers)
        {
            const double sr = w->getDeviceSampleRate();
            if (sr <= 0.0)
                continue;
            for (int c = 0; c < w->getNumOutputChannels(); ++c)
            {
                auto* ring = const_cast<DeviceWorker&> (*w).getOutputRing (c);
                if (ring == nullptr)
                    continue;
                const double ms = 1000.0 * (double) ring->readAvailable() / sr;
                if (!any || ms < minMs)
                {
                    minMs = ms;
                    any = true;
                }
            }
        }
        return any ? minMs : 0.0;
    }

} // namespace dcr

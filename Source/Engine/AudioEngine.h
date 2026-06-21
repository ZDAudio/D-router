#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "DSP/PluginHost.h"
#include "Engine/DeviceWorker.h"
#include "Engine/EngineSettings.h"
#include "Engine/MatrixProcessor.h"
#include "Routing/InputGroupManager.h"
#include "Routing/OutputGroupManager.h"
#include "Routing/RoutingMatrix.h"

#include <memory>
#include <vector>

namespace dcr
{

    // Top-level audio orchestration:
    //   - holds a CoreAudio AudioIODeviceType
    //   - opens N DeviceWorkers
    //   - builds the global input/output channel lists
    //   - runs the MatrixProcessor
    class AudioEngine : private juce::AudioIODeviceType::Listener
    {
    public:
        AudioEngine();
        ~AudioEngine() override;

        // Called on the message thread when the set of available devices changes
        // (hotplug). Host should re-check getDeviceInfo() and decide what to do.
        std::function<void()> onDeviceListChanged;

        // All tunables are now in EngineSettings; engine restart picks up changes.
        void setSettings (const EngineSettings& s) { settings = s; }
        const EngineSettings& getSettings() const noexcept { return settings; }

        // Plugin delay compensation: realign every output behind the slowest plugin
        // chain so a latent plugin on one output stays phase-aligned with the rest.
        // Live toggle -- no engine restart; replans immediately.  Persisted in
        // EngineSettings (the caller saves settings).
        void setPdcEnabled (bool enabled);
        bool isPdcEnabled() const noexcept { return settings.pdcEnabled; }

        // Recompute the PDC plan from current plugin latencies + the enable flag and
        // publish per-output compensation delays to the matrix processor.  Message-
        // thread only; cheap and idempotent (republishing an unchanged plan is a
        // no-op on the audio thread), so the host's status timer calls it as a
        // backstop to pick up plugin load/bypass/group-membership changes.
        void replanPdc();

        double getEngineSampleRate() const noexcept { return settings.engineSampleRate; }
        int getEngineBlockSize() const noexcept { return settings.engineBlockSize; }

        juce::StringArray getAvailableInputDevices() const;
        juce::StringArray getAvailableOutputDevices() const;

        // CoreAudio system default devices.
        juce::String getDefaultInputDeviceName() const;
        juce::String getDefaultOutputDeviceName() const;

        // Spec for one device participating in routing.
        struct DeviceSpec
        {
            juce::String name;
            bool wantInput = false;
            bool wantOutput = false;
            // Virtual loopback / audio-bridge devices feed their output straight
            // back to their input.
            // Routing this device's input ch N to its OWN output ch N then makes
            // a runaway feedback loop.  When this is set, the engine blocks those
            // same-device / same-channel crosspoints in the matrix.  Defaults to
            // true for devices the dialog detects as virtual.
            bool blockSelfLoop = false;
        };

        // Heuristic: does this device name look like a virtual loopback device?
        // Used to default blockSelfLoop on in the device dialog.
        static bool isLikelyVirtualDevice (const juce::String& name);

        // (Re)start the engine with the given devices. Returns true if all opened.
        bool start (const std::vector<DeviceSpec>& devices);

        void stop();

        RoutingMatrix& getRoutingMatrix() noexcept { return matrix; }
        const RoutingMatrix& getRoutingMatrix() const noexcept { return matrix; }

        // Per-device info available after start.
        struct DeviceInfo
        {
            juce::String name;
            int globalInputBase = -1;
            int numInputChannels = 0;
            int globalOutputBase = -1;
            int numOutputChannels = 0;
            double deviceSampleRate = 0.0;
            bool blockSelfLoop = false; // carried from the DeviceSpec
        };
        const std::vector<DeviceInfo>& getDeviceInfo() const noexcept { return deviceInfo; }

        // Diagnostics
        uint64_t getMatrixBlocksProcessed() const noexcept { return processor.getBlocksProcessed(); }
        uint64_t getMatrixBlocksStalled() const noexcept { return processor.getBlocksStalled(); }
        uint64_t getMatrixOutputDrops() const noexcept { return processor.getOutputDrops(); }

        // Master output fade for click-free restarts -- ramps the whole output bus
        // without touching per-channel trims.  1.0 = unity.
        void setOutputMasterGain (float g) noexcept { processor.setMasterGainTarget (g); }
        size_t getInputRingFill (int globalCh) const; // returns sample count available
        size_t getOutputRingFill (int globalCh) const;
        // Output ring fill as a fraction 0..1 of the ring's usable capacity.
        // The actual leading indicator of "about to xrun": when this drops
        // near zero, the matrix thread can't keep up with the device callback.
        float getOutputRingFillFraction (int globalCh) const;
        // Minimum fill fraction across every output channel -- worst-case
        // headroom in one number.  0.0 == about to underrun.
        float getMinOutputRingFillFraction() const;
        // Minimum output ring fill expressed as MILLISECONDS of audio at the
        // device's own sample rate.  This is the actionable metric -- it tells
        // the user "you have N ms of safety before the next xrun if matrix
        // stops producing right now".  Independent of ring capacity, so it
        // doesn't get spuriously larger when the user shrinks the buffers.
        double getMinOutputRingHeadroomMs() const;
        float getInputPeak (int globalCh) const noexcept { return processor.getInputPeak (globalCh); }
        float getOutputPeak (int globalCh) const noexcept { return processor.getOutputPeak (globalCh); }

        // Per-output-channel plugin slots. Returns nullptr if out of range or engine stopped.
        PluginHost* getPluginHost (int globalOutputCh) noexcept
        {
            if (globalOutputCh < 0 || globalOutputCh >= (int) pluginHosts.size())
                return nullptr;
            return pluginHosts[(size_t) globalOutputCh].get();
        }
        PluginHost* getInputPluginHost (int globalInputCh) noexcept
        {
            if (globalInputCh < 0 || globalInputCh >= (int) inputPluginHosts.size())
                return nullptr;
            return inputPluginHosts[(size_t) globalInputCh].get();
        }
        int getNumPluginHosts() const noexcept { return (int) pluginHosts.size(); }
        int getNumInputPluginHosts() const noexcept { return (int) inputPluginHosts.size(); }

        juce::AudioPluginFormatManager& getPluginFormatManager() { return pluginFormatManager; }
        juce::KnownPluginList& getKnownPluginList() { return knownPlugins; }

        OutputGroupManager& getGroupManager() noexcept { return groupManager; }
        const OutputGroupManager& getGroupManager() const noexcept { return groupManager; }
        InputGroupManager& getInputGroupManager() noexcept { return inputGroupManager; }
        const InputGroupManager& getInputGroupManager() const noexcept { return inputGroupManager; }

        // Sum of overruns / underruns across all open devices.
        uint64_t getTotalInputOverruns() const noexcept;
        uint64_t getTotalOutputUnderruns() const noexcept;
        // Most recent underrun hi-res ms timestamp (0 if none).
        double getMostRecentUnderrunMs() const noexcept;

        // True if ANY open device's sample rate / buffer size was renegotiated
        // by the OS after we opened it (another app grabbing the shared device).
        // The host polls this and does a preserve-state restart to re-sync.
        bool anyDeviceFormatChanged() const noexcept;
        // Zero every device worker's xrun / underrun / lastUnderrun counters.
        // Useful as a UI button: lets the user clear cold-start xruns once the
        // engine has stabilised so the gauge reflects only live problems.
        void resetXrunCounters() noexcept;

        // Halt the matrix processor thread (so no more processBlock calls fire)
        // WITHOUT destroying pluginHosts / workers / deviceInfo.  Use this when
        // you need to safely read every plugin's getStateInformation() (e.g.
        // for snapshot save on shutdown) before the full engine.stop() tears
        // the hosts down.
        void stopProcessor() noexcept;

        // CPU load (0..1) from MatrixProcessor.
        float getCpuLoadAvg() const noexcept { return processor.getCpuLoadAvg(); }
        float getCpuLoadPeak() const noexcept { return processor.getCpuLoadPeak(); }
        void resetCpuPeak() noexcept { processor.resetCpuPeak(); }

        // Latency report (all values converted to MILLISECONDS based on the engine SR).
        struct LatencyReport
        {
            struct DeviceItem
            {
                juce::String name;
                double deviceSampleRate = 0.0;
                int devBufSamples = 0;
                int hwInputSamples = 0; // at device rate
                int hwOutputSamples = 0; // at device rate
                int srcInLatencyEng = 0; // at engine rate
                int srcOutLatencyDev = 0; // at device rate
                bool hasInput = false;
                bool hasOutput = false;

                double getInputLatencyMs (double engineSr) const;
                double getOutputLatencyMs (double engineSr) const;
            };
            std::vector<DeviceItem> devices;
            double engineSampleRate = 48000.0;
            int engineBlockSize = 128;
            int outputPreFillBlocks = 8;

            // Worst per-output plugin-chain latency (per-output inserts + the group
            // insert on that output), in ENGINE samples (plugins run in the engine
            // clock domain).  0 when no output carries a latent plugin.  This is the
            // latency the slowest output already incurs today; PDC (when enabled, a
            // later change) raises the OTHER outputs to match it rather than adding
            // anything beyond it.
            int pluginMaxLatencyEng = 0;

            // Pipeline-only contribution: 1 engine block (matrix waits) +
            // pre-fill (static cushion).
            double getEngineContributionMs() const;

            // Worst-output plugin latency as milliseconds at the engine rate.
            double getPluginLatencyMsWorst() const;

            // Worst-case round-trip = max(input device latency) + engine path
            // + worst-output plugin latency + max(output device latency).
            double getRoundTripMsWorst() const;
        };

        LatencyReport getLatencyReport() const;

        // Per-device "have I seen at least one IO callback yet?" snapshot --
        // lets the PerfMonitor (and future UI) surface devices that opened
        // successfully but whose driver never delivered samples (silent
        // CoreAudio failure mode).
        struct DeviceLiveness
        {
            juce::String name;
            bool firstCallbackFired = false;
            bool hasInput = false;
            bool hasOutput = false;
        };
        std::vector<DeviceLiveness> getDeviceLiveness() const;

    private:
        void audioDeviceListChanged() override;

        // Per global output channel: that channel's per-output plugin-chain latency
        // plus the insert latency of the group it belongs to (samples, engine rate).
        // Message-thread only.  Shared by the latency report and the PDC planner so
        // both agree on each output's total plugin latency.
        std::vector<int> computePerOutputPluginLatencySamples() const;

        std::unique_ptr<juce::AudioIODeviceType> deviceType;
        EngineSettings settings;

        std::vector<std::unique_ptr<DeviceWorker>> workers;
        std::vector<std::unique_ptr<PluginHost>> pluginHosts; // one per output channel
        std::vector<std::unique_ptr<PluginHost>> inputPluginHosts; // one per input channel
        std::vector<DeviceInfo> deviceInfo;
        RoutingMatrix matrix;
        OutputGroupManager groupManager;
        InputGroupManager inputGroupManager;
        MatrixProcessor processor;
        juce::AudioPluginFormatManager pluginFormatManager;
        juce::KnownPluginList knownPlugins;
        bool runningFlag = false;
    };

} // namespace dcr

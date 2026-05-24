#pragma once

#include <juce_audio_devices/juce_audio_devices.h>

#include "Engine/DeviceWorker.h"
#include "Engine/EngineSettings.h"
#include "Engine/MatrixProcessor.h"
#include "Routing/RoutingMatrix.h"
#include "Routing/OutputGroupManager.h"
#include "Routing/InputGroupManager.h"
#include "DSP/PluginHost.h"

#include <memory>
#include <vector>

namespace dcr {

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

    double getEngineSampleRate() const noexcept { return settings.engineSampleRate; }
    int    getEngineBlockSize()  const noexcept { return settings.engineBlockSize;  }

    juce::StringArray getAvailableInputDevices()  const;
    juce::StringArray getAvailableOutputDevices() const;

    // CoreAudio system default devices.
    juce::String getDefaultInputDeviceName()  const;
    juce::String getDefaultOutputDeviceName() const;

    // Spec for one device participating in routing.
    struct DeviceSpec
    {
        juce::String name;
        bool wantInput  = false;
        bool wantOutput = false;
    };

    // (Re)start the engine with the given devices. Returns true if all opened.
    bool start (const std::vector<DeviceSpec>& devices);

    void stop();

    RoutingMatrix&       getRoutingMatrix()       noexcept { return matrix; }
    const RoutingMatrix& getRoutingMatrix() const noexcept { return matrix; }

    // Per-device info available after start.
    struct DeviceInfo
    {
        juce::String name;
        int globalInputBase  = -1;
        int numInputChannels  = 0;
        int globalOutputBase = -1;
        int numOutputChannels = 0;
        double deviceSampleRate = 0.0;
    };
    const std::vector<DeviceInfo>& getDeviceInfo() const noexcept { return deviceInfo; }

    // Diagnostics
    uint64_t getMatrixBlocksProcessed() const noexcept { return processor.getBlocksProcessed(); }
    uint64_t getMatrixBlocksStalled()   const noexcept { return processor.getBlocksStalled();   }
    size_t   getInputRingFill  (int globalCh) const;  // returns sample count available
    size_t   getOutputRingFill (int globalCh) const;
    float    getInputPeak  (int globalCh) const noexcept { return processor.getInputPeak  (globalCh); }
    float    getOutputPeak (int globalCh) const noexcept { return processor.getOutputPeak (globalCh); }

    // Per-output-channel plugin slots. Returns nullptr if out of range or engine stopped.
    PluginHost* getPluginHost (int globalOutputCh) noexcept
    {
        if (globalOutputCh < 0 || globalOutputCh >= (int) pluginHosts.size()) return nullptr;
        return pluginHosts[(size_t) globalOutputCh].get();
    }
    PluginHost* getInputPluginHost (int globalInputCh) noexcept
    {
        if (globalInputCh < 0 || globalInputCh >= (int) inputPluginHosts.size()) return nullptr;
        return inputPluginHosts[(size_t) globalInputCh].get();
    }
    int getNumPluginHosts() const noexcept { return (int) pluginHosts.size(); }
    int getNumInputPluginHosts() const noexcept { return (int) inputPluginHosts.size(); }

    juce::AudioPluginFormatManager& getPluginFormatManager() { return pluginFormatManager; }
    juce::KnownPluginList&          getKnownPluginList()     { return knownPlugins; }

    OutputGroupManager&       getGroupManager()       noexcept { return groupManager; }
    const OutputGroupManager& getGroupManager() const noexcept { return groupManager; }
    InputGroupManager&        getInputGroupManager()       noexcept { return inputGroupManager; }
    const InputGroupManager&  getInputGroupManager() const noexcept { return inputGroupManager; }

    // Sum of overruns / underruns across all open devices.
    uint64_t getTotalInputOverruns()   const noexcept;
    uint64_t getTotalOutputUnderruns() const noexcept;
    // Most recent underrun hi-res ms timestamp (0 if none).
    double   getMostRecentUnderrunMs() const noexcept;

    // CPU load (0..1) from MatrixProcessor.
    float    getCpuLoadAvg()  const noexcept { return processor.getCpuLoadAvg();  }
    float    getCpuLoadPeak() const noexcept { return processor.getCpuLoadPeak(); }
    void     resetCpuPeak()         noexcept { processor.resetCpuPeak(); }

    // Latency report (all values converted to MILLISECONDS based on the engine SR).
    struct LatencyReport
    {
        struct DeviceItem
        {
            juce::String name;
            double  deviceSampleRate = 0.0;
            int     devBufSamples    = 0;
            int     hwInputSamples   = 0;     // at device rate
            int     hwOutputSamples  = 0;     // at device rate
            int     srcInLatencyEng  = 0;     // at engine rate
            int     srcOutLatencyDev = 0;     // at device rate
            bool    hasInput  = false;
            bool    hasOutput = false;

            double getInputLatencyMs (double engineSr) const;
            double getOutputLatencyMs (double engineSr) const;
        };
        std::vector<DeviceItem> devices;
        double engineSampleRate = 48000.0;
        int    engineBlockSize  = 128;
        int    outputPreFillBlocks = 8;

        // Pipeline-only contribution: 1 engine block (matrix waits) +
        // pre-fill (static cushion).
        double getEngineContributionMs() const;

        // Worst-case round-trip = max(input device latency) + engine path
        // + max(output device latency).
        double getRoundTripMsWorst() const;
    };

    LatencyReport getLatencyReport() const;

private:
    void audioDeviceListChanged() override;

    std::unique_ptr<juce::AudioIODeviceType> deviceType;
    EngineSettings settings;

    std::vector<std::unique_ptr<DeviceWorker>> workers;
    std::vector<std::unique_ptr<PluginHost>>   pluginHosts;       // one per output channel
    std::vector<std::unique_ptr<PluginHost>>   inputPluginHosts;  // one per input channel
    std::vector<DeviceInfo>                    deviceInfo;
    RoutingMatrix                              matrix;
    OutputGroupManager                         groupManager;
    InputGroupManager                          inputGroupManager;
    MatrixProcessor                            processor;
    juce::AudioPluginFormatManager             pluginFormatManager;
    juce::KnownPluginList                      knownPlugins;
    bool                                       runningFlag = false;
};

} // namespace dcr

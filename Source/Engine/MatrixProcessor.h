#pragma once

#include <juce_core/juce_core.h>

#include "Engine/EngineSettings.h"
#include "Engine/WorkerPool.h"
#include "Routing/RoutingMatrix.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

namespace dcr {

class DeviceWorker;
class PluginHost;
class OutputGroupManager;
class InputGroupManager;

// Pulls one engine block from each global input channel, runs the routing
// matrix to produce each global output channel, and pushes to the corresponding
// output ring. Runs on its own thread.
class MatrixProcessor
{
public:
    MatrixProcessor();
    ~MatrixProcessor();

    struct GlobalInput  { DeviceWorker* device; int channelIndex; PluginHost* plugin = nullptr; };
    struct GlobalOutput { DeviceWorker* device; int channelIndex; PluginHost* plugin = nullptr; };

    void configure (std::vector<GlobalInput>  inputs,
                    std::vector<GlobalOutput> outputs,
                    RoutingMatrix*            matrix,
                    OutputGroupManager*       groupManager,
                    InputGroupManager*        inputGroupManager,
                    const EngineSettings&     settings);

    void start();
    void stop();

    // Diagnostics (UI-thread safe to read).
    uint64_t getBlocksProcessed() const noexcept { return blocksProcessed.load (std::memory_order_relaxed); }
    uint64_t getBlocksStalled()   const noexcept { return blocksStalled  .load (std::memory_order_relaxed); }

    // CPU load: 0..1 ratio of (block processing time) / (block period).
    float getCpuLoadAvg()  const noexcept { return cpuLoadAvg .load (std::memory_order_relaxed); }
    float getCpuLoadPeak() const noexcept { return cpuLoadPeak.load (std::memory_order_relaxed); }
    void  resetCpuPeak()         noexcept { cpuLoadPeak.store (0.0f, std::memory_order_relaxed); }

    // Latest-block peak (linear) per global channel. Returns 0 if out of range.
    float getInputPeak  (int globalIn)  const noexcept;
    float getOutputPeak (int globalOut) const noexcept;

private:
    void threadLoop();
    bool tryProcessOneBlock();

    std::vector<GlobalInput>  inputs;
    std::vector<GlobalOutput> outputs;
    RoutingMatrix*            matrix = nullptr;
    OutputGroupManager*       groupManager = nullptr;
    InputGroupManager*        inputGroupManager = nullptr;
    int                       blockSize = 128;
    int                       threadSleepMicros = 250;
    int                       drainPerWake = 16;
    std::vector<float>        groupSilenceRow;   // zeros for unfilled group slots

    // CRITICAL DECLARATION ORDER: pool MUST be declared before `thread`.
    // Destruction order is reverse of declaration -- thread destructs FIRST.
    // If ~MatrixProcessor ever runs without an explicit stop() (construction
    // failure mid-AudioEngine setup, future refactor), we want:
    //   thread dtor -> std::terminate if running   (loud, debuggable)
    //   pool dtor   -> kills workers
    // ... rather than the inverse, where the pool would die first while the
    // matrix thread is still blocked inside parallelFor on completedWorkers
    // (silent deadlock / UAF of jobFn).
    std::unique_ptr<WorkerPool> pool;

    std::thread thread;
    std::atomic<bool> running { false };
    std::atomic<uint64_t> blocksProcessed { 0 };
    std::atomic<uint64_t> blocksStalled   { 0 };
    std::atomic<float>    cpuLoadAvg     { 0.0f };
    std::atomic<float>    cpuLoadPeak    { 0.0f };
    double                sampleRate     = 48000.0;

    // Scratch (process thread)
    std::vector<float> inBuf;    // numIns * blockSize, channel-interleaved-by-channel (planar)
    std::vector<float> outBuf;   // numOuts * blockSize

    std::vector<std::atomic<float>> inputPeaks;
    std::vector<std::atomic<float>> outputPeaks;

    // Cached sparse routing list - rebuilt only when matrix dirtyGen changes.
    // currentGain is the smoothed actual gain applied this block; it
    // interpolates toward targetGain each block.  Routes whose target is 0
    // are kept until currentGain drops below epsilon (smooth fade-out).
    struct ActiveRoute { int outIdx; int inIdx; float targetGain; float currentGain; };
    std::vector<ActiveRoute> activeRoutes;
    std::vector<float>       inputEffGain;   // per-input mute/solo-aware trim
    uint64_t lastSnapGen = 0;
    float    smoothCoeff = 1.0f;             // 1.0 = no smoothing (instant)

    void refreshSnapshotIfDirty();
};

} // namespace dcr

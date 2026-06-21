#pragma once

#include <juce_core/juce_core.h>

#include "Engine/EngineSettings.h"
#include "Engine/PdcDelayLine.h"
#include "Engine/WorkerPool.h"
#include "Routing/RoutingMatrix.h"

#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dcr
{

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

        struct GlobalInput
        {
            DeviceWorker* device;
            int channelIndex;
            PluginHost* plugin = nullptr;
        };
        struct GlobalOutput
        {
            DeviceWorker* device;
            int channelIndex;
            PluginHost* plugin = nullptr;
        };

        void configure (std::vector<GlobalInput> inputs,
            std::vector<GlobalOutput> outputs,
            RoutingMatrix* matrix,
            OutputGroupManager* groupManager,
            InputGroupManager* inputGroupManager,
            const EngineSettings& settings);

        void start();
        void stop();

        // Diagnostics (UI-thread safe to read).
        uint64_t getBlocksProcessed() const noexcept { return blocksProcessed.load (std::memory_order_relaxed); }
        uint64_t getBlocksStalled() const noexcept { return blocksStalled.load (std::memory_order_relaxed); }
        // Output-ring-full drops: the matrix produced a block but the output ring
        // had no room (consumer fell behind -- typically the tail of a catch-up
        // burst after the thread was preempted).  Silently lost samples until now.
        uint64_t getOutputDrops() const noexcept { return outputDrops.load (std::memory_order_relaxed); }

        // Event-driven wakeup: each input device callback signals this after it
        // writes a fresh block into its input rings, so the matrix thread can run
        // immediately instead of sleep-polling (which added up to threadSleepMicros
        // of latency jitter and meant a preemption was only noticed a poll later).
        juce::WaitableEvent& getInputReadyEvent() noexcept { return inputReady; }

        // CPU load: 0..1 ratio of (block processing time) / (block period).
        float getCpuLoadAvg() const noexcept { return cpuLoadAvg.load (std::memory_order_relaxed); }
        float getCpuLoadPeak() const noexcept { return cpuLoadPeak.load (std::memory_order_relaxed); }
        void resetCpuPeak() noexcept { cpuLoadPeak.store (0.0f, std::memory_order_relaxed); }

        // Latest-block peak (linear) per global channel. Returns 0 if out of range.
        float getInputPeak (int globalIn) const noexcept;
        float getOutputPeak (int globalOut) const noexcept;

        // Master output fade, applied on top of every output AFTER the mix.  Used
        // by the engine-restart path to ramp the whole output to silence (click-
        // free) WITHOUT touching the user's per-channel output trims -- clobbering
        // those was leaking a permanent -60 dB ("silent") state when a restart
        // aborted or a snapshot save raced the fade.  1.0 = unity (normal).
        void setMasterGainTarget (float g) noexcept
        {
            masterGainTarget.store (g, std::memory_order_relaxed);
        }

        // Message thread: publish per-output PDC compensation delays (samples).
        // Entries past the configured output count are ignored.  Cheap atomic
        // stores; the matrix thread picks them up on its next block.
        void setPdcTargets (const std::vector<int>& delaysSamples) noexcept;

    private:
        void threadLoop();
        bool tryProcessOneBlock();

        std::vector<GlobalInput> inputs;
        std::vector<GlobalOutput> outputs;
        RoutingMatrix* matrix = nullptr;
        OutputGroupManager* groupManager = nullptr;
        InputGroupManager* inputGroupManager = nullptr;
        int blockSize = 128;
        int threadSleepMicros = 250;
        int drainPerWake = 16;
        std::vector<float> groupSilenceRow; // zeros for unfilled group slots

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
        juce::WaitableEvent inputReady { false }; // auto-reset: each wait() consumes one signal
        std::atomic<bool> running { false };
        std::atomic<uint64_t> blocksProcessed { 0 };
        std::atomic<uint64_t> blocksStalled { 0 };
        std::atomic<uint64_t> outputDrops { 0 };
        std::atomic<float> cpuLoadAvg { 0.0f };
        std::atomic<float> cpuLoadPeak { 0.0f };
        double sampleRate = 48000.0;

        // Scratch (process thread)
        std::vector<float> inBuf; // numIns * blockSize, channel-interleaved-by-channel (planar)
        std::vector<float> outBuf; // numOuts * blockSize

        std::vector<std::atomic<float>> inputPeaks;
        std::vector<std::atomic<float>> outputPeaks;

        // Cached sparse routing list - rebuilt only when matrix dirtyGen changes.
        // currentGain is the smoothed actual gain applied this block; it
        // interpolates toward targetGain each block.  Routes whose target is 0
        // are kept until currentGain drops below epsilon (smooth fade-out).
        struct ActiveRoute
        {
            int outIdx;
            int inIdx;
            float targetGain;
            float currentGain;
        };
        std::vector<ActiveRoute> activeRoutes;
        std::vector<float> inputEffGain; // per-input mute/solo-aware trim
        // Reused across refreshes (carry currentGain forward) -- declared as a
        // member so clear() reuses its buckets instead of allocating on the RT
        // thread each time the routing snapshot is rebuilt.
        std::unordered_map<uint64_t, float> oldGains;
        uint64_t lastSnapGen = 0;
        float smoothCoeff = 1.0f; // 1.0 = no smoothing (instant)

        // Per-output POST-fader gain (output trim, zeroed when muted), applied
        // AFTER the output plugin chains + group inserts so those process the
        // pre-fader signal -- standard DAW "inserts before fader" gain staging.
        // `target` is refreshed from the matrix; `current` is the smoothed value
        // actually applied, ramped per block to avoid zipper on fader moves.
        std::vector<float> outputFaderTarget;
        std::vector<float> outputFaderCurrent;
        bool faderInitialised = false;

        // Master output fade (engine-restart click suppression).  Target is set
        // from the message thread; current is ramped on the matrix thread with the
        // same one-pole coeff as the route gains.  Never touches per-channel trims.
        std::atomic<float> masterGainTarget { 1.0f };
        float masterGainCurrent = 1.0f;

        // ===== Plugin delay compensation (PDC) ==================================
        // One delay line per output channel, applied as the LAST stage (after the
        // meter tap, before the output ring) to realign every output behind the
        // slowest plugin chain.  Targets are computed on the message thread by
        // AudioEngine::replanPdc() and published via setPdcTargets(); the matrix
        // thread reads compTargetDelay[o] each block and pushes it into the delay
        // line, which ramps glitchlessly.  A target of 0 -- the common case (PDC
        // off / no latent plugin) -- costs only a history copy.
        std::vector<PdcDelayLine> outputDelays;
        std::vector<std::atomic<int>> compTargetDelay;

        void refreshSnapshotIfDirty();
    };

} // namespace dcr

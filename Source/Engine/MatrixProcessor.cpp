#include "Engine/MatrixProcessor.h"

#include "Engine/DeviceWorker.h"
#include "DSP/PluginHost.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"
#include "Routing/InputGroupManager.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include <pthread.h>
#include <sys/qos.h>

namespace dcr {

MatrixProcessor::MatrixProcessor()
{
    // Build the worker pool up-front so engine restarts don't pay thread-
    // creation cost.  Sizing: hardware threads minus 2 (one for matrix
    // thread itself, one to keep UI / system responsive); capped at 7
    // because plugin parallelism above ~8 chains tends to be cache-bound
    // and stops scaling.
    const int hw = (int) std::thread::hardware_concurrency();
    const int n  = std::min (7, std::max (0, hw - 2));
    pool = std::make_unique<WorkerPool> (n);
}

MatrixProcessor::~MatrixProcessor()
{
    // Defensive: AudioEngine::stop() already calls processor.stop() in the
    // normal shutdown path, but a partially-constructed AudioEngine, an
    // exception during start(), or a future refactor could destroy us with
    // the matrix thread still running.  Stop here so the thread is joined
    // before the pool member (declared after thread in MatrixProcessor.h)
    // tears down any worker the matrix thread might still be waiting on.
    stop();
}

void MatrixProcessor::configure (std::vector<GlobalInput>  ins,
                                 std::vector<GlobalOutput> outs,
                                 RoutingMatrix*            m,
                                 OutputGroupManager*       gm,
                                 InputGroupManager*        igm,
                                 const EngineSettings&     settings)
{
    inputs            = std::move (ins);
    outputs           = std::move (outs);
    matrix            = m;
    groupManager      = gm;
    inputGroupManager = igm;
    blockSize         = settings.engineBlockSize;
    threadSleepMicros = settings.matrixThreadSleepMicros;
    drainPerWake      = settings.matrixDrainPerWake;
    sampleRate        = settings.engineSampleRate;
    groupSilenceRow.assign ((size_t) blockSize, 0.0f);
    cpuLoadAvg .store (0.0f, std::memory_order_relaxed);
    cpuLoadPeak.store (0.0f, std::memory_order_relaxed);

    // Per-block one-pole smoothing coefficient toward target gain.
    if (settings.gainSmoothingMs > 0 && sampleRate > 0.0)
    {
        const float blockDur = (float) blockSize / (float) sampleRate;
        const float tau      = (float) settings.gainSmoothingMs * 1.0e-3f;
        smoothCoeff = 1.0f - std::exp (-blockDur / tau);
    }
    else
    {
        smoothCoeff = 1.0f;  // instant
    }

    inBuf .assign (inputs .size() * (size_t) blockSize, 0.0f);
    outBuf.assign (outputs.size() * (size_t) blockSize, 0.0f);

    std::vector<std::atomic<float>> ip (inputs .size());
    std::vector<std::atomic<float>> op (outputs.size());
    for (auto& a : ip) a.store (0.0f, std::memory_order_relaxed);
    for (auto& a : op) a.store (0.0f, std::memory_order_relaxed);
    inputPeaks  = std::move (ip);
    outputPeaks = std::move (op);

    activeRoutes.clear();
    activeRoutes.reserve (inputs.size() * outputs.size());
    lastSnapGen = 0;   // force refresh on first block

    // First-build routes have currentGain = 0 so they fade IN smoothly when
    // the engine starts.  No special action needed beyond the clear above.
}

float MatrixProcessor::getInputPeak (int n) const noexcept
{
    return (n >= 0 && n < (int) inputPeaks.size())
        ? inputPeaks[(size_t) n].load (std::memory_order_relaxed) : 0.0f;
}
float MatrixProcessor::getOutputPeak (int m) const noexcept
{
    return (m >= 0 && m < (int) outputPeaks.size())
        ? outputPeaks[(size_t) m].load (std::memory_order_relaxed) : 0.0f;
}

void MatrixProcessor::start()
{
    if (running.exchange (true)) return;
    thread = std::thread ([this] { threadLoop(); });
}

void MatrixProcessor::stop()
{
    if (! running.exchange (false)) return;
    if (thread.joinable()) thread.join();
}

void MatrixProcessor::refreshSnapshotIfDirty()
{
    if (matrix == nullptr) return;
    const uint64_t gen = matrix->getDirtyGeneration();
    if (gen == lastSnapGen) return;
    lastSnapGen = gen;

    const int nIn  = std::min ((int) inputs .size(), matrix->getNumInputs());
    const int nOut = std::min ((int) outputs.size(), matrix->getNumOutputs());

    // Single pass for "any solo active".
    bool anySolo = false;
    for (int n = 0; n < nIn; ++n)
        if (matrix->getInputSolo (n)) { anySolo = true; break; }

    // Cache per-input effective gain (incorporates mute + solo) so the
    // inner loop is a single multiply.
    inputEffGain.assign ((size_t) nIn, 0.0f);
    for (int n = 0; n < nIn; ++n)
    {
        if (matrix->getInputMute (n)) continue;
        if (anySolo && ! matrix->getInputSolo (n)) continue;
        inputEffGain[(size_t) n] = matrix->getInputTrim (n);
    }

    // Snapshot existing currentGains so they survive the rebuild and the
    // matrix processor never jumps a gain value abruptly.
    std::unordered_map<uint64_t, float> oldGains;
    oldGains.reserve (activeRoutes.size());
    for (auto& r : activeRoutes)
        oldGains[((uint64_t) (uint32_t) r.outIdx << 32) | (uint32_t) r.inIdx] = r.currentGain;

    // Build the new active route list with currentGain carried forward.
    activeRoutes.clear();
    for (int m = 0; m < nOut; ++m)
    {
        if (matrix->getOutputMute (m)) continue;
        const float outG = matrix->getOutputTrim (m);
        if (outG == 0.0f) continue;

        for (int n = 0; n < nIn; ++n)
        {
            const float inG = inputEffGain[(size_t) n];
            if (inG == 0.0f) continue;
            const float xp = matrix->getCrosspoint (m, n);
            if (xp == 0.0f) continue;
            const float g = outG * inG * xp;
            if (g == 0.0f) continue;

            const uint64_t key = ((uint64_t) (uint32_t) m << 32) | (uint32_t) n;
            float carriedCurrent = 0.0f;
            auto it = oldGains.find (key);
            if (it != oldGains.end())
            {
                carriedCurrent = it->second;
                oldGains.erase (it);
            }
            activeRoutes.push_back ({ m, n, g, carriedCurrent });
        }
    }

    // Routes that disappeared but still have audible signal become ghost
    // fade-outs (target 0, currentGain != 0).  They drop off via smoothing.
    for (auto& kv : oldGains)
    {
        if (kv.second > 1.0e-5f)
        {
            const int m = (int) (kv.first >> 32);
            const int n = (int) (kv.first & 0xFFFFFFFFu);
            activeRoutes.push_back ({ m, n, 0.0f, kv.second });
        }
    }
}

bool MatrixProcessor::tryProcessOneBlock()
{
    if (inputs.empty() || outputs.empty() || matrix == nullptr) return false;

    // Need every input ring to have >= blockSize samples available.
    for (auto& in : inputs)
    {
        auto* ring = in.device->getInputRing (in.channelIndex);
        if (ring == nullptr) return false;
        if (ring->readAvailable() < (size_t) blockSize)
        {
            blocksStalled.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
    }

    const auto t0 = std::chrono::steady_clock::now();

    // Read one block per input channel and compute peak (SIMD).
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        auto* ring = inputs[i].device->getInputRing (inputs[i].channelIndex);
        float* dst = inBuf.data() + i * (size_t) blockSize;
        ring->read (dst, (size_t) blockSize);

        const auto r = juce::FloatVectorOperations::findMinAndMax (dst, blockSize);
        const float peak = juce::jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
        inputPeaks[i].store (peak, std::memory_order_relaxed);
    }

    // Per-input single-channel plugin chains (3 slots each), run BEFORE
    // the input group multi-channel chain and the matrix mix.  Channels are
    // independent (each has its own buffer slice and its own PluginHost),
    // so they fork across the worker pool.  The pool inlines on the caller
    // for count <= 2 to skip sync overhead in the common 1/2-channel case.
    if (! inputs.empty() && pool != nullptr)
    {
        pool->parallelFor ((int) inputs.size(), [this] (int i)
        {
            if (inputs[(size_t) i].plugin != nullptr)
                inputs[(size_t) i].plugin->processBlock (
                    inBuf.data() + (size_t) i * (size_t) blockSize, blockSize);
        });
    }

    refreshSnapshotIfDirty();

    // Input-side multi-channel plugin chains, run BEFORE the matrix mix so
    // the routed signal already includes any input-group processing.
    if (inputGroupManager != nullptr)
    {
        const int nIn = (int) inputs.size();
        const size_t maxGroupCh = 16;
        std::array<float*, maxGroupCh> chPtrs{};

        inputGroupManager->forEachGroupForAudio ([&] (OutputGroup& g)
        {
            const int n = (int) g.memberChannels.size();
            if (n <= 0 || n > (int) maxGroupCh) return;

            bool anyActive = false;
            for (auto& s : g.pluginSlots)
                if (s && s->getPlugin() && ! s->isBypassed()) { anyActive = true; break; }
            if (! anyActive) return;

            for (int slot = 0; slot < n; ++slot)
            {
                const int ch = g.memberChannels[(size_t) slot];
                if (ch >= 0 && ch < nIn)
                    chPtrs[(size_t) slot] = inBuf.data() + (size_t) ch * (size_t) blockSize;
                else
                    chPtrs[(size_t) slot] = groupSilenceRow.data();
            }
            for (auto& s : g.pluginSlots)
                if (s && s->getPlugin() && ! s->isBypassed())
                    s->processBlock (chPtrs.data(), blockSize);
        });
    }

    // Zero output bus (one memset for the whole buffer is faster than per-row).
    if (! outBuf.empty())
        std::memset (outBuf.data(), 0, sizeof (float) * outBuf.size());

    // Mix - iterate only active (non-zero) routes, SIMD inner.  Smooth each
    // route's gain toward its target so trim moves / mutes / fader sweeps
    // never produce zipper noise.
    for (auto& rt : activeRoutes)
    {
        rt.currentGain += (rt.targetGain - rt.currentGain) * smoothCoeff;
        if (rt.currentGain <= 1.0e-7f && rt.targetGain == 0.0f) continue;

        float* outRow = outBuf.data() + (size_t) rt.outIdx * (size_t) blockSize;
        const float* inRow = inBuf.data() + (size_t) rt.inIdx * (size_t) blockSize;
        juce::FloatVectorOperations::addWithMultiply (outRow, inRow, rt.currentGain, blockSize);
    }

    // Per-output single-channel plugin chains.  Same parallelism story as
    // the input side -- each output channel has its own buffer slice + its
    // own host, so the pool can fan the work out across cores.
    if (! outputs.empty() && pool != nullptr)
    {
        pool->parallelFor ((int) outputs.size(), [this] (int i)
        {
            float* src = outBuf.data() + (size_t) i * (size_t) blockSize;
            if (outputs[(size_t) i].plugin != nullptr)
                outputs[(size_t) i].plugin->processBlock (src, blockSize);
        });
    }

    // Multi-channel group inserts.  For each group, gather member rows into
    // a temporary pointer array and let the plugin process them in-place.
    if (groupManager != nullptr)
    {
        const int nOut = (int) outputs.size();
        const size_t maxGroupCh = 16;   // enough for 7.1.4 (12) + slack
        std::array<float*, maxGroupCh> chPtrs{};

        groupManager->forEachGroupForAudio ([&] (OutputGroup& g)
        {
            const int n = (int) g.memberChannels.size();
            if (n <= 0 || n > (int) maxGroupCh) return;

            bool anyActive = false;
            for (auto& s : g.pluginSlots)
                if (s && s->getPlugin() && ! s->isBypassed()) { anyActive = true; break; }
            if (! anyActive) return;

            for (int slot = 0; slot < n; ++slot)
            {
                const int ch = g.memberChannels[(size_t) slot];
                if (ch >= 0 && ch < nOut)
                    chPtrs[(size_t) slot] = outBuf.data() + (size_t) ch * (size_t) blockSize;
                else
                    chPtrs[(size_t) slot] = groupSilenceRow.data();
            }

            // Run the chain in order; bypass-or-empty slots skipped.
            for (auto& s : g.pluginSlots)
                if (s && s->getPlugin() && ! s->isBypassed())
                    s->processBlock (chPtrs.data(), blockSize);
        });
    }

    // Peak (SIMD), then write rings.
    for (size_t i = 0; i < outputs.size(); ++i)
    {
        float* src = outBuf.data() + i * (size_t) blockSize;

        const auto r = juce::FloatVectorOperations::findMinAndMax (src, blockSize);
        const float peak = juce::jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
        outputPeaks[i].store (peak, std::memory_order_relaxed);

        auto* ring = outputs[i].device->getOutputRing (outputs[i].channelIndex);
        if (ring != nullptr)
            ring->write (src, (size_t) blockSize);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const double elapsedSec     = std::chrono::duration<double> (t1 - t0).count();
    const double blockPeriodSec = blockSize / juce::jmax (1.0, sampleRate);
    const float  load           = (float) (elapsedSec / blockPeriodSec);

    // EMA (~1 sec time constant at typical 375 blocks/sec)
    const float oldAvg = cpuLoadAvg.load (std::memory_order_relaxed);
    cpuLoadAvg.store (oldAvg * 0.997f + load * 0.003f, std::memory_order_relaxed);
    // Peak hold with slow decay (~10 s to fall by half)
    const float oldPeak = cpuLoadPeak.load (std::memory_order_relaxed);
    cpuLoadPeak.store (juce::jmax (load, oldPeak * 0.9998f), std::memory_order_relaxed);

    blocksProcessed.fetch_add (1, std::memory_order_relaxed);
    return true;
}

void MatrixProcessor::threadLoop()
{
    juce::Thread::setCurrentThreadName ("dcr.Matrix");

    // Audio-rate worker -- bump scheduling QoS to the highest non-RT class
    // so the kernel doesn't time-slice us against random UI / Spotlight /
    // background work.  USER_INTERACTIVE is Apple's recommended class for
    // audio worker threads that aren't the I/O callback itself.  Without
    // this, OpenGL paint storms or another busy app can preempt us for
    // long enough to underrun the output ring -> audible pops.
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);

    while (running.load (std::memory_order_relaxed))
    {
        bool progressed = false;
        for (int i = 0; i < drainPerWake; ++i)
        {
            if (! tryProcessOneBlock()) break;
            progressed = true;
        }
        if (! progressed)
            std::this_thread::sleep_for (std::chrono::microseconds (threadSleepMicros));
    }
}

} // namespace dcr

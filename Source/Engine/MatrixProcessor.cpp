#include "Engine/MatrixProcessor.h"

#include "Engine/DeviceWorker.h"
#include "Engine/PdcPlan.h"
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
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>

namespace dcr {

namespace
{
    // Promote the calling thread to a real-time (time-constraint) scheduling
    // class so macOS won't preempt it for tens of milliseconds during UI
    // repaints / Spotlight / other apps -- the exact stall that drained the
    // output ring and produced the audible stutter + tear.  Parameters are
    // expressed against the engine block period: we ask for up to `comp` of
    // CPU within every `period`, with a hard `constraint` deadline.
    // preemptible=true keeps us BELOW the CoreAudio HAL I/O threads (which set
    // tighter constraints), so device callbacks still win -- we only out-rank
    // ordinary timeshare work.
    void setRealtimeSchedule (double blockPeriodSec)
    {
        mach_timebase_info_data_t tb{};
        if (mach_timebase_info (&tb) != KERN_SUCCESS || tb.numer == 0) return;

        // ns -> mach absolute-time ticks.
        auto nsToAbs = [&] (double ns) -> uint32_t
        {
            const double abs = ns * (double) tb.denom / (double) tb.numer;
            return (uint32_t) juce::jlimit (1.0, 4.0e9, abs);
        };

        const double periodNs = juce::jmax (1.0e6, blockPeriodSec * 1.0e9); // >= 1 ms

        thread_time_constraint_policy_data_t pol{};
        pol.period      = nsToAbs (periodNs);
        pol.computation = nsToAbs (periodNs * 0.5);   // expected CPU per period
        pol.constraint  = nsToAbs (periodNs * 0.9);   // hard deadline within the period
        pol.preemptible = 1;

        const kern_return_t kr = thread_policy_set (
            pthread_mach_thread_np (pthread_self()),
            THREAD_TIME_CONSTRAINT_POLICY,
            (thread_policy_t) &pol,
            THREAD_TIME_CONSTRAINT_POLICY_COUNT);

        juce::Logger::writeToLog (kr == KERN_SUCCESS
            ? juce::String ("dcr.Matrix: real-time scheduling engaged (period ")
                  + juce::String (periodNs / 1.0e6, 2) + " ms)"
            : juce::String ("dcr.Matrix: real-time scheduling REQUEST FAILED (kr=")
                  + juce::String ((int) kr) + ") -- staying on USER_INTERACTIVE QoS");
    }
}

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

    // Post-fader gain stage (applied after the output inserts).  Start at
    // unity; the first dirty-refresh snaps current -> target so there's no
    // fade-in artifact on engine start.
    outputFaderTarget .assign (outputs.size(), 1.0f);
    outputFaderCurrent.assign (outputs.size(), 1.0f);
    faderInitialised = false;

    // Fresh engine -> master fade starts at unity (the OLD engine's fade-to-0
    // died with its processor; this one plays at full volume immediately).
    masterGainTarget.store (1.0f, std::memory_order_relaxed);
    masterGainCurrent = 1.0f;

    // PDC delay lines: one per output, pre-sized to the hard cap so a latency
    // change never allocates on the audio thread.  Targets start at 0 (no
    // compensation); AudioEngine::replanPdc() publishes the real plan after
    // configure and whenever a plugin's latency changes.
    outputDelays.clear();
    outputDelays.resize (outputs.size());
    for (auto& d : outputDelays) d.prepare (kMaxPdcSamples, blockSize);
    std::vector<std::atomic<int>> ct (outputs.size());
    for (auto& a : ct) a.store (0, std::memory_order_relaxed);
    compTargetDelay = std::move (ct);

    // First-build routes have currentGain = 0 so they fade IN smoothly when
    // the engine starts.  No special action needed beyond the clear above.
}

void MatrixProcessor::setPdcTargets (const std::vector<int>& delays) noexcept
{
    const size_t n = std::min (delays.size(), compTargetDelay.size());
    for (size_t o = 0; o < n; ++o)
        compTargetDelay[o].store (delays[o], std::memory_order_relaxed);
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
    inputReady.signal();   // wake the thread out of its wait so it exits now
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

    // Cache per-input effective gain (incorporates mute + solo + any Router-mode
    // input-group overlay) so the inner loop is a single multiply.  The overlay
    // is 1.0 for VCA-mode / ungrouped channels; a Router-muted channel reads 0.
    inputEffGain.assign ((size_t) nIn, 0.0f);
    for (int n = 0; n < nIn; ++n)
    {
        if (matrix->getInputMute (n)) continue;
        if (anySolo && ! matrix->getInputSolo (n)) continue;
        const float groupG = inputGroupManager != nullptr
                                 ? inputGroupManager->getChannelRouterGain (n) : 1.0f;
        inputEffGain[(size_t) n] = matrix->getInputTrim (n) * groupG;
    }

    // Snapshot existing currentGains so they survive the rebuild and the
    // matrix processor never jumps a gain value abruptly.  oldGains is a
    // reused member: clear() keeps the bucket array allocated, so after the
    // first refresh this does no heap allocation on the matrix (RT) thread --
    // important because a fader drag dirties the snapshot many times a second.
    oldGains.clear();
    for (auto& r : activeRoutes)
        oldGains[((uint64_t) (uint32_t) r.outIdx << 32) | (uint32_t) r.inIdx] = r.currentGain;

    // Capture the per-output POST-fader gain (trim, zeroed when muted).  This
    // is applied AFTER the output plugin chains so inserts see the pre-fader
    // signal.  Output mute/trim therefore do NOT gate the mix below -- the
    // plugin must keep processing even when the fader is down or muted.
    for (size_t i = 0; i < outputFaderTarget.size(); ++i)
    {
        const int m = (int) i;
        // Fold in any Router-mode output-group overlay (1.0 for VCA / ungrouped;
        // 0 when the Router group is muted).  Output mute still forces silence.
        const float groupG = groupManager != nullptr ? groupManager->getChannelRouterGain (m) : 1.0f;
        const float t = (m < nOut && ! matrix->getOutputMute (m))
                            ? matrix->getOutputTrim (m) * groupG : (m < nOut ? 0.0f : 1.0f);
        outputFaderTarget[i] = t;
    }
    if (! faderInitialised)   // snap on first refresh -> no start-up fade
    {
        outputFaderCurrent = outputFaderTarget;
        faderInitialised = true;
    }

    // Build the new active route list with currentGain carried forward.  Route
    // gain is now input-side only (inputEffGain x crosspoint); the output
    // fader is applied post-plugin.
    activeRoutes.clear();
    for (int m = 0; m < nOut; ++m)
    {
        for (int n = 0; n < nIn; ++n)
        {
            const float inG = inputEffGain[(size_t) n];
            if (inG == 0.0f) continue;
            const float xp = matrix->getCrosspoint (m, n);
            if (xp == 0.0f) continue;
            const float g = inG * xp;
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

    // POST-FADER gain stage.  The output trim / mute, the VCA group fader (which
    // drives the per-channel trims) and any Router-mode group overlay (folded
    // into outputFaderTarget above) are applied HERE -- after every output
    // insert + group insert -- so plugins process the pre-fader signal (standard
    // DAW gain staging).  Smoothed per block toward the target so a fader sweep
    // doesn't zipper.
    for (size_t i = 0; i < outputs.size() && i < outputFaderCurrent.size(); ++i)
    {
        const float tgt = outputFaderTarget[i];
        float&      cur = outputFaderCurrent[i];
        cur += (tgt - cur) * smoothCoeff;
        if (cur >= 0.99999f && tgt >= 0.99999f) continue;   // unity: skip
        if (cur <= 1.0e-7f  && tgt == 0.0f)                  // fully faded: zero it
        {
            std::memset (outBuf.data() + i * (size_t) blockSize, 0, sizeof (float) * (size_t) blockSize);
            continue;
        }
        juce::FloatVectorOperations::multiply (outBuf.data() + i * (size_t) blockSize, cur, blockSize);
    }

    // Master output fade (engine-restart click suppression).  Ramp toward the
    // target once per block; when it's effectively unity the multiply is
    // skipped so steady-state costs nothing.  Applied to the post-mix bus so
    // the meters reflect the faded output too.
    masterGainCurrent += (masterGainTarget.load (std::memory_order_relaxed) - masterGainCurrent) * smoothCoeff;
    const float masterG = masterGainCurrent;
    const bool  applyMaster = masterG < 0.99999f;

    // Peak (SIMD), then write rings.
    for (size_t i = 0; i < outputs.size(); ++i)
    {
        float* src = outBuf.data() + i * (size_t) blockSize;

        if (applyMaster)
            juce::FloatVectorOperations::multiply (src, masterG, blockSize);

        const auto r = juce::FloatVectorOperations::findMinAndMax (src, blockSize);
        const float peak = juce::jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
        outputPeaks[i].store (peak, std::memory_order_relaxed);

        // PDC: realign this output behind the slowest plugin chain.  Applied
        // LAST -- after the meter tap (the meter reads the pre-delay level; a
        // delay shifts time, not level) and just before the ring.  A target of
        // 0 -- PDC off or no latent plugin -- makes this a cheap history copy.
        if (i < outputDelays.size())
        {
            outputDelays[i].setTargetDelay (compTargetDelay[i].load (std::memory_order_relaxed));
            outputDelays[i].process (src, blockSize);
        }

        auto* ring = outputs[i].device->getOutputRing (outputs[i].channelIndex);
        if (ring != nullptr)
        {
            const size_t wrote = ring->write (src, (size_t) blockSize);
            if (wrote < (size_t) blockSize)
                outputDrops.fetch_add (1, std::memory_order_relaxed);
        }
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

    // First raise QoS (so even if the RT request below fails we're still on
    // the highest timeshare class), THEN engage real-time time-constraint
    // scheduling so the kernel won't preempt us mid-block for UI / Spotlight /
    // other apps -- the stall that drained the output ring and produced the
    // audible stutter + tear.
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
    const double blockPeriodSec = (double) blockSize / juce::jmax (1.0, sampleRate);
    setRealtimeSchedule (blockPeriodSec);

    // Event-driven: block on inputReady (signalled by each input device
    // callback after it writes a fresh block) instead of sleep-polling.  The
    // wait has a timeout fallback so a missed/coalesced signal still makes
    // progress, and so we re-check `running` to exit promptly.  The timeout is
    // ~half a block period: short enough to self-heal, long enough not to spin.
    const int timeoutMs = juce::jlimit (1, 50, (int) std::lround (blockPeriodSec * 500.0));

    while (running.load (std::memory_order_relaxed))
    {
        bool progressed = false;
        for (int i = 0; i < drainPerWake; ++i)
        {
            if (! tryProcessOneBlock()) break;
            progressed = true;
        }
        // If we processed everything available, sleep on the event until the
        // next input block arrives (or the timeout fallback fires).  If we hit
        // the drain cap with work still pending, loop straight back without
        // waiting so we don't fall behind.
        if (! progressed)
            inputReady.wait (timeoutMs);
    }
}

} // namespace dcr

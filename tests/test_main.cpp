// Pure-logic regression tests for D-Router.  No JUCE, no audio device --
// these run headless in milliseconds and guard the deterministic core:
// the SPSC ring buffer and the routing matrix (gains, mute/solo, the
// virtual-device self-loop block).  Exit code != 0 on any failure (ctest).
//
// DSP tests that need JUCE (STFT/WOLA reconstruction, the pre-fader gain
// staging) are intentionally NOT here -- they belong in a JUCE-linked target;
// see the PR notes.

#include "Engine/RingBuffer.h"
#include "Engine/PdcDelayLine.h"
#include "Engine/PdcPlan.h"
#include "Routing/GroupGain.h"
#include "Routing/RoutingMatrix.h"
#include "DSP/Builtin/ResonanceMath.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <vector>

namespace {

int g_checks = 0;
int g_fails  = 0;

#define CHECK(cond)                                                            \
    do {                                                                      \
        ++g_checks;                                                           \
        if (!(cond)) {                                                        \
            ++g_fails;                                                        \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);    \
        }                                                                     \
    } while (0)

bool feq (float a, float b, float eps = 1.0e-6f) { return std::fabs (a - b) <= eps; }

// ---------------------------------------------------------------------------
// FloatRingBuffer (SPSC)
// ---------------------------------------------------------------------------
void test_ring_basic()
{
    dcr::FloatRingBuffer r (100);
    // capacity is next-power-of-two minus one.
    CHECK (r.bufferSize() == 128);
    CHECK (r.capacity()   == 127);
    CHECK (r.readAvailable()  == 0);
    CHECK (r.writeAvailable() == 127);

    float in[10];
    for (int i = 0; i < 10; ++i) in[i] = (float) i;
    CHECK (r.write (in, 10) == 10);
    CHECK (r.readAvailable()  == 10);
    CHECK (r.writeAvailable() == 117);

    float out[10] = {0};
    CHECK (r.read (out, 10) == 10);
    for (int i = 0; i < 10; ++i) CHECK (feq (out[i], (float) i));
    CHECK (r.readAvailable() == 0);
}

void test_ring_overflow_underflow()
{
    // Size rounds up to the next power of two that can hold AT LEAST the
    // requested count: request 8 -> bufferSize 16, capacity 15 (one slot is
    // reserved to disambiguate full vs empty).
    dcr::FloatRingBuffer r (8);
    CHECK (r.capacity()   == 15);
    CHECK (r.bufferSize() == 16);
    CHECK (r.capacity() >= 8);             // the contract: can hold the request

    std::vector<float> big (40, 1.0f);
    // write only takes up to capacity.
    CHECK (r.write (big.data(), 40) == 15);
    CHECK (r.writeAvailable() == 0);
    // further writes take nothing while full.
    CHECK (r.write (big.data(), 5) == 0);

    std::vector<float> out (40, -1.0f);
    // read only returns what's available.
    CHECK (r.read (out.data(), 40) == 15);
    CHECK (r.readAvailable() == 0);
    // further reads return nothing while empty.
    CHECK (r.read (out.data(), 5) == 0);
}

void test_ring_wraparound_integrity()
{
    // Push a long monotonic sequence through in mismatched chunk sizes so the
    // read/write indices wrap many times; the consumer must see every sample
    // exactly once, in order.
    dcr::FloatRingBuffer r (16);
    float nextWrite = 0.0f, nextRead = 0.0f;
    int wrote = 0, read = 0;
    for (int iter = 0; iter < 1000; ++iter)
    {
        float buf[7];
        const int wantW = (iter % 7) + 1;
        for (int i = 0; i < wantW; ++i) buf[i] = nextWrite + (float) i;
        const size_t w = r.write (buf, (size_t) wantW);
        nextWrite += (float) w; wrote += (int) w;

        float ob[5];
        const int wantR = (iter % 5) + 1;
        const size_t got = r.read (ob, (size_t) wantR);
        for (size_t i = 0; i < got; ++i) CHECK (feq (ob[i], nextRead + (float) i));
        nextRead += (float) got; read += (int) got;
    }
    // drain the rest and verify the running counters stayed in lock-step.
    float ob[64];
    size_t got;
    while ((got = r.read (ob, 64)) > 0)
    {
        for (size_t i = 0; i < got; ++i) CHECK (feq (ob[i], nextRead + (float) i));
        nextRead += (float) got; read += (int) got;
    }
    CHECK (read == wrote);
    CHECK (feq (nextRead, nextWrite));
}

void test_ring_clear()
{
    dcr::FloatRingBuffer r (16);
    float in[4] = {1, 2, 3, 4};
    r.write (in, 4);
    CHECK (r.readAvailable() == 4);
    r.clear();
    CHECK (r.readAvailable()  == 0);
    CHECK (r.writeAvailable() == r.capacity());
}

// ---------------------------------------------------------------------------
// RoutingMatrix
// ---------------------------------------------------------------------------
void test_matrix_defaults()
{
    dcr::RoutingMatrix m;
    m.resize (4, 3);
    CHECK (m.getNumInputs()  == 4);
    CHECK (m.getNumOutputs() == 3);
    for (int n = 0; n < 4; ++n) { CHECK (feq (m.getInputTrim (n), 1.0f)); CHECK (! m.getInputMute (n)); CHECK (! m.getInputSolo (n)); }
    for (int o = 0; o < 3; ++o) { CHECK (feq (m.getOutputTrim (o), 1.0f)); CHECK (! m.getOutputMute (o)); }
    for (int o = 0; o < 3; ++o)
        for (int n = 0; n < 4; ++n) { CHECK (feq (m.getCrosspoint (o, n), 0.0f)); CHECK (! m.isBlocked (o, n)); }
    // Out-of-range reads must not crash and return 0.
    CHECK (feq (m.getCrosspoint (99, 99), 0.0f));
    CHECK (feq (m.getInputTrim (99), 0.0f));
}

void test_matrix_crosspoint()
{
    dcr::RoutingMatrix m;
    m.resize (4, 3);
    m.setCrosspoint (2, 1, 0.5f);
    CHECK (feq (m.getCrosspoint (2, 1), 0.5f));
    CHECK (feq (m.getCrosspoint (1, 2), 0.0f));   // not the transpose
    m.setInputTrim (0, 0.25f);
    m.setOutputTrim (1, 0.75f);
    CHECK (feq (m.getInputTrim (0), 0.25f));
    CHECK (feq (m.getOutputTrim (1), 0.75f));
}

void test_matrix_self_loop_block()
{
    // The feedback guard: a blocked crosspoint is forced to 0 and stays 0
    // even if something tries to set it (snapshot restore, drag).
    dcr::RoutingMatrix m;
    m.resize (2, 2);
    m.setCrosspoint (0, 0, 1.0f);
    CHECK (feq (m.getCrosspoint (0, 0), 1.0f));

    m.setBlocked (0, 0, true);
    CHECK (m.isBlocked (0, 0));
    CHECK (feq (m.getCrosspoint (0, 0), 0.0f));     // blocking forces silence
    m.setCrosspoint (0, 0, 1.0f);                   // attempt to re-enable
    CHECK (feq (m.getCrosspoint (0, 0), 0.0f));     // ... is refused

    m.setBlocked (0, 0, false);                     // unblock
    m.setCrosspoint (0, 0, 1.0f);
    CHECK (feq (m.getCrosspoint (0, 0), 1.0f));     // now it takes
}

void test_matrix_mute_solo()
{
    dcr::RoutingMatrix m;
    m.resize (3, 2);
    m.setInputMute (1, true);
    m.setOutputMute (0, true);
    m.setInputSolo (2, true);
    CHECK (m.getInputMute (1));
    CHECK (! m.getInputMute (0));
    CHECK (m.getOutputMute (0));
    CHECK (m.getInputSolo (2));
    CHECK (! m.getInputSolo (0));
}

void test_matrix_dirty_generation()
{
    dcr::RoutingMatrix m;
    m.resize (2, 2);
    const auto g0 = m.getDirtyGeneration();
    m.setCrosspoint (0, 0, 0.5f);
    const auto g1 = m.getDirtyGeneration();
    CHECK (g1 > g0);
    m.setInputMute (0, true);
    CHECK (m.getDirtyGeneration() > g1);
}

void test_matrix_resize_clears_blocks()
{
    dcr::RoutingMatrix m;
    m.resize (2, 2);
    m.setBlocked (0, 0, true);
    m.setCrosspoint (1, 1, 0.5f);
    m.setOutputTrim (0, 0.1f);
    CHECK (m.isBlocked (0, 0));

    m.resize (2, 2);                                // re-resize must reset state
    CHECK (! m.isBlocked (0, 0));
    CHECK (feq (m.getCrosspoint (1, 1), 0.0f));
    CHECK (feq (m.getOutputTrim (0), 1.0f));
}

void test_matrix_snapshot()
{
    dcr::RoutingMatrix m;
    m.resize (2, 2);
    m.setInputTrim (0, 0.5f);
    m.setOutputTrim (1, 0.25f);
    m.setCrosspoint (1, 0, 0.8f);
    m.setInputMute (1, true);
    m.setOutputMute (0, true);
    m.setInputSolo (0, true);

    dcr::RoutingMatrix::Snapshot s;
    m.takeSnapshot (s);
    CHECK (s.numIns == 2 && s.numOuts == 2);
    CHECK (feq (s.inputTrim[0], 0.5f));
    CHECK (feq (s.outputTrim[1], 0.25f));
    CHECK (feq (s.at (1, 0), 0.8f));
    CHECK (s.inputMute[1] != 0);
    CHECK (s.outputMute[0] != 0);
    CHECK (s.inputSolo[0] != 0);
    CHECK (s.anySoloActive);
}

// ---------------------------------------------------------------------------
// GroupGain (VCA bake math + Router overlay gain)
// ---------------------------------------------------------------------------
void test_groupgain_db_roundtrip()
{
    using namespace dcr::groupgain;
    CHECK (feq (dbToLin (0.0f), 1.0f));
    CHECK (feq (dbToLin (-6.0206f), 0.5f, 1.0e-4f));
    CHECK (feq (dbToLin (6.0206f), 2.0f, 1.0e-4f));
    // -60 dB floor collapses to true silence on both directions.
    CHECK (feq (dbToLin (-60.0f), 0.0f));
    CHECK (feq (dbToLin (-90.0f), 0.0f));
    CHECK (feq (linToDb (0.0f), -60.0f));
    CHECK (feq (linToDb (1.0e-9f), -60.0f));
    // round-trip an audible level.
    CHECK (feq (linToDb (dbToLin (-3.0f)), -3.0f, 1.0e-3f));
}

void test_groupgain_clamp()
{
    using namespace dcr::groupgain;
    CHECK (feq (clampTrimDb (0.0f), 0.0f));
    CHECK (feq (clampTrimDb (99.0f), 12.0f));      // ceiling
    CHECK (feq (clampTrimDb (-99.0f), -60.0f));    // floor
    CHECK (feq (clampTrimDb (12.0f), 12.0f));
    CHECK (feq (clampTrimDb (-60.0f), -60.0f));
}

void test_groupgain_router_channel_gain()
{
    using namespace dcr::groupgain;
    // Unity fader, unmuted -> no overlay.
    CHECK (feq (routerChannelGain (false, 0.0f), 1.0f));
    // Unmuted tracks the fader.
    CHECK (feq (routerChannelGain (false, 6.0206f), 2.0f, 1.0e-4f));
    CHECK (feq (routerChannelGain (false, -6.0206f), 0.5f, 1.0e-4f));
    // Muted is always silent regardless of fader.
    CHECK (feq (routerChannelGain (true, 0.0f), 0.0f));
    CHECK (feq (routerChannelGain (true, 12.0f), 0.0f));
}

void test_groupgain_bake_vca()
{
    using namespace dcr::groupgain;
    // bake(x, 0) is just the clamped trim.
    CHECK (feq (bakeVcaTrimDb (-3.0f, 0.0f), -3.0f));
    // Additive in dB.
    CHECK (feq (bakeVcaTrimDb (-3.0f, -3.0f), -6.0f));
    CHECK (feq (bakeVcaTrimDb (-10.0f, 4.0f), -6.0f));
    // Clamps at the rails.
    CHECK (feq (bakeVcaTrimDb (10.0f, 10.0f), 12.0f));
    CHECK (feq (bakeVcaTrimDb (-55.0f, -20.0f), -60.0f));
}

void test_groupgain_mode_switch_preserves_level()
{
    using namespace dcr::groupgain;
    // The Router->VCA bake must reproduce the same audible level the Router
    // overlay produced, when no rail clamps:  trim * dbToLin(fader)  ==
    // dbToLin( bakeVcaTrimDb( linToDb(trim), fader ) ).
    const float fader = -4.5f;
    for (float trimDb : { -20.0f, -12.0f, -6.0f, -1.0f, 3.0f })
    {
        const float trimLin   = dbToLin (trimDb);
        const float routerLvl = trimLin * dbToLin (fader);                 // Router-mode level
        const float bakedLin  = dbToLin (bakeVcaTrimDb (linToDb (trimLin), fader)); // VCA-mode level
        CHECK (feq (routerLvl, bakedLin, 1.0e-3f));
    }
}

// ---------------------------------------------------------------------------
// PDC plan math (computePdcPlan): align every output to the slowest one.
// ---------------------------------------------------------------------------
void test_pdc_plan_disabled()
{
    // PDC off -> nothing is delayed, regardless of latencies.
    std::vector<int> lat { 1024, 0, 512 };
    auto p = dcr::computePdcPlan (lat, /*enabled*/ false, /*cap*/ 48000);
    CHECK (p.maxLatency == 0);
    CHECK (! p.clamped);
    CHECK (p.compDelay.size() == 3);
    for (int d : p.compDelay) CHECK (d == 0);
}

void test_pdc_plan_aligns_to_max()
{
    std::vector<int> lat { 1024, 0, 512 };
    auto p = dcr::computePdcPlan (lat, true, 48000);
    CHECK (p.maxLatency == 1024);
    CHECK (! p.clamped);
    CHECK (p.compDelay[0] == 0);
    CHECK (p.compDelay[1] == 1024);
    CHECK (p.compDelay[2] == 512);
    // The defining invariant: latency + compensation is equal across outputs.
    for (size_t o = 0; o < lat.size(); ++o)
        CHECK (lat[o] + p.compDelay[o] == p.maxLatency);
}

void test_pdc_plan_no_latency()
{
    std::vector<int> lat { 0, 0, 0 };
    auto p = dcr::computePdcPlan (lat, true, 48000);
    CHECK (p.maxLatency == 0);
    for (int d : p.compDelay) CHECK (d == 0);
}

void test_pdc_plan_empty()
{
    std::vector<int> lat;
    auto p = dcr::computePdcPlan (lat, true, 48000);
    CHECK (p.maxLatency == 0);
    CHECK (p.compDelay.empty());
}

void test_pdc_plan_negative_treated_as_zero()
{
    std::vector<int> lat { -5, 100 };
    auto p = dcr::computePdcPlan (lat, true, 48000);
    CHECK (p.maxLatency == 100);
    CHECK (p.compDelay[0] == 100);   // negative latency floored to 0
    CHECK (p.compDelay[1] == 0);
}

void test_pdc_plan_cap_clamps_and_flags()
{
    // An output beyond the cap is clamped and the plan flags it (the engine
    // logs it -- never a silent mis-alignment).
    std::vector<int> lat { 60000, 0 };
    auto p = dcr::computePdcPlan (lat, true, 48000);
    CHECK (p.clamped);
    CHECK (p.maxLatency == 48000);
    CHECK (p.compDelay[0] == 0);
    CHECK (p.compDelay[1] == 48000);
}

// ---------------------------------------------------------------------------
// PDC delay line (PdcDelayLine): exact integer delay + glitchless re-target.
// ---------------------------------------------------------------------------

// Feed a known ramp signal in[i]=i+1, verify the steady-state delay equals K
// (out[i] == in[i-K] well past the crossfade).
void check_settled_delay (int K)
{
    dcr::PdcDelayLine dl;
    dl.prepare (2000, 128, /*ramp*/ 8);
    dl.setTargetDelay (K);

    const int N = 4000;
    std::vector<float> in ((size_t) N), out ((size_t) N);
    for (int i = 0; i < N; ++i) { in[(size_t) i] = (float) (i + 1); out[(size_t) i] = in[(size_t) i]; }

    for (int off = 0; off < N; off += 128)
        dl.process (out.data() + off, std::min (128, N - off));

    CHECK (dl.getCurrentDelay() == K);
    for (int i = K + 64; i < N - 8; ++i)
        CHECK (feq (out[(size_t) i], in[(size_t) (i - K)]));
}

void test_pdc_delay_static()
{
    check_settled_delay (0);       // passthrough
    check_settled_delay (1);
    check_settled_delay (100);
    check_settled_delay (2000);    // == maxDelay (deepest valid tap)
}

void test_pdc_delay_change_settles()
{
    dcr::PdcDelayLine dl;
    dl.prepare (2000, 128, 8);

    const int N = 8000;
    std::vector<float> in ((size_t) N), out ((size_t) N);
    for (int i = 0; i < N; ++i) { in[(size_t) i] = (float) (i + 1); out[(size_t) i] = in[(size_t) i]; }

    auto run = [&] (int from, int to)
    {
        for (int i = from; i < to; i += 128)
            dl.process (out.data() + i, std::min (128, to - i));
    };

    dl.setTargetDelay (50);
    run (0, 4000);
    CHECK (dl.getCurrentDelay() == 50);
    for (int i = 50 + 64; i < 3900; ++i) CHECK (feq (out[(size_t) i], in[(size_t) (i - 50)]));

    dl.setTargetDelay (300);
    run (4000, N);
    CHECK (dl.getCurrentDelay() == 300);
    for (int i = 4000 + 300 + 64; i < N - 64; ++i) CHECK (feq (out[(size_t) i], in[(size_t) (i - 300)]));
}

void test_pdc_delay_idle_then_activate()
{
    // Run at delay 0 (exercising the idle fast path), then activate a delay and
    // confirm the steady state is exactly K -- i.e. the fast path kept history
    // current so the newly-delayed output reads real past samples, not silence.
    dcr::PdcDelayLine dl;
    dl.prepare (2000, 128, 8);
    const int N = 8000;
    std::vector<float> in ((size_t) N), out ((size_t) N);
    for (int i = 0; i < N; ++i) { in[(size_t) i] = (float) (i + 1); out[(size_t) i] = in[(size_t) i]; }
    auto run = [&] (int from, int to)
    {
        for (int i = from; i < to; i += 128) dl.process (out.data() + i, std::min (128, to - i));
    };

    run (0, 4000);                                     // delay 0: passthrough via fast path
    for (int i = 0; i < 3999; ++i) CHECK (feq (out[(size_t) i], in[(size_t) i]));

    dl.setTargetDelay (120);                           // activate
    run (4000, N);
    CHECK (dl.getCurrentDelay() == 120);
    for (int i = 4000 + 120 + 64; i < N - 64; ++i) CHECK (feq (out[(size_t) i], in[(size_t) (i - 120)]));
}

void test_pdc_delay_ramp_is_finite()
{
    // The crossfade region must never produce NaN/inf.
    dcr::PdcDelayLine dl;
    dl.prepare (1000, 128, 256);
    std::vector<float> buf (2000, 1.0f);
    dl.setTargetDelay (500);
    for (int off = 0; off < 2000; off += 128)
        dl.process (buf.data() + off, std::min (128, 2000 - off));
    for (float v : buf) CHECK (std::isfinite (v));
}

// ---------------------------------------------------------------------------
// ResonanceMath (Resonance Suppressor: node log-grid + threshold->reduction).
// JUCE-free deterministic pieces shared by the processor and editor; the
// per-bin attack/release smoothing is stateful and lives in the processor
// (verified by build + real-device listening, not here).
// ---------------------------------------------------------------------------
void test_resonance_node_freq()
{
    using namespace dcr::resonance;
    // 31-node log grid spans 20 Hz .. 20 kHz with clean decade anchors.
    CHECK (feq (nodeFreq (0, 31),  20.0f));
    CHECK (feq (nodeFreq (30, 31), 20000.0f, 1.0e-2f));
    CHECK (feq (nodeFreq (10, 31), 200.0f,  1.0e-2f));    // 1000^(1/3) == 10
    CHECK (feq (nodeFreq (20, 31), 2000.0f, 1.0e-2f));    // 1000^(2/3) == 100
    CHECK (feq (nodeFreq (15, 31), 632.4555f, 1.0e-2f));  // 20*sqrt(1000)
    // out-of-range indices clamp to the ends.
    CHECK (feq (nodeFreq (-5, 31), 20.0f));
    CHECK (feq (nodeFreq (99, 31), 20000.0f, 1.0e-2f));
}

void test_resonance_node_interp()
{
    using namespace dcr::resonance;
    // nodeInterp is the inverse of nodeFreq: a node's own frequency maps back
    // to that node's continuous position lo+frac (== i).  We assert the
    // position, not the lo/frac split -- at an exact boundary (i,0) and
    // (i-1,1) are the same point and float rounding may return either.
    for (int i = 0; i <= 29; ++i)
    {
        auto ni = nodeInterp (nodeFreq (i, 31), 31);
        CHECK (feq ((float) ni.lo + ni.frac, (float) i, 1.0e-3f));
    }
    // The top frequency lands on the last segment fully toward the top node.
    auto top = nodeInterp (nodeFreq (30, 31), 31);
    CHECK (top.lo == 29);
    CHECK (feq (top.frac, 1.0f, 1.0e-3f));
    // A frequency halfway (in log) between nodes 10 and 11 -> frac 0.5.
    const float fMid = 20.0f * std::pow (1000.0f, 10.5f / 30.0f);
    auto mid = nodeInterp (fMid, 31);
    CHECK (mid.lo == 10);
    CHECK (feq (mid.frac, 0.5f, 1.0e-3f));
    // Below/above the grid clamp to the end segments.
    auto lo = nodeInterp (5.0f, 31);
    CHECK (lo.lo == 0 && feq (lo.frac, 0.0f));
    auto hi = nodeInterp (40000.0f, 31);
    CHECK (hi.lo == 29 && feq (hi.frac, 1.0f, 1.0e-3f));
}

void test_resonance_target_reduction()
{
    using namespace dcr::resonance;
    // Below baseline+threshold -> no reduction.
    CHECK (feq (targetReductionDb (-40.0f, -50.0f, 12.0f, 0.8f, 24.0f), 0.0f));
    // Exactly at threshold (excess 0) -> no reduction.
    CHECK (feq (targetReductionDb (-30.0f, -50.0f, 20.0f, 1.0f, 24.0f), 0.0f));
    // Excess * sharpness, below the ceiling -> negative reduction. excess 16 * 0.8.
    CHECK (feq (targetReductionDb (-30.0f, -50.0f, 4.0f, 0.8f, 24.0f), -12.8f, 1.0e-4f));
    // Clamped at max reduction.
    CHECK (feq (targetReductionDb (-30.0f, -50.0f, 4.0f, 0.8f, 10.0f), -10.0f));
    // Raising the (per-frequency) threshold reduces the action monotonically.
    const float a = targetReductionDb (-30.0f, -50.0f, 10.0f, 1.0f, 24.0f);   // excess 10 -> -10
    const float b = targetReductionDb (-30.0f, -50.0f, 18.0f, 1.0f, 24.0f);   // excess  2 -> -2
    CHECK (feq (a, -10.0f));
    CHECK (feq (b, -2.0f));
    CHECK (b > a);                 // higher threshold => less (closer to 0) reduction
    // Reduction is never positive.
    CHECK (targetReductionDb (0.0f, -90.0f, 0.0f, 2.0f, 24.0f) <= 0.0f);
}

void test_resonance_base_strength()
{
    using namespace dcr::resonance;
    CHECK (feq (baseStrengthForRes (3),  0.6f));
    CHECK (feq (baseStrengthForRes (6),  1.2f));
    CHECK (feq (baseStrengthForRes (12), 2.4f));
    CHECK (feq (baseStrengthForRes (24), 4.0f));
    CHECK (feq (baseStrengthForRes (99), 4.0f));   // default = finest/tightest
}

} // namespace

int main()
{
    std::printf ("dcorerouter tests\n");

    test_ring_basic();
    test_ring_overflow_underflow();
    test_ring_wraparound_integrity();
    test_ring_clear();

    test_matrix_defaults();
    test_matrix_crosspoint();
    test_matrix_self_loop_block();
    test_matrix_mute_solo();
    test_matrix_dirty_generation();
    test_matrix_resize_clears_blocks();
    test_matrix_snapshot();

    test_groupgain_db_roundtrip();
    test_groupgain_clamp();
    test_groupgain_router_channel_gain();
    test_groupgain_bake_vca();
    test_groupgain_mode_switch_preserves_level();

    test_pdc_plan_disabled();
    test_pdc_plan_aligns_to_max();
    test_pdc_plan_no_latency();
    test_pdc_plan_empty();
    test_pdc_plan_negative_treated_as_zero();
    test_pdc_plan_cap_clamps_and_flags();

    test_pdc_delay_static();
    test_pdc_delay_change_settles();
    test_pdc_delay_idle_then_activate();
    test_pdc_delay_ramp_is_finite();

    test_resonance_node_freq();
    test_resonance_node_interp();
    test_resonance_target_reduction();
    test_resonance_base_strength();

    std::printf ("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

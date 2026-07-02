// Pure-logic regression tests for D-Router.  No JUCE, no audio device --
// these run headless in milliseconds and guard the deterministic core:
// the SPSC ring buffer and the routing matrix (gains, mute/solo, the
// virtual-device self-loop block).  Exit code != 0 on any failure (ctest).
//
// DSP tests that need JUCE (STFT/WOLA reconstruction, the pre-fader gain
// staging) are intentionally NOT here -- they belong in a JUCE-linked target;
// see the PR notes.

#include "DSP/Builtin/DeEsserMath.h"
#include "DSP/Builtin/RecorderNaming.h"
#include "DSP/Builtin/ResonanceMath.h"
#include "DSP/Builtin/SpectralNodeMath.h"
#include "DSP/Builtin/StereoMeterMath.h"
#include "Engine/AppInputResolver.h"
#include "Engine/DeviceRateChoice.h"
#include "Engine/FormatRestartGuard.h"
#include "Engine/MatrixInputPlan.h"
#include "Engine/PdcDelayLine.h"
#include "Engine/PdcPlan.h"
#include "Engine/RingAutoSize.h"
#include "Engine/RingBuffer.h"
#include "Routing/GroupGain.h"
#include "Routing/PanicController.h"
#include "Routing/RoutingMatrix.h"
#include "UI/Eased.h"
#include "Update/Version.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace
{

    int g_checks = 0;
    int g_fails = 0;

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        ++g_checks;                                                         \
        if (!(cond))                                                        \
        {                                                                   \
            ++g_fails;                                                      \
            std::printf ("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                   \
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
        CHECK (r.capacity() == 127);
        CHECK (r.readAvailable() == 0);
        CHECK (r.writeAvailable() == 127);

        float in[10];
        for (int i = 0; i < 10; ++i)
            in[i] = (float) i;
        CHECK (r.write (in, 10) == 10);
        CHECK (r.readAvailable() == 10);
        CHECK (r.writeAvailable() == 117);

        float out[10] = { 0 };
        CHECK (r.read (out, 10) == 10);
        for (int i = 0; i < 10; ++i)
            CHECK (feq (out[i], (float) i));
        CHECK (r.readAvailable() == 0);
    }

    void test_ring_overflow_underflow()
    {
        // Size rounds up to the next power of two that can hold AT LEAST the
        // requested count: request 8 -> bufferSize 16, capacity 15 (one slot is
        // reserved to disambiguate full vs empty).
        dcr::FloatRingBuffer r (8);
        CHECK (r.capacity() == 15);
        CHECK (r.bufferSize() == 16);
        CHECK (r.capacity() >= 8); // the contract: can hold the request

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
            for (int i = 0; i < wantW; ++i)
                buf[i] = nextWrite + (float) i;
            const size_t w = r.write (buf, (size_t) wantW);
            nextWrite += (float) w;
            wrote += (int) w;

            float ob[5];
            const int wantR = (iter % 5) + 1;
            const size_t got = r.read (ob, (size_t) wantR);
            for (size_t i = 0; i < got; ++i)
                CHECK (feq (ob[i], nextRead + (float) i));
            nextRead += (float) got;
            read += (int) got;
        }
        // drain the rest and verify the running counters stayed in lock-step.
        float ob[64];
        size_t got;
        while ((got = r.read (ob, 64)) > 0)
        {
            for (size_t i = 0; i < got; ++i)
                CHECK (feq (ob[i], nextRead + (float) i));
            nextRead += (float) got;
            read += (int) got;
        }
        CHECK (read == wrote);
        CHECK (feq (nextRead, nextWrite));
    }

    void test_ring_clear()
    {
        dcr::FloatRingBuffer r (16);
        float in[4] = { 1, 2, 3, 4 };
        r.write (in, 4);
        CHECK (r.readAvailable() == 4);
        r.clear();
        CHECK (r.readAvailable() == 0);
        CHECK (r.writeAvailable() == r.capacity());
    }

    // ---------------------------------------------------------------------------
    // RoutingMatrix
    // ---------------------------------------------------------------------------
    void test_matrix_defaults()
    {
        dcr::RoutingMatrix m;
        m.resize (4, 3);
        CHECK (m.getNumInputs() == 4);
        CHECK (m.getNumOutputs() == 3);
        for (int n = 0; n < 4; ++n)
        {
            CHECK (feq (m.getInputTrim (n), 1.0f));
            CHECK (!m.getInputMute (n));
            CHECK (!m.getInputSolo (n));
        }
        for (int o = 0; o < 3; ++o)
        {
            CHECK (feq (m.getOutputTrim (o), 1.0f));
            CHECK (!m.getOutputMute (o));
        }
        for (int o = 0; o < 3; ++o)
            for (int n = 0; n < 4; ++n)
            {
                CHECK (feq (m.getCrosspoint (o, n), 0.0f));
                CHECK (!m.isBlocked (o, n));
            }
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
        CHECK (feq (m.getCrosspoint (1, 2), 0.0f)); // not the transpose
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
        CHECK (feq (m.getCrosspoint (0, 0), 0.0f)); // blocking forces silence
        m.setCrosspoint (0, 0, 1.0f); // attempt to re-enable
        CHECK (feq (m.getCrosspoint (0, 0), 0.0f)); // ... is refused

        m.setBlocked (0, 0, false); // unblock
        m.setCrosspoint (0, 0, 1.0f);
        CHECK (feq (m.getCrosspoint (0, 0), 1.0f)); // now it takes
    }

    void test_matrix_mute_solo()
    {
        dcr::RoutingMatrix m;
        m.resize (3, 2);
        m.setInputMute (1, true);
        m.setOutputMute (0, true);
        m.setInputSolo (2, true);
        CHECK (m.getInputMute (1));
        CHECK (!m.getInputMute (0));
        CHECK (m.getOutputMute (0));
        CHECK (m.getInputSolo (2));
        CHECK (!m.getInputSolo (0));
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

        m.resize (2, 2); // re-resize must reset state
        CHECK (!m.isBlocked (0, 0));
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
    // PanicController (Phase C1) -- mirrors the pre-extraction MainComponent logic
    // ---------------------------------------------------------------------------
    void test_panic_engage_saves_and_mutes_all()
    {
        dcr::RoutingMatrix m;
        m.resize (3, 2);
        m.setInputMute (1, true); // one input already muted before panic

        dcr::PanicController p;
        CHECK (!p.isActive());
        CHECK (p.engage (m) == true);
        CHECK (p.isActive());
        CHECK (p.state() == dcr::PanicController::State::Active);
        for (int n = 0; n < 3; ++n)
            CHECK (m.getInputMute (n));
        for (int o = 0; o < 2; ++o)
            CHECK (m.getOutputMute (o));
    }

    void test_panic_engage_empty_matrix_is_noop()
    {
        dcr::RoutingMatrix m;
        m.resize (0, 0);
        dcr::PanicController p;
        CHECK (p.engage (m) == false);
        CHECK (!p.isActive());
    }

    void test_panic_release_restores_prior_state()
    {
        dcr::RoutingMatrix m;
        m.resize (3, 2);
        m.setInputMute (1, true);
        m.setOutputMute (0, true);

        dcr::PanicController p;
        p.engage (m);
        p.release (m);
        CHECK (!p.isActive());
        CHECK (!m.getInputMute (0));
        CHECK (m.getInputMute (1));
        CHECK (!m.getInputMute (2));
        CHECK (m.getOutputMute (0));
        CHECK (!m.getOutputMute (1));
    }

    void test_panic_release_when_inactive_is_noop()
    {
        dcr::RoutingMatrix m;
        m.resize (2, 2);
        m.setInputMute (0, true);
        dcr::PanicController p;
        p.release (m);
        CHECK (!p.isActive());
        CHECK (m.getInputMute (0));
        CHECK (!m.getInputMute (1));
    }

    void test_panic_forget_drops_saved_state_without_touching_matrix()
    {
        dcr::RoutingMatrix m;
        m.resize (2, 1);
        dcr::PanicController p;
        p.engage (m); // everything muted, prior (all-unmuted) state saved
        m.setInputMute (0, false); // user manually unmutes while panic active
        p.noteUserMuteChanged();
        CHECK (!p.isActive());
        CHECK (!m.getInputMute (0));
        CHECK (m.getInputMute (1));
        // re-engage now saves the CURRENT pattern; release returns to it,
        // proving the stale saved state was truly dropped.
        p.engage (m);
        p.release (m);
        CHECK (!m.getInputMute (0));
        CHECK (m.getInputMute (1));
    }

    void test_panic_forget_when_inactive_is_noop()
    {
        dcr::PanicController p;
        p.noteUserMuteChanged();
        CHECK (!p.isActive());
    }

    void test_panic_reset_clears_state_and_reports()
    {
        dcr::RoutingMatrix m;
        m.resize (2, 2);
        dcr::PanicController p;
        p.engage (m);
        CHECK (p.reset() == true);
        CHECK (!p.isActive());
        // reset() must NOT touch the matrix -- the caller is about to rebuild it,
        // and the saved indices belong to the old layout.
        CHECK (m.getInputMute (0));
        CHECK (m.getInputMute (1));
        CHECK (m.getOutputMute (0));
        CHECK (m.getOutputMute (1));
        CHECK (p.reset() == false);
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
        CHECK (feq (clampTrimDb (99.0f), 12.0f)); // ceiling
        CHECK (feq (clampTrimDb (-99.0f), -60.0f)); // floor
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
            const float trimLin = dbToLin (trimDb);
            const float routerLvl = trimLin * dbToLin (fader); // Router-mode level
            const float bakedLin = dbToLin (bakeVcaTrimDb (linToDb (trimLin), fader)); // VCA-mode level
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
        CHECK (!p.clamped);
        CHECK (p.compDelay.size() == 3);
        for (int d : p.compDelay)
            CHECK (d == 0);
    }

    void test_pdc_plan_aligns_to_max()
    {
        std::vector<int> lat { 1024, 0, 512 };
        auto p = dcr::computePdcPlan (lat, true, 48000);
        CHECK (p.maxLatency == 1024);
        CHECK (!p.clamped);
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
        for (int d : p.compDelay)
            CHECK (d == 0);
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
        CHECK (p.compDelay[0] == 100); // negative latency floored to 0
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

    void test_pdc_plan_device_domains()
    {
        // Two devices (domains 0 and 1): each aligns to ITS OWN slowest chain --
        // device 1's clean outputs must not inherit device 0's plugin latency.
        std::vector<int> lat { 1024, 0, 512, 0 };
        std::vector<int> dom { 0, 0, 1, 1 };
        auto p = dcr::computePdcPlan (lat, dom, true, 48000);
        CHECK (p.maxLatency == 1024); // global worst, for the status display
        CHECK (p.compDelay[0] == 0);
        CHECK (p.compDelay[1] == 1024); // aligned within device 0
        CHECK (p.compDelay[2] == 0);
        CHECK (p.compDelay[3] == 512); // aligned within device 1 (NOT 1024)
        // Per-domain invariant: latency + compensation is equal within a domain.
        CHECK (lat[0] + p.compDelay[0] == lat[1] + p.compDelay[1]);
        CHECK (lat[2] + p.compDelay[2] == lat[3] + p.compDelay[3]);
    }

    void test_pdc_plan_domain_with_no_latency_stays_untouched()
    {
        // A device with no latent plugin gets zero compensation everywhere even
        // while another device is being aligned.
        std::vector<int> lat { 2048, 0, 0, 0 };
        std::vector<int> dom { 7, 7, 3, 3 }; // labels are arbitrary
        auto p = dcr::computePdcPlan (lat, dom, true, 48000);
        CHECK (p.compDelay[0] == 0);
        CHECK (p.compDelay[1] == 2048);
        CHECK (p.compDelay[2] == 0);
        CHECK (p.compDelay[3] == 0);
    }

    void test_pdc_plan_mismatched_domains_fall_back_to_global()
    {
        // Wrong-sized domain vector (defensive path) -> single global domain,
        // identical to the legacy 3-arg call.
        std::vector<int> lat { 1024, 0, 512 };
        std::vector<int> dom { 0, 1 }; // size mismatch
        auto p = dcr::computePdcPlan (lat, dom, true, 48000);
        auto legacy = dcr::computePdcPlan (lat, true, 48000);
        CHECK (p.compDelay == legacy.compDelay);
        CHECK (p.maxLatency == legacy.maxLatency);
    }

    void test_pdc_plan_domains_disabled_and_clamped()
    {
        // Disabled: all zeros regardless of domains.
        std::vector<int> lat { 1024, 0 };
        std::vector<int> dom { 0, 1 };
        auto off = dcr::computePdcPlan (lat, dom, false, 48000);
        for (int d : off.compDelay)
            CHECK (d == 0);
        // Clamp + flag still work per domain.
        std::vector<int> lat2 { 60000, 0, 100, 0 };
        std::vector<int> dom2 { 0, 0, 1, 1 };
        auto p = dcr::computePdcPlan (lat2, dom2, true, 48000);
        CHECK (p.clamped);
        CHECK (p.compDelay[1] == 48000); // device 0 aligned to the clamped cap
        CHECK (p.compDelay[3] == 100); // device 1 untouched by the clamp
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
        for (int i = 0; i < N; ++i)
        {
            in[(size_t) i] = (float) (i + 1);
            out[(size_t) i] = in[(size_t) i];
        }

        for (int off = 0; off < N; off += 128)
            dl.process (out.data() + off, std::min (128, N - off));

        CHECK (dl.getCurrentDelay() == K);
        for (int i = K + 64; i < N - 8; ++i)
            CHECK (feq (out[(size_t) i], in[(size_t) (i - K)]));
    }

    void test_pdc_delay_static()
    {
        check_settled_delay (0); // passthrough
        check_settled_delay (1);
        check_settled_delay (100);
        check_settled_delay (2000); // == maxDelay (deepest valid tap)
    }

    void test_pdc_delay_change_settles()
    {
        dcr::PdcDelayLine dl;
        dl.prepare (2000, 128, 8);

        const int N = 8000;
        std::vector<float> in ((size_t) N), out ((size_t) N);
        for (int i = 0; i < N; ++i)
        {
            in[(size_t) i] = (float) (i + 1);
            out[(size_t) i] = in[(size_t) i];
        }

        auto run = [&] (int from, int to) {
            for (int i = from; i < to; i += 128)
                dl.process (out.data() + i, std::min (128, to - i));
        };

        dl.setTargetDelay (50);
        run (0, 4000);
        CHECK (dl.getCurrentDelay() == 50);
        for (int i = 50 + 64; i < 3900; ++i)
            CHECK (feq (out[(size_t) i], in[(size_t) (i - 50)]));

        dl.setTargetDelay (300);
        run (4000, N);
        CHECK (dl.getCurrentDelay() == 300);
        for (int i = 4000 + 300 + 64; i < N - 64; ++i)
            CHECK (feq (out[(size_t) i], in[(size_t) (i - 300)]));
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
        for (int i = 0; i < N; ++i)
        {
            in[(size_t) i] = (float) (i + 1);
            out[(size_t) i] = in[(size_t) i];
        }
        auto run = [&] (int from, int to) {
            for (int i = from; i < to; i += 128)
                dl.process (out.data() + i, std::min (128, to - i));
        };

        run (0, 4000); // delay 0: passthrough via fast path
        for (int i = 0; i < 3999; ++i)
            CHECK (feq (out[(size_t) i], in[(size_t) i]));

        dl.setTargetDelay (120); // activate
        run (4000, N);
        CHECK (dl.getCurrentDelay() == 120);
        for (int i = 4000 + 120 + 64; i < N - 64; ++i)
            CHECK (feq (out[(size_t) i], in[(size_t) (i - 120)]));
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
        for (float v : buf)
            CHECK (std::isfinite (v));
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
        CHECK (feq (nodeFreq (0, 31), 20.0f));
        CHECK (feq (nodeFreq (30, 31), 20000.0f, 1.0e-2f));
        CHECK (feq (nodeFreq (10, 31), 200.0f, 1.0e-2f)); // 1000^(1/3) == 10
        CHECK (feq (nodeFreq (20, 31), 2000.0f, 1.0e-2f)); // 1000^(2/3) == 100
        CHECK (feq (nodeFreq (15, 31), 632.4555f, 1.0e-2f)); // 20*sqrt(1000)
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
        const float a = targetReductionDb (-30.0f, -50.0f, 10.0f, 1.0f, 24.0f); // excess 10 -> -10
        const float b = targetReductionDb (-30.0f, -50.0f, 18.0f, 1.0f, 24.0f); // excess  2 -> -2
        CHECK (feq (a, -10.0f));
        CHECK (feq (b, -2.0f));
        CHECK (b > a); // higher threshold => less (closer to 0) reduction
        // Reduction is never positive.
        CHECK (targetReductionDb (0.0f, -90.0f, 0.0f, 2.0f, 24.0f) <= 0.0f);
    }

    void test_resonance_base_strength()
    {
        using namespace dcr::resonance;
        CHECK (feq (baseStrengthForRes (3), 0.6f));
        CHECK (feq (baseStrengthForRes (6), 1.2f));
        CHECK (feq (baseStrengthForRes (12), 2.4f));
        CHECK (feq (baseStrengthForRes (24), 4.0f));
        CHECK (feq (baseStrengthForRes (99), 4.0f)); // default = finest/tightest
    }

    // ---------------------------------------------------------------------------
    // DeEsserMath (soft-knee gain-reduction curve for the sibilance band).
    // The stateful envelope/attack-release and the IIR band filter live in the
    // processor; only the static curve is unit-tested here.
    // ---------------------------------------------------------------------------
    void test_deesser_gain_curve()
    {
        using namespace dcr::deess;
        // Below the knee region -> no reduction.
        CHECK (feq (gainReductionDb (-40.0f, -30.0f, 4.0f, 6.0f, -24.0f), 0.0f));
        // Hard knee (knee = 0), well above threshold: GR = -(1-1/ratio)*over.
        // over = 10, ratio 4 -> slope 0.75 -> -7.5 dB.
        CHECK (feq (gainReductionDb (-20.0f, -30.0f, 4.0f, 0.0f, -24.0f), -7.5f, 1.0e-4f));
        // ratio 1 -> no reduction ever.
        CHECK (feq (gainReductionDb (0.0f, -30.0f, 1.0f, 0.0f, -24.0f), 0.0f));
        // Clamped to the range ceiling: -(0.75*40) = -30 -> clamped to -12.
        CHECK (feq (gainReductionDb (10.0f, -30.0f, 4.0f, 0.0f, -12.0f), -12.0f));
        // Never positive.
        CHECK (gainReductionDb (20.0f, -40.0f, 8.0f, 6.0f, -24.0f) <= 0.0f);
        // More reduction as level rises (monotonic, below the ceiling).
        const float lo = gainReductionDb (-25.0f, -30.0f, 4.0f, 0.0f, -24.0f); // over 5 -> -3.75
        const float hi = gainReductionDb (-20.0f, -30.0f, 4.0f, 0.0f, -24.0f); // over 10 -> -7.5
        CHECK (hi < lo);
        // Soft knee is C1-continuous with the hard-knee curve at the corners.
        const float ratio = 4.0f, knee = 12.0f, thr = -30.0f, rng = -48.0f;
        const float upper = gainReductionDb (thr + knee * 0.5f, thr, ratio, knee, rng); // top corner
        const float upperHard = gainReductionDb (thr + knee * 0.5f, thr, ratio, 0.0f, rng);
        CHECK (feq (upper, upperHard, 1.0e-4f)); // knee meets the hard curve at +knee/2
        const float lower = gainReductionDb (thr - knee * 0.5f, thr, ratio, knee, rng); // bottom corner
        CHECK (feq (lower, 0.0f, 1.0e-4f)); // and meets 0 at -knee/2
        // Knee centre (over = 0, knee = 12): x = 6, gr = -slope*36/24 = -0.75*1.5.
        const float mid = gainReductionDb (thr, thr, ratio, knee, rng);
        CHECK (feq (mid, -1.125f, 1.0e-4f));
        // dB/linear round-trip.
        CHECK (feq (dbToGain (0.0f), 1.0f));
        CHECK (feq (gainToDb (dbToGain (-6.0f)), -6.0f, 1.0e-3f));

        // Auto threshold: manual mode returns the param unchanged; auto mode
        // sits relative to the running average (default -30 param == at avg).
        CHECK (feq (effectiveThresholdDb (-30.0f, -42.0f, false), -30.0f));
        CHECK (feq (effectiveThresholdDb (-30.0f, -42.0f, true), -42.0f)); // offset 0
        CHECK (feq (effectiveThresholdDb (-24.0f, -42.0f, true), -36.0f)); // +6 above avg
    }

    // ---------------------------------------------------------------------------
    // RingAutoSize (per-device ring sizing from hardware latency, Safe..Safest).
    // ---------------------------------------------------------------------------
    void test_ring_auto_size()
    {
        using namespace dcr::ringauto;

        // Zero hardware latency -> exactly the "Safe" preset (Eng 3/6, Dev 4/8,
        // prefill 8).  48k engine == 48k device, 128-sample buffer.
        auto safe = computeAutoRingPlan (128, 48000.0, 128, 48000.0, 0, 0);
        CHECK (safe.inRingSamples == 512); // max(3*128, 4*128)
        CHECK (safe.outRingSamples == 1024); // max(6*128, 8*128)
        CHECK (safe.prefillBlocks == 8);

        // Moderate latency: hwIn 256 (slack 2), hwOut 512 (slack 4).
        auto mid = computeAutoRingPlan (128, 48000.0, 128, 48000.0, 256, 512);
        CHECK (mid.inRingSamples == 768); // inEng 4, inDev 6 -> max(512, 768)
        CHECK (mid.outRingSamples == 1536); // outEng 10, outDev 12 -> max(1280, 1536)
        CHECK (mid.prefillBlocks == 10); // ceil(512/128)+6
        // Auto never drops below the Safe floor and never exceeds Safest.
        CHECK (mid.inRingSamples >= safe.inRingSamples);
        CHECK (mid.outRingSamples >= safe.outRingSamples);

        // Huge latency clamps to the Safest envelope (Dev 12, Eng 12) + prefill 32.
        auto hi = computeAutoRingPlan (128, 48000.0, 128, 48000.0, 4096, 4096);
        CHECK (hi.outRingSamples == 1536); // max(12*128, 12*128)
        CHECK (hi.prefillBlocks == 32);

        // Sample-rate conversion widens the device-buffer branch by engineSr/devSr.
        auto src = computeAutoRingPlan (128, 48000.0, 128, 44100.0, 0, 0);
        CHECK (src.outRingSamples == (int) std::ceil (8.0 * 128.0 * 48000.0 / 44100.0)); // 1115
        CHECK (src.outRingSamples > safe.outRingSamples);
    }

    // ---------------------------------------------------------------------------
    // StereoMeterMath (HF-tilt visualization: freq-to-norm axis + high-lift gain)
    // ---------------------------------------------------------------------------
    void test_stereometer_freq_to_norm()
    {
        using dcr::builtin::freqToNorm;
        const float lo = 20.0f, nyq = 24000.0f;
        CHECK (std::abs (freqToNorm (lo, lo, nyq) - 0.0f) < 1e-5f); // bottom -> 0
        CHECK (std::abs (freqToNorm (nyq, lo, nyq) - 1.0f) < 1e-5f); // top -> 1
        CHECK (freqToNorm (10.0f, lo, nyq) == 0.0f); // below clamps
        CHECK (freqToNorm (48000.0f, lo, nyq) == 1.0f); // above clamps
        CHECK (freqToNorm (100.0f, lo, nyq) < freqToNorm (1000.0f, lo, nyq)); // monotonic
        CHECK (freqToNorm (1000.0f, lo, nyq) < freqToNorm (10000.0f, lo, nyq));
        const float n1k = freqToNorm (1000.0f, lo, nyq); // ln(50)/ln(1200) ~= 0.55
        CHECK (n1k > 0.4f && n1k < 0.7f);
    }

    void test_stereometer_high_lift_gain()
    {
        using dcr::builtin::highLiftGain;
        const float nyq = 24000.0f, pivot = 2000.0f;
        // The bug-fix regression: at/below the pivot the gain is ~1 (bass/mids untouched).
        CHECK (std::abs (highLiftGain (100.0f, pivot, nyq, 0.5f) - 1.0f) < 1e-5f);
        CHECK (std::abs (highLiftGain (500.0f, pivot, nyq, 0.5f) - 1.0f) < 1e-5f);
        CHECK (std::abs (highLiftGain (2000.0f, pivot, nyq, 0.5f) - 1.0f) < 1e-5f);
        // Above the pivot: > 1 and strictly increasing with frequency.
        const float g5k = highLiftGain (5000.0f, pivot, nyq, 0.5f);
        const float g10k = highLiftGain (10000.0f, pivot, nyq, 0.5f);
        const float g20k = highLiftGain (20000.0f, pivot, nyq, 0.5f);
        CHECK (g5k > 1.0f);
        CHECK (g5k < g10k);
        CHECK (g10k < g20k);
        // strength 0 -> no lift anywhere; full strength lifts more than half.
        CHECK (std::abs (highLiftGain (20000.0f, pivot, nyq, 0.0f) - 1.0f) < 1e-5f);
        CHECK (highLiftGain (20000.0f, pivot, nyq, 1.0f) > g20k);
        // bounded by 1 + strength*kHighLiftMax.
        CHECK (g20k <= 1.0f + 0.5f * dcr::builtin::kHighLiftMax + 1e-4f);
    }

    void test_stereometer_high_lift_knee()
    {
        using dcr::builtin::highLiftGain;
        const float nyq = 24000.0f, pivot = 2000.0f, str = 0.5f;
        // knee == 0 reproduces the hard knee (flat at/below the pivot).
        CHECK (std::abs (highLiftGain (1000.0f, pivot, nyq, str, 0.0f) - 1.0f) < 1e-5f);
        CHECK (std::abs (highLiftGain (2000.0f, pivot, nyq, str, 0.0f) - 1.0f) < 1e-5f);
        CHECK (std::abs (highLiftGain (8000.0f, pivot, nyq, str, 0.0f)
                         - highLiftGain (8000.0f, pivot, nyq, str))
               < 1e-5f); // default arg == hard knee
        // knee > 0 rounds the corner: a touch of lift AT and just BELOW the pivot.
        CHECK (highLiftGain (2000.0f, pivot, nyq, str, 0.5f) > 1.0f);
        CHECK (highLiftGain (1500.0f, pivot, nyq, str, 0.5f) > 1.0f);
        // still monotonic in frequency with a knee.
        CHECK (highLiftGain (3000.0f, pivot, nyq, str, 0.5f)
               < highLiftGain (12000.0f, pivot, nyq, str, 0.5f));
        // and never exceeds the hard ceiling 1 + strength*kHighLiftMax.
        CHECK (highLiftGain (20000.0f, pivot, nyq, str, 1.0f)
               <= 1.0f + str * dcr::builtin::kHighLiftMax + 1e-4f);
        // strength 0 -> no lift regardless of knee.
        CHECK (std::abs (highLiftGain (8000.0f, pivot, nyq, 0.0f, 0.8f) - 1.0f) < 1e-5f);
    }

    void test_stereometer_db_to_norm_y()
    {
        using dcr::builtin::dbToNormY;
        // ceiling -> +1 (top), floor -> -1 (bottom), midpoint -> 0.
        CHECK (std::abs (dbToNormY (0.0f, -60.0f, 0.0f) - 1.0f) < 1e-5f);
        CHECK (std::abs (dbToNormY (-60.0f, -60.0f, 0.0f) - (-1.0f)) < 1e-5f);
        CHECK (std::abs (dbToNormY (-30.0f, -60.0f, 0.0f) - 0.0f) < 1e-5f);
        // clamps outside the window.
        CHECK (dbToNormY (10.0f, -60.0f, 0.0f) == 1.0f);
        CHECK (dbToNormY (-90.0f, -60.0f, 0.0f) == -1.0f);
        // degenerate range stays finite (no divide-by-zero).
        CHECK (dbToNormY (-30.0f, -30.0f, -30.0f) == -1.0f);
    }

    // Per-octave (RTA) display weighting: pink noise's per-bin amplitude falls as
    // 1/sqrt(f); the weight must exactly cancel that so pink reads FLAT, and it
    // must add +3 dB/oct (x sqrt(2) per octave) to flat-spectral-density material.
    void test_stereometer_per_octave_weight()
    {
        using dcr::builtin::perOctaveWeight;
        const float ref = 1000.0f;
        CHECK (std::abs (perOctaveWeight (ref, ref) - 1.0f) < 1e-6f); // unity at ref
        // Pink amplitude ~ 1/sqrt(f): weighted product is constant across the band.
        const float p100 = (1.0f / std::sqrt (100.0f)) * perOctaveWeight (100.0f, ref);
        const float p1k = (1.0f / std::sqrt (1000.0f)) * perOctaveWeight (1000.0f, ref);
        const float p10k = (1.0f / std::sqrt (10000.0f)) * perOctaveWeight (10000.0f, ref);
        CHECK (std::abs (p100 - p1k) < 1e-6f);
        CHECK (std::abs (p10k - p1k) < 1e-6f);
        // One octave up = x sqrt(2) on amplitude (+3 dB).
        CHECK (std::abs (perOctaveWeight (2000.0f, ref) / perOctaveWeight (1000.0f, ref)
                         - std::sqrt (2.0f))
               < 1e-5f);
        // Degenerate inputs stay harmless.
        CHECK (perOctaveWeight (0.0f, ref) == 1.0f);
        CHECK (perOctaveWeight (1000.0f, 0.0f) == 1.0f);
    }

    // Spectral node-curve preset restore is an untrusted surface: a hand-edited or
    // corrupt blob can carry NaN/Inf or a wild value, which would latch into the
    // per-bin smoother and poison the FFT output with NaN forever.  sanitizeNodeDb
    // is the guard on the way in.
    void test_spectral_sanitize_node_db()
    {
        using dcr::spectral::kNodeLimitDb;
        using dcr::spectral::sanitizeNodeDb;

        // In-range values pass through unchanged.
        CHECK (feq (sanitizeNodeDb (0.0), 0.0f));
        CHECK (feq (sanitizeNodeDb (6.0), 6.0f));
        CHECK (feq (sanitizeNodeDb (-12.5), -12.5f));
        // Exactly at the limits.
        CHECK (feq (sanitizeNodeDb ((double) kNodeLimitDb), kNodeLimitDb));
        CHECK (feq (sanitizeNodeDb (-(double) kNodeLimitDb), -kNodeLimitDb));
        // Out of range clamps to the limit (values an untrusted preset might carry).
        CHECK (feq (sanitizeNodeDb (1.0e308), kNodeLimitDb));
        CHECK (feq (sanitizeNodeDb (-500.0), -kNodeLimitDb));
        CHECK (feq (sanitizeNodeDb (18.001), kNodeLimitDb));
        // Non-finite -> neutral 0 (the bug: NaN/Inf otherwise reached the FFT path,
        // and juce::jlimit does NOT reject NaN).
        CHECK (feq (sanitizeNodeDb ((double) NAN), 0.0f));
        CHECK (feq (sanitizeNodeDb ((double) INFINITY), 0.0f));
        CHECK (feq (sanitizeNodeDb (-(double) INFINITY), 0.0f));
        // Whatever comes in, the result is always finite and within the node range.
        const double probes[] = { (double) NAN, (double) INFINITY, -(double) INFINITY, 1.0e308, -1.0e308, 50.0, -50.0, 3.3 };
        for (double v : probes)
        {
            const float r = sanitizeNodeDb (v);
            CHECK (std::isfinite (r));
            CHECK (r >= -kNodeLimitDb && r <= kNodeLimitDb);
        }
    }

    // GitHub auto-updater version comparison.  Must be numeric (not lexical), treat a
    // prerelease as older than the same stable, and refuse to call a malformed tag
    // "newer" (so a garbage release name can never trigger a spurious update prompt).
    void test_update_version_compare()
    {
        using namespace dcr::update;

        CHECK (isNewer ("0.2.0", "0.1.0"));
        CHECK (isNewer ("v0.2.0", "0.1.0")); // leading 'v' tolerated
        CHECK (isNewer ("0.1.1", "0.1.0"));
        CHECK (isNewer ("1.10.0", "1.9.0")); // numeric, not lexical ("10" > "9")
        CHECK (!isNewer ("1.9.0", "1.10.0"));
        CHECK (isNewer ("0.2.0", "0.2.0-beta")); // stable > prerelease
        CHECK (!isNewer ("0.2.0-beta", "0.2.0")); // prerelease < stable
        CHECK (isNewer ("0.2.0-beta.2", "0.2.0-beta.1"));
        CHECK (!isNewer ("0.1.0", "0.1.0")); // equal -> not newer
        CHECK (!isNewer ("0.1.0-beta", "0.1.0-beta"));
        CHECK (!isNewer ("garbage", "0.1.0")); // malformed candidate -> never newer
        CHECK (!isNewer ("0.2.0", "garbage")); // malformed current  -> never newer
        CHECK (!isNewer ("", "0.1.0"));

        const Version v = parseVersion ("v0.2.0-beta");
        CHECK (v.valid && v.major == 0 && v.minor == 2 && v.patch == 0 && v.prerelease == "beta");
        const Version w = parseVersion ("garbage");
        CHECK (!w.valid);
    }

    // ---------------------------------------------------------------------------
    // AppInputResolver (auto-reattach: reconcile configured app sources with the
    // set of processes currently running, keyed by bundle id).
    // ---------------------------------------------------------------------------
    void test_appinput_reconcile_attach_when_running()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 0 } }; // offline
        std::vector<RunningProcess> running { { "com.google.Chrome", 42 } };
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 1);
        CHECK (cmds[0].type == CommandType::Attach);
        CHECK (cmds[0].sourceIndex == 0);
        CHECK (cmds[0].processId == 42);
    }

    void test_appinput_reconcile_noop_when_offline_and_not_running()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.apple.Music", 0 } };
        std::vector<RunningProcess> running {}; // nothing running
        auto cmds = reconcile (sources, running);
        CHECK (cmds.empty());
    }

    void test_appinput_reconcile_steady_state_no_command()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 42 } }; // attached to 42
        std::vector<RunningProcess> running { { "com.google.Chrome", 42 } };
        auto cmds = reconcile (sources, running);
        CHECK (cmds.empty());
    }

    void test_appinput_reconcile_detach_when_quit()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 42 } }; // attached
        std::vector<RunningProcess> running {}; // Chrome quit
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 1);
        CHECK (cmds[0].type == CommandType::Detach);
        CHECK (cmds[0].sourceIndex == 0);
    }

    void test_appinput_reconcile_relaunch_detach_then_attach()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 42 } }; // attached to old pid
        std::vector<RunningProcess> running { { "com.google.Chrome", 99 } }; // relaunched
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 2);
        CHECK (cmds[0].type == CommandType::Detach);
        CHECK (cmds[0].sourceIndex == 0);
        CHECK (cmds[1].type == CommandType::Attach);
        CHECK (cmds[1].sourceIndex == 0);
        CHECK (cmds[1].processId == 99);
    }

    void test_appinput_reconcile_multiple_sources_mixed()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources {
            { "com.google.Chrome", 0 }, // offline -> attach
            { "com.apple.Music", 7 }, // attached, still running -> noop
            { "com.foo.Game", 12 }, // attached, quit -> detach
        };
        std::vector<RunningProcess> running {
            { "com.google.Chrome", 42 },
            { "com.apple.Music", 7 },
        };
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 2);
        CHECK (cmds[0].type == CommandType::Attach);
        CHECK (cmds[0].sourceIndex == 0);
        CHECK (cmds[0].processId == 42);
        CHECK (cmds[1].type == CommandType::Detach);
        CHECK (cmds[1].sourceIndex == 2);
    }

    // ---------------------------------------------------------------------------
    // MatrixInputPlan (per-input read/silence/stall decision).  Hardware inputs
    // gate the matrix (stall until a full block is ready); app inputs NEVER stall
    // -- an offline or warming-up app contributes silence instead of freezing all
    // audio.
    // ---------------------------------------------------------------------------
    void test_matrixinput_hardware_reads_when_full()
    {
        auto p = dcr::planMatrixInput (/*isAppInput*/ false, /*attached*/ false, /*avail*/ 128, /*block*/ 128);
        CHECK (p.action == dcr::MatrixInputAction::Read);
        CHECK (!p.stalls);
    }

    void test_matrixinput_hardware_stalls_when_underfull()
    {
        auto p = dcr::planMatrixInput (false, false, 64, 128);
        CHECK (p.stalls); // hardware underfull -> the matrix waits
    }

    void test_matrixinput_app_reads_when_attached_and_full()
    {
        auto p = dcr::planMatrixInput (true, /*attached*/ true, 128, 128);
        CHECK (p.action == dcr::MatrixInputAction::Read);
        CHECK (!p.stalls);
    }

    void test_matrixinput_app_silence_when_attached_but_underfull()
    {
        auto p = dcr::planMatrixInput (true, true, 64, 128);
        CHECK (p.action == dcr::MatrixInputAction::Silence);
        CHECK (!p.stalls); // never stalls the matrix
    }

    void test_matrixinput_app_silence_when_detached()
    {
        auto p = dcr::planMatrixInput (true, /*attached*/ false, 9999, 128);
        CHECK (p.action == dcr::MatrixInputAction::Silence);
        CHECK (!p.stalls);
    }

    // Output backpressure: the matrix stalls when the output ring can't take a
    // full block.  Regression for the app-input-only spin: with no gating
    // hardware input, this is the only thing that paces the matrix thread.
    void test_matrix_output_backpressure()
    {
        using dcr::matrixOutputStalls;
        // Room for a full block (or more) -> produce, don't stall.
        CHECK (matrixOutputStalls (128, 128) == false);
        CHECK (matrixOutputStalls (256, 128) == false);
        // Less than a block of room -> stall (wait for the device to drain).
        CHECK (matrixOutputStalls (127, 128) == true);
        CHECK (matrixOutputStalls (1, 128) == true);
        CHECK (matrixOutputStalls (0, 128) == true);
    }

    // ---------------------------------------------------------------------------
    // RecorderNaming (JUCE-free recording-file naming)
    // ---------------------------------------------------------------------------
    void test_recorder_naming()
    {
        using namespace dcr::recorder;
        CHECK (extensionForFormat (0) == "wav");
        CHECK (extensionForFormat (1) == "flac");
        CHECK (extensionForFormat (2) == "m4a");
        CHECK (extensionForFormat (99) == "wav"); // out-of-range clamps

        CHECK (sanitizePrefix ("Vocals") == "Vocals");
        CHECK (sanitizePrefix ("a/b:c") == "a_b_c"); // unsafe -> '_'
        CHECK (sanitizePrefix ("") == "Recording"); // empty -> fallback
        CHECK (sanitizePrefix ("   ") == "Recording"); // whitespace -> fallback
        CHECK (sanitizePrefix ("__Lead__") == "Lead"); // trim _ . space
        CHECK (sanitizePrefix ("My Take 1") == "My Take 1"); // spaces kept

        CHECK (makeFileName ("Drums", 2026, 6, 23, 14, 30, 5, 0)
               == "Drums_2026-06-23_14-30-05.wav");
        CHECK (makeFileName ("", 2026, 12, 1, 9, 8, 7, 2)
               == "Recording_2026-12-01_09-08-07.m4a"); // zero-pad + fallback
        CHECK (makeFileName ("Take", 2026, 1, 1, 0, 0, 0, 1)
               == "Take_2026-01-01_00-00-00.flac"); // FLAC ext + all-zero time
    }

    // ---------------------------------------------------------------------------
    // Recorder channel count -- how many channels a take is written with.
    // ---------------------------------------------------------------------------
    void test_recorder_channel_count()
    {
        using dcr::recorder::recordChannelCount;
        // A per-channel insert runs in a "mono host" that duplicates the mono
        // signal across a stereo scratch (L == R).  Collapse it to one true
        // mono channel regardless of the presented (duplicated) width.
        CHECK (recordChannelCount (true, 2) == 1); // the duplicated-mono case
        CHECK (recordChannelCount (true, 1) == 1);
        CHECK (recordChannelCount (true, 0) == 1); // never zero channels

        // A group host presents its true N-channel buffer -> record as-is.
        CHECK (recordChannelCount (false, 2) == 2); // stereo group -> stereo
        CHECK (recordChannelCount (false, 6) == 6); // N-ch group -> N
        CHECK (recordChannelCount (false, 1) == 1); // (defensive) 1-ch group
        CHECK (recordChannelCount (false, 0) == 1); // never zero channels
    }

    // ---------------------------------------------------------------------------
    // chooseDeviceSampleRate -- adopt the device's current rate, don't force it.
    // ---------------------------------------------------------------------------
    void test_device_rate_choice()
    {
        using dcr::chooseDeviceSampleRate;
        const std::vector<double> common { 16000, 24000, 44100, 48000, 96000 };

        // Regression for the shared-device sample-rate war: another app put the
        // device at 24k while our engine runs at 48k.  We MUST follow the device
        // (24k), not yank it back to 48k -- yanking is what caused the restart
        // ping-pong that broke the other app's stream and froze the machine.
        CHECK (chooseDeviceSampleRate (common, 48000.0, 24000.0) == 24000.0);

        // Matched case: device already at the engine rate -> open at engine rate.
        CHECK (chooseDeviceSampleRate (common, 48000.0, 48000.0) == 48000.0);

        // Device at 44.1k, engine 48k -> follow the device; the SRC bridges it.
        CHECK (chooseDeviceSampleRate (common, 48000.0, 44100.0) == 44100.0);

        // No usable current rate reported -> fall back to the engine rate.
        CHECK (chooseDeviceSampleRate (common, 48000.0, 0.0) == 48000.0);
        CHECK (chooseDeviceSampleRate (common, 48000.0, -1.0) == 48000.0);

        // Fallback target unsupported -> nearest supported wins (current<=0 so
        // target == engine == 88200; nearest of {44100,96000} is 96000).
        const std::vector<double> sparse { 44100, 96000 };
        CHECK (chooseDeviceSampleRate (sparse, 88200.0, 0.0) == 96000.0);

        // Empty supported list -> pass the target through untouched.
        const std::vector<double> none {};
        CHECK (chooseDeviceSampleRate (none, 48000.0, 24000.0) == 24000.0);
        CHECK (chooseDeviceSampleRate (none, 48000.0, 0.0) == 48000.0);

        // Within 1 Hz counts as supported (drivers report e.g. 47999.9 vs 48000).
        const std::vector<double> jittered { 47999.9, 96000 };
        CHECK (chooseDeviceSampleRate (jittered, 48000.0, 48000.0) == 47999.9);
    }

    // ---------------------------------------------------------------------------
    // FormatRestartGuard -- rate-limit watchdog restarts; never spin the engine.
    // ---------------------------------------------------------------------------
    void test_format_restart_guard()
    {
        // First change always restarts; an immediate repeat is debounced; after
        // the debounce interval it is allowed again.
        {
            dcr::FormatRestartGuard g;
            CHECK (g.allowRestart (1000.0) == true);
            CHECK (g.allowRestart (1100.0) == false);
            CHECK (g.allowRestart (1000.0 + g.minIntervalMs + 1.0) == true);
        }

        // Burst cap: only maxInWindow restarts are allowed inside one window,
        // then the guard backs off instead of spinning the engine.
        {
            dcr::FormatRestartGuard g;
            double t = 0.0;
            int allowed = 0;
            for (int i = 0; i < 12; ++i)
            {
                if (g.allowRestart (t))
                    ++allowed;
                t += g.minIntervalMs + 1.0;
                if (t > g.windowMs)
                    break;
            }
            CHECK (allowed == g.maxInWindow);
            CHECK (g.isBackedOff() == true);
        }

        // Self-heal: a change long after the cooldown re-arms automatically.
        {
            dcr::FormatRestartGuard g;
            CHECK (g.allowRestart (0.0) == true);
            double t = 0.0;
            for (int i = 0; i < 8; ++i)
            {
                g.allowRestart (t);
                t += g.minIntervalMs + 1.0;
            }
            CHECK (g.isBackedOff() == true);
            CHECK (g.allowRestart (t + g.cooldownMs + 1.0) == true);
            CHECK (g.isBackedOff() == false);
        }

        // reset() re-arms immediately (e.g. after a manual Reset).
        {
            dcr::FormatRestartGuard g;
            for (int i = 0; i < 8; ++i)
                g.allowRestart ((double) i * (g.minIntervalMs + 1.0));
            CHECK (g.isBackedOff() == true);
            g.reset();
            CHECK (g.isBackedOff() == false);
            CHECK (g.allowRestart (1.0e6) == true);
        }
    }

    // ---------------------------------------------------------------------------
    // Eased (shared UI animation value)
    // ---------------------------------------------------------------------------
    void test_eased_snap_and_rest()
    {
        dcr::Eased e;
        CHECK (e.atRest()); // fresh: current == target == 0
        e.snap (5.0);
        CHECK (feq ((float) e.current, 5.0f));
        CHECK (feq ((float) e.target, 5.0f));
        CHECK (e.atRest());
        // snap onto a target it was mid-chase toward.
        e.to (10.0);
        CHECK (!e.atRest());
        e.snap (10.0);
        CHECK (e.atRest());
    }

    void test_eased_converges_and_stops()
    {
        dcr::Eased e;
        e.snap (0.0);
        e.to (100.0);

        int frames = 0;
        bool moving = true;
        double prev = e.current;
        while (moving && frames < 1000)
        {
            moving = e.step(); // default 0.30 / eps 0.5
            CHECK (e.current >= prev - 1.0e-9); // monotonic approach from below
            CHECK (e.current <= 100.0 + 1.0e-9); // never overshoots
            prev = e.current;
            ++frames;
        }
        CHECK (!moving); // step() reported settled
        CHECK (e.atRest()); // landed exactly on target
        CHECK (feq ((float) e.current, 100.0f));
        CHECK (frames < 60); // ~10-frame ease, not spinning
        CHECK (frames > 1);
    }

    void test_eased_downward_and_epsilon_snap()
    {
        dcr::Eased e;
        e.snap (10.0);
        e.to (0.0);
        // First step moves 30% of the -10 gap.
        const bool moving = e.step();
        CHECK (moving);
        CHECK (feq ((float) e.current, 7.0f));

        // Within eps of target -> snaps exactly and reports stopped.
        dcr::Eased s;
        s.current = 0.4;
        s.target = 0.0;
        CHECK (s.step (0.30, 0.5) == false);
        CHECK (s.current == 0.0);
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

    test_panic_engage_saves_and_mutes_all();
    test_panic_engage_empty_matrix_is_noop();
    test_panic_release_restores_prior_state();
    test_panic_release_when_inactive_is_noop();
    test_panic_forget_drops_saved_state_without_touching_matrix();
    test_panic_forget_when_inactive_is_noop();
    test_panic_reset_clears_state_and_reports();

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
    test_pdc_plan_device_domains();
    test_pdc_plan_domain_with_no_latency_stays_untouched();
    test_pdc_plan_mismatched_domains_fall_back_to_global();
    test_pdc_plan_domains_disabled_and_clamped();

    test_pdc_delay_static();
    test_pdc_delay_change_settles();
    test_pdc_delay_idle_then_activate();
    test_pdc_delay_ramp_is_finite();

    test_resonance_node_freq();
    test_resonance_node_interp();
    test_resonance_target_reduction();
    test_resonance_base_strength();

    test_deesser_gain_curve();

    test_ring_auto_size();

    test_stereometer_freq_to_norm();
    test_stereometer_high_lift_gain();
    test_stereometer_high_lift_knee();
    test_stereometer_db_to_norm_y();
    test_stereometer_per_octave_weight();
    test_recorder_naming();
    test_recorder_channel_count();

    test_spectral_sanitize_node_db();

    test_update_version_compare();

    test_appinput_reconcile_attach_when_running();
    test_appinput_reconcile_noop_when_offline_and_not_running();
    test_appinput_reconcile_steady_state_no_command();
    test_appinput_reconcile_detach_when_quit();
    test_appinput_reconcile_relaunch_detach_then_attach();
    test_appinput_reconcile_multiple_sources_mixed();

    test_matrixinput_hardware_reads_when_full();
    test_matrixinput_hardware_stalls_when_underfull();
    test_matrixinput_app_reads_when_attached_and_full();
    test_matrixinput_app_silence_when_attached_but_underfull();
    test_matrixinput_app_silence_when_detached();
    test_matrix_output_backpressure();

    test_device_rate_choice();
    test_format_restart_guard();

    test_eased_snap_and_rest();
    test_eased_converges_and_stops();
    test_eased_downward_and_epsilon_snap();

    std::printf ("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails == 0 ? 0 : 1;
}

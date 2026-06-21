#pragma once

#include <AudioToolbox/AudioToolbox.h>

namespace dcr
{

    // All tunable engine + UI parameters in one place.  No code path may use a
    // magic number that isn't represented here.  Edited by the user via the
    // Settings dialog; persisted to ~/Library/Application Support/dcorerouter.
    struct EngineSettings
    {
        // ===== Engine clock domain ===============================================
        //  All matrix processing and SRC targets run at this rate.  Each device's
        //  own rate is converted to/from it via per-channel SRC instances.
        double engineSampleRate = 48000.0; // Hz
        int engineBlockSize = 128; // samples per matrix processing block

        // ===== Ring buffers ======================================================
        //  Per-channel SPSC ring between IO callback and matrix thread.
        //  Raw size before pow2 rounding =
        //    max (multEng * engineBlockSize,
        //         multDev * deviceBufferSize * engineRate/deviceRate)
        //  Larger = more drift tolerance + latency; smaller = less latency + risk.
        int inputRingMultEng = 3;
        int inputRingMultDev = 4;
        int outputRingMultEng = 6;
        int outputRingMultDev = 8;

        //  Pre-fill output rings with this many engine blocks of silence at
        //  device open.  Gives the matrix thread headroom to ramp up and absorbs
        //  short-term drift.  Capped to outRing/4 internally.
        int outputPreFillBlocks = 8;

        // ===== Sample rate converter (AudioToolbox AudioConverter) ===============
        //  Quality:    0 = Min   0x20 = Low   0x40 = Medium   0x60 = High   0x7F = Max
        //  Complexity: 'line' = Linear   'norm' = Normal   'mast' = Mastering   'minp' = MinPhase
        //  Both ignored when device SR == engine SR (SRC is bypassed by identity).
        unsigned int srcQuality = kAudioConverterQuality_Max;
        unsigned int srcComplexity = kAudioConverterSampleRateConverterComplexity_Mastering;

        // ===== Matrix processor thread ===========================================
        int matrixThreadSleepMicros = 250; // sleep between polls when idle
        int matrixDrainPerWake = 16; // max blocks to process per wake

        // Per-route gain smoothing.  Any time a trim / crosspoint / mute changes,
        // the route's effective gain interpolates toward the new value with this
        // time constant (one-pole, block-level).  0 disables (instant jumps -
        // expect zipper noise).  Also applies during device reconfigure so the
        // output fades out before the engine restarts.
        int gainSmoothingMs = 25;

        // ===== Plugin delay compensation (PDC) ==================================
        //  When on, the engine delays every output to match the slowest plugin
        //  chain, so a latent plugin (e.g. a spectral insert) on one output stays
        //  phase-aligned with the rest of a multi-device group.  Optional and OFF
        //  by default -- a live router shouldn't silently add latency; the user
        //  opts in.  Toggled live (no engine restart) via setPdcEnabled(), so it is
        //  deliberately NOT part of audioPathEquals().
        bool pdcEnabled = false;

        // ===== UI ===============================================================
        int meterTimerHz = 30; // level meter repaint rate
        float meterDecayFactor = 0.92f; // per-frame multiplicative decay
        int statusTimerMs = 1000; // engine-status panel refresh interval (default 1 Hz to save CPU)

        // ===== Performance warnings ============================================
        //  Health classification of (stalled / (stalled + processed)).
        float stalledWarnRatio = 0.01f; // yellow above 1%
        float stalledCritRatio = 0.05f; // red above 5%
        //  CPU thresholds for status colour.
        float cpuWarnRatio = 0.70f; // yellow above 70%
        float cpuCritRatio = 0.90f; // red above 90%

        // ===== Theme colours ===================================================
        //  Stored as 0xRRGGBB; alpha is implicitly 0xFF.  Used by LookAndFeel and
        //  the status panel; semantic role colours (mute/solo/fx) stay hardwired
        //  in LookAndFeel since those follow pro-audio convention.
        unsigned int accentColorRGB = 0x00FFD2; // primary accent (knob arc, fader cap, focus rings)
        unsigned int warningColorRGB = 0xFFCC00; // warning state (CPU > warn, stalled > warn)
        unsigned int criticalColorRGB = 0xFF3B30; // critical state (CPU > crit, dropouts, mute)

        // ===== Subset comparison =================================================
        //  Audio-path fields that, if changed, require an engine restart.
        bool audioPathEquals (const EngineSettings& o) const noexcept
        {
            return engineSampleRate == o.engineSampleRate
                   && engineBlockSize == o.engineBlockSize
                   && inputRingMultEng == o.inputRingMultEng
                   && inputRingMultDev == o.inputRingMultDev
                   && outputRingMultEng == o.outputRingMultEng
                   && outputRingMultDev == o.outputRingMultDev
                   && outputPreFillBlocks == o.outputPreFillBlocks
                   && srcQuality == o.srcQuality
                   && srcComplexity == o.srcComplexity
                   && matrixThreadSleepMicros == o.matrixThreadSleepMicros
                   && matrixDrainPerWake == o.matrixDrainPerWake
                   && gainSmoothingMs == o.gainSmoothingMs;
        }
    };

} // namespace dcr

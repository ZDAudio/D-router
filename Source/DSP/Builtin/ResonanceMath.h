#pragma once

#include <algorithm>
#include <cmath>

// ===========================================================================
// Resonance Suppressor -- JUCE-free deterministic math, shared by the
// processor (audio thread) and the editor (drawing) as the single source of
// truth, and unit-tested headless (tests/test_main.cpp).  Mirrors the log-grid
// node mapping used by the Spectral Auto-EQ but lives here so neither side
// duplicates it.  The stateful per-bin attack/release smoothing is NOT here --
// it belongs to the processor.
// ===========================================================================
namespace dcr::resonance
{

    // Log-spaced node frequency: 20 Hz .. 20 kHz across [0, numNodes-1].
    // 20 * 1000^t, so the decade anchors fall on clean nodes (e.g. node 10 -> 200,
    // node 20 -> 2000 for a 31-node grid).  Index is clamped to the valid range.
    inline float nodeFreq (int i, int numNodes) noexcept
    {
        const int last = numNodes - 1;
        const int ci = std::clamp (i, 0, last);
        const float t = (float) ci / (float) last;
        return 20.0f * std::pow (1000.0f, t);
    }

    // Inverse of nodeFreq: for a frequency, the lower node index plus the [0,1]
    // fraction toward the next node, on the same log grid.  Frequencies outside
    // 20 Hz..20 kHz clamp onto the end segments.
    struct NodeInterp
    {
        int lo;
        float frac;
    };

    inline NodeInterp nodeInterp (float freqHz, int numNodes) noexcept
    {
        constexpr float fLo = 20.0f, fHi = 20000.0f;
        const float f = std::clamp (freqHz, fLo, fHi);
        const float denom = std::log (1000.0f);
        float ir = std::log (f / fLo) / denom * (float) (numNodes - 1);
        ir = std::clamp (ir, 0.0f, (float) (numNodes - 1));
        const int lo = std::clamp ((int) std::floor (ir), 0, numNodes - 2);
        const float fr = std::clamp (ir - (float) lo, 0.0f, 1.0f);
        return { lo, fr };
    }

    // Instantaneous target reduction (dB, <= 0) for one bin: how far the magnitude
    // pokes above the smooth baseline beyond the (per-frequency) threshold, scaled
    // by sharpness and capped at maxReductionDb.  Returns 0 when the bin doesn't
    // exceed baseline+threshold.  Stateless -- the caller time-smooths it per bin.
    inline float targetReductionDb (float magDb, float baseDb, float thresholdDb, float sharpness, float maxReductionDb) noexcept
    {
        const float excess = magDb - baseDb - thresholdDb;
        if (excess <= 0.0f)
            return 0.0f;
        return -std::min (maxReductionDb, excess * sharpness);
    }

    // Resolution choice (1/N octave) -> broad-baseline logSmooth strength.  Wider
    // band (1/3 oct) => broadest baseline => most aggressive; narrowest (1/24 oct)
    // => tightest baseline => only the sharpest resonances are caught.  Unknown N
    // falls back to the tightest setting.
    inline float baseStrengthForRes (int n) noexcept
    {
        switch (n)
        {
            case 3:
                return 0.6f;
            case 6:
                return 1.2f;
            case 12:
                return 2.4f;
            default:
                return 4.0f; // 1/24 oct (and any unexpected value)
        }
    }

} // namespace dcr::resonance

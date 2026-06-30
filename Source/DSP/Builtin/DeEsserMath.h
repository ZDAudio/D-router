#pragma once

#include <algorithm>
#include <cmath>

// ===========================================================================
// De-esser -- JUCE-free deterministic math, shared by the processor (audio
// thread) and unit-tested headless.  Only the static gain-curve mapping lives
// here; the stateful envelope follower (attack/release smoothing) and the IIR
// band filter belong to the processor and are verified by build + listening.
// ===========================================================================
namespace dcr::deess
{

    // Downward-compressor gain-reduction curve for the detected sibilance band.
    // Given the band level (dB), returns the target reduction in dB (<= 0):
    //   levelDb     measured band level
    //   thresholdDb knee centre
    //   ratio       >= 1 (1 = no compression)
    //   kneeDb      soft-knee width (>= 0; 0 = hard knee)
    //   rangeDb     maximum reduction (<= 0), e.g. -12
    // Standard soft-knee compressor static curve, then clamped to rangeDb.  The
    // quadratic knee is C1-continuous: value and slope match the hard-knee curve
    // at both knee corners (over = +/- kneeDb/2).
    inline float gainReductionDb (float levelDb, float thresholdDb, float ratio, float kneeDb, float rangeDb) noexcept
    {
        const float r = std::max (1.0f, ratio);
        const float slope = 1.0f - 1.0f / r; // 0..1
        const float over = levelDb - thresholdDb;
        const float halfKnee = kneeDb * 0.5f;

        float gr; // <= 0
        if (kneeDb > 0.0f && over > -halfKnee && over < halfKnee)
        {
            const float x = over + halfKnee; // 0..kneeDb
            gr = -slope * (x * x) / (2.0f * kneeDb);
        }
        else if (over >= halfKnee)
        {
            gr = -slope * over;
        }
        else
        {
            gr = 0.0f;
        }

        const float maxRed = std::min (0.0f, rangeDb);
        return std::max (gr, maxRed); // clamp reduction magnitude to the range
    }

    // Effective threshold.  In Auto mode the threshold tracks the program: it
    // sits relative to a slow running average of the band level, with the
    // threshold param acting as an offset (its -30 dB default == "at the
    // average").  In manual mode the param IS the threshold.
    inline float effectiveThresholdDb (float thresholdParam, float autoAvgDb, bool autoOn) noexcept
    {
        return autoOn ? (autoAvgDb + (thresholdParam + 30.0f)) : thresholdParam;
    }

    // dB <-> linear helpers so the processor and tests agree exactly.
    inline float dbToGain (float db) noexcept { return std::pow (10.0f, db * 0.05f); }

    inline float gainToDb (float g, float floorDb = -100.0f) noexcept
    {
        return g > 1.0e-9f ? std::max (floorDb, 20.0f * std::log10 (g)) : floorDb;
    }

} // namespace dcr::deess

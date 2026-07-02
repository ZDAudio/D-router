#pragma once

#include <algorithm>
#include <cmath>

namespace dcr::builtin
{

    // Max extra display-gain factor at full strength at/above Nyquist.
    inline constexpr float kHighLiftMax = 6.0f;

    // Proportional-bandwidth (per-octave) display weighting: +3 dB/oct on
    // amplitude, unity at `refHz`.  A raw FFT bin has constant bandwidth, so
    // equal-energy-per-octave material (pink noise) falls 3 dB/oct on a per-bin
    // display; hardware/software RTAs integrate proportional bands instead and
    // show pink as FLAT.  Multiplying each point's magnitude by this weight
    // reproduces the RTA convention.  White noise then reads +3 dB/oct rising --
    // that is correct, not a bug.
    inline float perOctaveWeight (float hz, float refHz) noexcept
    {
        if (hz <= 0.0f || refHz <= 0.0f)
            return 1.0f;
        return std::sqrt (hz / refHz);
    }

    // Log position of `hz` within [lowestHz, nyquistHz], clamped to [0, 1].
    // Mirrors the analyzer's log bin spacing so labels and points share one axis.
    inline float freqToNorm (float hz, float lowestHz, float nyquistHz) noexcept
    {
        if (nyquistHz <= lowestHz || hz <= lowestHz)
            return 0.0f;
        if (hz >= nyquistHz)
            return 1.0f;
        return std::log (hz / lowestHz) / std::log (nyquistHz / lowestHz);
    }

    // Max knee width (in normalized log-frequency units) at knee == 1.
    inline constexpr float kHighLiftKneeMax = 0.35f;

    // Display-intensity multiplier (>= 1) for the high-frequency tilt. ~1.0 at or
    // below the pivot (bass/low-mids untouched), rising toward Nyquist. This is a
    // *visualization* weighting, never an audio gain. strength in [0, 1].
    //   v    = log(hz/pivot) / log(nyquist/pivot)
    //   gain = 1 + strength * clamp(t, 0, 1) * kHighLiftMax
    // `knee` in [0, 1] softens the corner at the pivot: 0 = hard knee (flat at/
    // below the pivot, linear-in-log ramp above); higher rounds the corner via a
    // softplus, letting the lift ease in slightly below the pivot.
    inline float highLiftGain (float hz, float pivotHz, float nyquistHz, float strength, float knee = 0.0f) noexcept
    {
        if (strength <= 0.0f || pivotHz <= 0.0f || nyquistHz <= pivotHz)
            return 1.0f;

        const float v = std::log (hz / pivotHz) / std::log (nyquistHz / pivotHz);

        float tc;
        if (knee <= 0.0f)
        {
            if (hz <= pivotHz)
                return 1.0f; // hard knee: nothing below the pivot
            tc = std::min (1.0f, std::max (0.0f, v));
        }
        else
        {
            // Soft knee: softplus rounds the corner; `w` is the knee width.
            const float w = knee * kHighLiftKneeMax;
            const float z = v / w;
            const float sp = z > 20.0f ? z : std::log (1.0f + std::exp (z)); // overflow-guarded
            tc = std::min (1.0f, w * sp); // softplus is already >= 0
        }
        return 1.0f + strength * tc * kHighLiftMax;
    }

    // Map a dB value within [floorDb, ceilDb] to a vertical screen coordinate in
    // [-1, 1] (floor -> -1 bottom, ceil -> +1 top), clamped.  Used by the RTA
    // (side) view's level axis.  JUCE-free so it links into the test target.
    inline float dbToNormY (float db, float floorDb, float ceilDb) noexcept
    {
        if (ceilDb <= floorDb)
            return -1.0f;
        const float t = (db - floorDb) / (ceilDb - floorDb);
        const float tc = std::min (1.0f, std::max (0.0f, t));
        return 2.0f * tc - 1.0f;
    }

} // namespace dcr::builtin

#pragma once

#include <algorithm>
#include <cmath>

namespace dcr::builtin
{

    // Max extra display-gain factor at full strength at/above Nyquist.
    inline constexpr float kHighLiftMax = 6.0f;

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

    // Display-intensity multiplier (>= 1) for the high-frequency tilt. ~1.0 at or
    // below the pivot (bass/low-mids untouched), rising toward Nyquist. This is a
    // *visualization* weighting, never an audio gain. strength in [0, 1].
    //   t    = clamp( log(hz/pivot) / log(nyquist/pivot), 0, 1 )
    //   gain = 1 + strength * t * kHighLiftMax
    inline float highLiftGain (float hz, float pivotHz, float nyquistHz, float strength) noexcept
    {
        if (strength <= 0.0f || pivotHz <= 0.0f || nyquistHz <= pivotHz || hz <= pivotHz)
            return 1.0f;
        const float t = std::log (hz / pivotHz) / std::log (nyquistHz / pivotHz);
        const float tc = std::min (1.0f, std::max (0.0f, t));
        return 1.0f + strength * tc * kHighLiftMax;
    }

} // namespace dcr::builtin

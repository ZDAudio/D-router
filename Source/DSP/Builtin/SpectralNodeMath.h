#pragma once

#include <cmath>

namespace dcr::spectral
{

    // The Spectral Auto-EQ and Resonance Suppressor both expose a draggable
    // per-frequency node curve: a dB offset per node, editor-clamped to
    // +/- kNodeLimitDb.  setStateInformation restores those nodes from a preset,
    // which is an UNTRUSTED surface -- a hand-edited or corrupt blob can carry NaN,
    // Inf, or a wildly out-of-range value.  An unsanitised value latches into the
    // per-bin smoother and poisons the FFT output with NaN forever (juce::jlimit
    // does NOT reject NaN: both of its comparisons are false, so NaN passes
    // straight through).  Every restored node goes through here.  JUCE-free so it
    // is unit-tested headless, and the single source of truth for the node range.
    inline constexpr float kNodeLimitDb = 18.0f;

    inline float sanitizeNodeDb (double v) noexcept
    {
        if (!std::isfinite (v))
            return 0.0f; // NaN / +-Inf -> neutral
        if (v < -(double) kNodeLimitDb)
            return -kNodeLimitDb;
        if (v > (double) kNodeLimitDb)
            return kNodeLimitDb;
        return (float) v;
    }

} // namespace dcr::spectral

#pragma once

#include <algorithm>
#include <cmath>

// Pure, JUCE-free gain math shared by the input/output group managers.  Kept
// dependency-light on purpose so it can be unit-tested in the headless
// dcorerouter_tests target (which links no JUCE).  These were previously
// duplicated as anonymous-namespace helpers inside each manager; centralised
// here so VCA bake math and Router overlay gain can't diverge between the two.
namespace dcr::groupgain {

// dB -> linear.  Anything at or below the -60 dB floor collapses to true
// silence (gain 0) so a fader dragged to the bottom mutes rather than leaving a
// -60 dB whisper.
inline float dbToLin (float db) noexcept
{
    if (db <= -60.0f) return 0.0f;
    return std::pow (10.0f, db * 0.05f);
}

// linear -> dB, with a -60 dB floor mirroring dbToLin so a round-trip through
// silence is stable instead of diving to -inf.
inline float linToDb (float lin) noexcept
{
    if (lin <= 1.0e-6f) return -60.0f;
    return 20.0f * std::log10 (lin);
}

// Trim faders live in [-60, +12] dB (the matrix trim range).
inline float clampTrimDb (float db) noexcept
{
    return std::clamp (db, -60.0f, 12.0f);
}

// Router-mode per-channel overlay gain: the group fader is a separate stage
// multiplied on top of the channel's own trim.  Group mute zeroes it.
inline float routerChannelGain (bool muted, float faderDb) noexcept
{
    return muted ? 0.0f : dbToLin (faderDb);
}

// VCA bake: when a group leaves Router mode the overlay is folded into each
// member's own trim so the audible level is preserved.  Additive in dB, clamped
// to the trim range.
inline float bakeVcaTrimDb (float memberDb, float faderDb) noexcept
{
    return clampTrimDb (memberDb + faderDb);
}

} // namespace dcr::groupgain

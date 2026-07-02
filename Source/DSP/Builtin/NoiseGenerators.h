#pragma once

#include <cmath>
#include <cstdint>

// JUCE-free noise-shaping DSP for the Tone Generator, split out so the JUCE
// test target can verify the spectra (octave-band flatness, crest factors)
// against the same code the plugin runs.  Everything here is allocation-free
// after construction and safe on the audio thread.
namespace dcr::noise
{

    // Paul Kellett's "refined" pink-noise filter: white in -> pink out, accurate
    // to well under +/-1 dB per octave across the audible band (coefficients are
    // tuned near 44.1/48 kHz; the error stays small at higher rates).  This
    // replaces the old single one-pole "pink approx", whose spectrum was white
    // below ~150 Hz and fell at -6 dB/oct above it -- visibly wrong on any RTA.
    class PinkFilter
    {
    public:
        void reset() noexcept { b0 = b1 = b2 = b3 = b4 = b5 = b6 = 0.0f; }

        float process (float white) noexcept
        {
            b0 = 0.99886f * b0 + white * 0.0555179f;
            b1 = 0.99332f * b1 + white * 0.0750759f;
            b2 = 0.96900f * b2 + white * 0.1538520f;
            b3 = 0.86650f * b3 + white * 0.3104856f;
            b4 = 0.55000f * b4 + white * 0.5329522f;
            b5 = -0.7616f * b5 - white * 0.0168980f;
            const float pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + white * 0.5362f;
            b6 = white * 0.115926f;
            return pink * 0.11f; // keep peaks inside +/-1 for full-scale white in
        }

    private:
        float b0 = 0.0f, b1 = 0.0f, b2 = 0.0f, b3 = 0.0f, b4 = 0.0f, b5 = 0.0f, b6 = 0.0f;
    };

    // Tiny deterministic RNG for envelope decisions (and tests).  xorshift32 --
    // no libc state, trivially RT-safe.
    class Rng32
    {
    public:
        explicit Rng32 (std::uint32_t seed = 0x9E3779B9u) noexcept
            : s (seed != 0 ? seed : 1u) {}

        // Uniform in [-1, 1) -- suitable as full-scale white noise.
        float bipolar() noexcept { return unit() * 2.0f - 1.0f; }

        // Uniform in [0, 1).
        float unit() noexcept
        {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            return (float) (s >> 8) * (1.0f / 16777216.0f);
        }

    private:
        std::uint32_t s;
    };

    // Music-weighted noise ("MNoise" mode), an *approximation* of the published
    // behaviour of music-like loudspeaker test signals (AES75-style): per-octave
    // RMS tracks pink noise below ~500 Hz, falls ~3 dB/oct above it (about
    // -12 dB by 16 kHz), while the high band is emitted in random bursts so its
    // peaks stay near pink's -- i.e. the crest factor grows with frequency
    // (~12 dB low, ~18-20 dB in the top octaves).  It is NOT the licensed
    // measurement signal; for standards-compliant max-SPL tests play the
    // official file instead.
    //
    // Structure: pink -> 1-pole split at 500 Hz -> HF branch gets two
    // pole/zero shelves (one octave apart each => ~-3 dB/oct trend) and two
    // independent two-level burst envelopes (fast 20-60 ms + slow 200-600 ms,
    // each unity mean-square so the long-term spectrum is preserved).
    class MusicNoise
    {
    public:
        void prepare (double sampleRate, std::uint32_t seed = 0x9E3779B9u) noexcept
        {
            sr = sampleRate > 1000.0 ? sampleRate : 48000.0;
            pink.reset();
            env = Rng32 (seed);
            lp = 0.0f;
            xoCoeff = onePoleCoeff (500.0);

            sh1.set (700.0, 1400.0, sr); // each shelf: -6 dB spread over ~1.5 oct
            sh2.set (5600.0, 11200.0, sr);

            fast.reset();
            slow.reset();
            gainSmooth = 1.0f;
            // ~3 ms gain smoothing keeps envelope steps from clicking.
            smoothCoeff = onePoleCoeff (55.0);
        }

        float process (float white) noexcept
        {
            const float p = pink.process (white);

            // 500 Hz complementary split (lp + hf == p exactly).
            lp += (1.0f - xoCoeff) * (p - lp);
            const float hf = p - lp;

            // Burst envelopes: advance per-sample counters, retarget on expiry.
            const float target = fast.tick (env, sr, 0.020, 0.060, 1.7f, 0.25f)
                                 * slow.tick (env, sr, 0.200, 0.600, 1.4f, 0.35f);
            gainSmooth += (1.0f - smoothCoeff) * (target - gainSmooth);

            return lp + sh2.process (sh1.process (hf)) * gainSmooth;
        }

    private:
        static double onePoleCoeffFor (double hz, double sampleRate) noexcept
        {
            return std::exp (-2.0 * 3.14159265358979323846 * hz / sampleRate);
        }
        float onePoleCoeff (double hz) const noexcept
        {
            return (float) onePoleCoeffFor (hz, sr);
        }

        // First-order pole/zero shelf, unity at DC, cutting above `poleHz`
        // (zero one octave up flattens it again => a gentle -3 dB/oct segment).
        struct Shelf
        {
            void set (double poleHz, double zeroHz, double sampleRate) noexcept
            {
                ep = (float) onePoleCoeffFor (poleHz, sampleRate);
                ez = (float) onePoleCoeffFor (zeroHz, sampleRate);
                g = (1.0f - ep) / (1.0f - ez); // unity DC gain
                x1 = y1 = 0.0f;
            }
            float process (float x) noexcept
            {
                const float y = g * (x - ez * x1) + ep * y1;
                x1 = x;
                y1 = y;
                return y;
            }
            float ep = 0.0f, ez = 0.0f, g = 1.0f, x1 = 0.0f, y1 = 0.0f;
        };

        // Two-level random burst gate.  E[g^2] == 1 by construction, so it shapes
        // peaks (crest) without moving the long-term RMS spectrum.
        struct Burst
        {
            void reset() noexcept
            {
                samplesLeft = 0;
                gain = 1.0f;
            }
            float tick (Rng32& rng, double sr, double minSec, double maxSec, float hiGain, float pHi) noexcept
            {
                if (--samplesLeft <= 0)
                {
                    samplesLeft = (int) (sr * (minSec + (maxSec - minSec) * rng.unit()));
                    const float hi2 = hiGain * hiGain;
                    const float lo = std::sqrt ((1.0f - pHi * hi2) / (1.0f - pHi));
                    gain = rng.unit() < pHi ? hiGain : lo;
                }
                return gain;
            }
            int samplesLeft = 0;
            float gain = 1.0f;
        };

        double sr = 48000.0;
        PinkFilter pink;
        Rng32 env;
        float lp = 0.0f, xoCoeff = 0.0f;
        Shelf sh1, sh2;
        Burst fast, slow;
        float gainSmooth = 1.0f, smoothCoeff = 0.0f;
    };

} // namespace dcr::noise

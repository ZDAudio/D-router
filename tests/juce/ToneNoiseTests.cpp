// Spectrum regression net for the Tone Generator noise sources
// (Source/DSP/Builtin/NoiseGenerators.h).  Exists because the original "pink"
// was a one-pole approximation whose RTA read white below ~150 Hz and fell
// -6 dB/oct above it -- caught on a real analyzer, not by any test.  These
// tests measure octave-band energy the way an RTA does, so a regression in
// any noise source's spectrum fails headlessly.
#include "DSP/Builtin/NoiseGenerators.h"

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <functional>
#include <vector>

namespace
{
    constexpr double kSr = 48000.0;
    constexpr int kNumSamples = 1 << 19; // ~10.9 s -- enough averaging for +/-dB claims

    std::vector<float> generate (const std::function<float (float)>& gen, std::uint32_t seed)
    {
        dcr::noise::Rng32 rng (seed);
        std::vector<float> v ((size_t) kNumSamples);
        for (auto& s : v)
            s = gen (rng.bipolar());
        return v;
    }

    // Welch-averaged power spectrum (Hann, 50% hop), then octave-band energy
    // sums -- the proportional-bandwidth view an RTA shows.  Returns dB per band.
    std::vector<double> octaveBandDb (const std::vector<float>& x, const std::vector<double>& centres)
    {
        constexpr int kFft = 8192;
        juce::dsp::FFT fft (13); // 2^13 == 8192

        std::vector<float> window ((size_t) kFft);
        for (int i = 0; i < kFft; ++i)
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0 * juce::MathConstants<double>::pi * i / (kFft - 1));

        std::vector<double> power ((size_t) (kFft / 2 + 1), 0.0);
        std::vector<float> scratch ((size_t) (kFft * 2), 0.0f);

        int segments = 0;
        for (int start = 0; start + kFft <= (int) x.size(); start += kFft / 2, ++segments)
        {
            std::fill (scratch.begin(), scratch.end(), 0.0f);
            for (int i = 0; i < kFft; ++i)
                scratch[(size_t) i] = x[(size_t) (start + i)] * window[(size_t) i];
            fft.performRealOnlyForwardTransform (scratch.data());
            for (int k = 0; k <= kFft / 2; ++k)
            {
                const double re = scratch[(size_t) (2 * k)];
                const double im = scratch[(size_t) (2 * k + 1)];
                power[(size_t) k] += re * re + im * im;
            }
        }

        std::vector<double> bands;
        for (const double fc : centres)
        {
            const double lo = fc / juce::MathConstants<double>::sqrt2;
            const double hi = fc * juce::MathConstants<double>::sqrt2;
            double sum = 0.0;
            for (int k = 1; k <= kFft / 2; ++k)
            {
                const double f = (double) k * kSr / kFft;
                if (f >= lo && f < hi)
                    sum += power[(size_t) k];
            }
            bands.push_back (10.0 * std::log10 (juce::jmax (1e-30, sum / juce::jmax (1, segments))));
        }
        return bands;
    }

    double rmsOf (const std::vector<float>& x)
    {
        double acc = 0.0;
        for (float s : x)
            acc += (double) s * s;
        return std::sqrt (acc / (double) x.size());
    }

    // Crest factor (dB) of `x` after a simple 2x one-pole high- or low-pass at
    // `fc` -- enough band isolation to compare the same band across two signals.
    double bandCrestDb (const std::vector<float>& x, double fc, bool highpass)
    {
        const float a = (float) std::exp (-2.0 * juce::MathConstants<double>::pi * fc / kSr);
        float lp1 = 0.0f, lp2 = 0.0f;
        std::vector<float> y;
        y.reserve (x.size());
        for (float s : x)
        {
            lp1 += (1.0f - a) * (s - lp1);
            lp2 += (1.0f - a) * (lp1 - lp2);
            y.push_back (highpass ? s - lp2 : lp2);
        }
        // skip the filter warm-up
        y.erase (y.begin(), y.begin() + 4800);
        double peak = 0.0, acc = 0.0;
        for (float s : y)
        {
            peak = juce::jmax (peak, (double) std::abs (s));
            acc += (double) s * s;
        }
        const double rms = std::sqrt (acc / (double) y.size());
        return 20.0 * std::log10 (juce::jmax (1e-12, peak) / juce::jmax (1e-12, rms));
    }

    juce::String rowToString (const std::vector<double>& v)
    {
        juce::String s;
        for (double d : v)
            s << juce::String (d, 1) << "  ";
        return s;
    }
} // namespace

struct ToneNoiseTests : juce::UnitTest
{
    ToneNoiseTests() : juce::UnitTest ("Tone Generator noise spectra") {}

    void runTest() override
    {
        const std::vector<double> centres = { 31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0 };

        // ---- white: flat spectral density == +3 dB/oct on octave bands -------
        beginTest ("white noise rises ~3 dB per octave band (RTA convention)");
        std::vector<float> white;
        {
            white = generate ([] (float w) { return w; }, 0xA0A0A001u);
            const auto b = octaveBandDb (white, centres);
            logMessage ("  white bands (dB): " + rowToString (b));
            // Consecutive octave steps 500 Hz..16 kHz land near +3 dB each.
            for (size_t i = 4; i + 1 < b.size(); ++i)
            {
                expectGreaterThan (b[i + 1] - b[i], 2.2);
                expectLessThan (b[i + 1] - b[i], 3.8);
            }
        }

        // ---- pink: equal energy per octave == flat octave bands --------------
        beginTest ("pink noise is flat per octave band within +/-1.5 dB");
        std::vector<float> pink;
        {
            dcr::noise::PinkFilter f;
            f.reset();
            pink = generate ([&f] (float w) { return f.process (w); }, 0xA0A0A002u);
            const auto b = octaveBandDb (pink, centres);
            logMessage ("  pink bands (dB): " + rowToString (b));
            // Judge 63 Hz..16 kHz (the 31.5 band gets the fewest FFT averages).
            double mean = 0.0;
            for (size_t i = 1; i < b.size(); ++i)
                mean += b[i];
            mean /= (double) (b.size() - 1);
            for (size_t i = 1; i < b.size(); ++i)
            {
                expectLessThan (std::abs (b[i] - mean), 1.5);
            }
        }

        // ---- MNoise: pink plateau, HF RMS shelved down, HF crest raised ------
        beginTest ("MNoise: pink low end, ~-3 dB/oct RMS above 500 Hz");
        std::vector<float> mnoise;
        {
            dcr::noise::MusicNoise m;
            m.prepare (kSr);
            mnoise = generate ([&m] (float w) { return m.process (w); }, 0xA0A0A003u);
            const auto b = octaveBandDb (mnoise, centres);
            logMessage ("  mnoise bands (dB): " + rowToString (b));

            // Plateau: 63..500 Hz flat within +/-2 dB of its mean.
            double plateau = 0.0;
            for (size_t i = 1; i <= 4; ++i)
                plateau += b[i];
            plateau /= 4.0;
            for (size_t i = 1; i <= 4; ++i)
                expectLessThan (std::abs (b[i] - plateau), 2.0);

            // Roll-off vs the 500 Hz band: monotonic and in the published range
            // (~-8 dB by 8 kHz, ~-12 dB by 16 kHz, generous tolerance).
            const double d8k = b[8] - b[4];
            const double d16k = b[9] - b[4];
            logMessage ("  mnoise roll-off: 8k " + juce::String (d8k, 1) + " dB, 16k " + juce::String (d16k, 1) + " dB");
            expectLessThan (d8k, -5.0);
            expectGreaterThan (d8k, -14.0);
            expectLessThan (d16k, -7.0);
            expectGreaterThan (d16k, -18.0);
            expectLessThan (d16k, d8k + 0.5); // still falling past 8 kHz

            // Crest: HF band crest well above pink's (bursts), LF crest similar.
            const double hfM = bandCrestDb (mnoise, 2000.0, true);
            const double hfP = bandCrestDb (pink, 2000.0, true);
            const double lfM = bandCrestDb (mnoise, 300.0, false);
            const double lfP = bandCrestDb (pink, 300.0, false);
            logMessage ("  crest HF mnoise/pink: " + juce::String (hfM, 1) + " / " + juce::String (hfP, 1)
                        + " dB;  LF: " + juce::String (lfM, 1) + " / " + juce::String (lfP, 1) + " dB");
            expectGreaterThan (hfM, hfP + 3.0);
            expectLessThan (std::abs (lfM - lfP), 3.0);

            // Broadband RMS stays in the same ballpark as pink at the same Level.
            const double relDb = 20.0 * std::log10 (rmsOf (mnoise) / rmsOf (pink));
            logMessage ("  mnoise RMS rel pink: " + juce::String (relDb, 1) + " dB");
            expectGreaterThan (relDb, -6.0);
            expectLessThan (relDb, 1.5);
        }

        // ---- determinism: same seed -> same signal (tests stay meaningful) ---
        beginTest ("generators are deterministic for a fixed seed");
        {
            dcr::noise::MusicNoise m;
            m.prepare (kSr);
            const auto again = generate ([&m] (float w) { return m.process (w); }, 0xA0A0A003u);
            expectEquals (again[1000], mnoise[1000]);
            expectEquals (again[100000], mnoise[100000]);
            expectEquals (again[400000], mnoise[400000]);
        }
    }
};

static ToneNoiseTests toneNoiseTests;

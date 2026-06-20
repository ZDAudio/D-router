#pragma once

#include "DSP/Builtin/SpectralProcessor.h"

#include <array>
#include <atomic>
#include <vector>

namespace dcr::builtin
{

// ===========================================================================
// Spectral Auto-EQ (Gullfoss-style) -- continuously balances the tone by
// flattening each band toward the broadband spectral trend.  Peaks above the
// trend are tamed; dips below it are recovered.  A tilt control brightens /
// darkens, and the whole correction is time-smoothed per bin so it rides the
// program without pumping.  Built on the shared STFT engine.
// ===========================================================================
class SpectralAutoEqProcessor : public SpectralProcessor
{
public:
    static constexpr int kDisp = 128;   // downsampled display resolution

    SpectralAutoEqProcessor()
        : SpectralProcessor (ids::autoeq, "Spectral Auto-EQ", createLayout(), /*fftOrder*/ 10, /*overlap*/ 4) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "tame", 1 }, "Tame",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.45f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "recover", 1 }, "Recover",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.30f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "tilt", 1 }, "Brighten",
            juce::NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "range", 1 }, "Max change",
            juce::NormalisableRange<float> (1.0f, 18.0f, 0.1f), 9.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "amount", 1 }, "Amount",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.6f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "speed", 1 }, "Speed",
            juce::NormalisableRange<float> (5.0f, 500.0f, 1.0f, 0.4f), 80.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        return l;
    }

    juce::AudioProcessorEditor* createEditor() override;   // SpectralCurveEditor.h

    // Display snapshots (ch0), racy-but-harmless for the visualiser.
    int   getDisplaySize() const noexcept { return kDisp; }
    float getDisplayMagDb (int i) const noexcept { return (i >= 0 && i < kDisp) ? dispMag[(size_t) i]  : -100.0f; }
    float getDisplayGainDb (int i) const noexcept { return (i >= 0 && i < kDisp) ? dispGain[(size_t) i] : 0.0f; }

protected:
    void prepareSpectral (double sr, int numBins, int numChannels) override
    {
        gainDb.assign ((size_t) numChannels, std::vector<float> ((size_t) numBins, 0.0f));
        envWork.assign ((size_t) numBins, 0.0f);
        refWork.assign ((size_t) numBins, 0.0f);
        magDbWork.assign ((size_t) numBins, -100.0f);
        const double frameSec = (double) (1 << 10) / 4.0 / sr;   // hop/sr
        smoothBaseSec = frameSec;
        for (auto& v : dispMag)  v = -100.0f;
        for (auto& v : dispGain) v = 0.0f;
    }

    // Forward+backward one-pole across bins with a per-bin coefficient that
    // widens the window with frequency (constant-Q-ish log smoothing).
    void logSmooth (const float* src, float* dst, int n, float strength)
    {
        float run = src[0];
        for (int k = 0; k < n; ++k)
        {
            const float a = juce::jlimit (0.02f, 1.0f, strength / (float) (k + 1));
            run += a * (src[k] - run);
            dst[k] = run;
        }
        run = dst[n - 1];
        for (int k = n - 1; k >= 0; --k)
        {
            const float a = juce::jlimit (0.02f, 1.0f, strength / (float) (k + 1));
            run += a * (dst[k] - run);
            dst[k] = run;
        }
    }

    void computeBinGains (const float* mags, float* gains, int numBins, double sr, int channel) override
    {
        const float tame    = param ("tame");
        const float recover = param ("recover");
        const float tilt    = param ("tilt");
        const float rangeDb = param ("range");
        const float amount  = param ("amount");
        const float speedMs = juce::jmax (5.0f, param ("speed"));
        const float tcoeff  = 1.0f - std::exp (-(float) smoothBaseSec / (speedMs * 0.001f));

        for (int k = 0; k < numBins; ++k)
            magDbWork[(size_t) k] = juce::Decibels::gainToDecibels (mags[k], -120.0f);

        logSmooth (magDbWork.data(), envWork.data(), numBins, 6.0f);   // ~1/3 oct
        logSmooth (magDbWork.data(), refWork.data(), numBins, 1.5f);   // broad trend

        const float nyq = (float) (sr * 0.5);
        auto& gch = gainDb[(size_t) channel];

        for (int k = 0; k < numBins; ++k)
        {
            const float dev = envWork[(size_t) k] - refWork[(size_t) k];   // +peak / -dip
            float corr = (dev > 0.0f) ? -dev * tame : -dev * recover;       // flatten toward trend

            // Tilt: +/- dB across the spectrum (log-frequency).
            const float f = (float) k / (float) (numBins - 1) * nyq;
            const float oct = std::log2 (juce::jmax (20.0f, f) / 1000.0f);  // octaves from 1 kHz
            corr += tilt * 1.5f * oct;

            corr = juce::jlimit (-rangeDb, rangeDb, corr * amount);

            float& g = gch[(size_t) k];
            g += (corr - g) * tcoeff;                       // time-smooth per bin
            gains[k] = juce::Decibels::decibelsToGain (g);
        }

        if (channel == 0) updateDisplay (numBins);
    }

private:
    void updateDisplay (int numBins)
    {
        for (int i = 0; i < kDisp; ++i)
        {
            const int k = juce::jlimit (0, numBins - 1, (int) ((float) i / (kDisp - 1) * (numBins - 1)));
            dispMag[(size_t) i]  = magDbWork[(size_t) k];
            dispGain[(size_t) i] = gainDb[0][(size_t) k];
        }
    }

    std::vector<std::vector<float>> gainDb;     // [channel][bin] smoothed correction dB
    std::vector<float> envWork, refWork, magDbWork;
    double smoothBaseSec = 0.005;
    std::array<float, kDisp> dispMag {};
    std::array<float, kDisp> dispGain {};
};

} // namespace dcr::builtin

#pragma once

#include "DSP/Builtin/SpectralProcessor.h"

#include <array>
#include <vector>

namespace dcr::builtin
{

// ===========================================================================
// Resonance Suppressor (soothe-style) -- finds bins that stick out above the
// smooth spectral baseline (resonances / harshness) and dynamically ducks
// them, with attack/release per bin so it only acts while the resonance rings.
// Built on the shared STFT engine.
// ===========================================================================
class ResonanceSuppressorProcessor : public SpectralProcessor
{
public:
    static constexpr int kDisp = 128;

    ResonanceSuppressorProcessor()
        : SpectralProcessor (ids::resonance, "Resonance Suppressor", createLayout(), /*fftOrder*/ 11, /*overlap*/ 4) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "depth", 1 }, "Depth",
            juce::NormalisableRange<float> (0.0f, 12.0f, 0.1f), 4.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "sharpness", 1 }, "Sharpness",
            juce::NormalisableRange<float> (0.1f, 2.0f, 0.01f), 0.8f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "reduction", 1 }, "Max reduction",
            juce::NormalisableRange<float> (1.0f, 24.0f, 0.1f), 12.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "attack", 1 }, "Attack",
            juce::NormalisableRange<float> (1.0f, 100.0f, 0.1f, 0.5f), 8.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "release", 1 }, "Release",
            juce::NormalisableRange<float> (10.0f, 500.0f, 1.0f, 0.4f), 120.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "minfreq", 1 }, "Min freq",
            juce::NormalisableRange<float> (20.0f, 2000.0f, 1.0f, 0.3f), 150.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));
        return l;
    }

    juce::AudioProcessorEditor* createEditor() override;   // SpectralCurveEditor.h

    int   getDisplaySize() const noexcept { return kDisp; }
    float getDisplayMagDb (int i) const noexcept { return (i >= 0 && i < kDisp) ? dispMag[(size_t) i]  : -100.0f; }
    float getDisplayGainDb (int i) const noexcept { return (i >= 0 && i < kDisp) ? dispGain[(size_t) i] : 0.0f; }

protected:
    void prepareSpectral (double sr, int numBins, int numChannels) override
    {
        redDb.assign ((size_t) numChannels, std::vector<float> ((size_t) numBins, 0.0f));
        baseWork.assign ((size_t) numBins, 0.0f);
        magDbWork.assign ((size_t) numBins, -100.0f);
        frameSec = (double) getFftSize() / 4.0 / sr;     // hop/sr
        for (auto& v : dispMag)  v = -100.0f;
        for (auto& v : dispGain) v = 0.0f;
    }

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
        const float depth     = param ("depth");
        const float sharp     = param ("sharpness");
        const float maxRed    = param ("reduction");
        const float attMs     = juce::jmax (1.0f, param ("attack"));
        const float relMs     = juce::jmax (1.0f, param ("release"));
        const float minFreq   = param ("minfreq");
        const float attCoeff  = 1.0f - std::exp (-(float) frameSec / (attMs * 0.001f));
        const float relCoeff  = 1.0f - std::exp (-(float) frameSec / (relMs * 0.001f));
        const float nyq       = (float) (sr * 0.5);
        const int   minBin    = juce::jlimit (1, numBins - 1, (int) (minFreq / nyq * (numBins - 1)));

        for (int k = 0; k < numBins; ++k)
            magDbWork[(size_t) k] = juce::Decibels::gainToDecibels (mags[k], -120.0f);

        // Broad baseline = the "smooth" spectrum a resonance pokes through.
        logSmooth (magDbWork.data(), baseWork.data(), numBins, 1.2f);

        auto& rch = redDb[(size_t) channel];
        for (int k = 0; k < numBins; ++k)
        {
            float targetRed = 0.0f;   // <= 0
            if (k >= minBin)
            {
                const float excess = magDbWork[(size_t) k] - baseWork[(size_t) k] - depth;
                if (excess > 0.0f)
                    targetRed = -juce::jmin (maxRed, excess * sharp);
            }

            // Attack when ducking harder (target more negative), release when easing.
            float& r = rch[(size_t) k];
            const float c = (targetRed < r) ? attCoeff : relCoeff;
            r += (targetRed - r) * c;
            gains[k] = juce::Decibels::decibelsToGain (r);
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
            dispGain[(size_t) i] = redDb[0][(size_t) k];
        }
    }

    std::vector<std::vector<float>> redDb;       // [channel][bin] smoothed reduction dB (<= 0)
    std::vector<float> baseWork, magDbWork;
    double frameSec = 0.01;
    std::array<float, kDisp> dispMag {};
    std::array<float, kDisp> dispGain {};
};

} // namespace dcr::builtin

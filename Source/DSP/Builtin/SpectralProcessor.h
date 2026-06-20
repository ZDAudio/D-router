#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

namespace dcr::builtin
{

// ===========================================================================
// Shared STFT (weighted overlap-add) engine for spectral plugins.  Per channel
// it runs a Hann-windowed analysis FFT, hands the magnitude spectrum to a
// subclass that fills a per-bin linear gain, then reconstructs via inverse FFT
// + synthesis window + overlap-add.  Latency = one FFT frame.
//
// Subclasses implement computeBinGains() (and may override prepareSpectral()
// to size per-channel time-smoothing state).
// ===========================================================================
class SpectralProcessor : public BuiltinProcessor
{
public:
    SpectralProcessor (juce::String id, juce::String name, APVTS::ParameterLayout layout,
                       int fftOrder = 10, int overlap = 4)
        : BuiltinProcessor (std::move (id), std::move (name), std::move (layout)),
          fftOrderVal (fftOrder), overlapFactor (overlap),
          fft (fftOrder)
    {
        fftSize = 1 << fftOrder;
        hopSize = fftSize / overlap;
        numBins = fftSize / 2 + 1;
    }

protected:
    // ---- subclass hooks ----------------------------------------------------
    virtual void prepareSpectral (double /*sr*/, int /*numBins*/, int /*numChannels*/) {}
    // mags[0..numBins) -> fill gains[0..numBins) (linear, 1 = unchanged).
    virtual void computeBinGains (const float* mags, float* gains,
                                  int numBins, double sr, int channel) = 0;

    int getFftSize() const noexcept { return fftSize; }
    int getNumBins() const noexcept { return numBins; }
    int getHopSize() const noexcept { return hopSize; }
    int getOverlapFactor() const noexcept { return overlapFactor; }
    const char* getWindowName() const noexcept { return "Hann"; }   // fixed Hann@WOLA

    // Zero-phase log-frequency smoother shared by the spectral subclasses:
    // a forward+backward one-pole across bins whose window widens with
    // frequency (constant-Q-ish).  `strength` ~ how tight (larger = narrower).
    static void logSmooth (const float* src, float* dst, int n, float strength)
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

    // ---- BuiltinProcessor plumbing ----------------------------------------
    void prepareDsp (double sr, int /*blockSize*/, int numChannels) override
    {
        dspSampleRate = sr;
        const int nch = juce::jmax (1, numChannels);

        window.resize ((size_t) fftSize);
        double sumSq = 0.0;
        for (int i = 0; i < fftSize; ++i)
        {
            window[(size_t) i] = 0.5f - 0.5f * std::cos (2.0 * juce::MathConstants<double>::pi * i / (fftSize - 1));
            sumSq += (double) window[(size_t) i] * window[(size_t) i];
        }
        // WOLA normalisation so analysis*synthesis windows overlap-add to unity.
        normScale = (float) ((double) hopSize / juce::jmax (1.0e-9, sumSq));

        fftData.assign ((size_t) (fftSize * 2), 0.0f);
        mags.assign ((size_t) numBins, 0.0f);
        gains.assign ((size_t) numBins, 1.0f);

        chans.assign ((size_t) nch, {});
        for (auto& c : chans)
        {
            c.inputFifo.assign ((size_t) fftSize, 0.0f);
            c.outputFifo.assign ((size_t) fftSize, 0.0f);
            c.fifoIndex = 0;
            c.hopCounter = 0;
        }

        setLatencySamples (fftSize);
        prepareSpectral (sr, numBins, nch);
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = juce::jmin (buffer.getNumChannels(), (int) chans.size());

        for (int ch = 0; ch < nch; ++ch)
        {
            auto& cs = chans[(size_t) ch];
            float* x = buffer.getWritePointer (ch);

            for (int i = 0; i < ns; ++i)
            {
                cs.inputFifo[(size_t) cs.fifoIndex] = x[i];
                x[i] = cs.outputFifo[(size_t) cs.fifoIndex];
                cs.outputFifo[(size_t) cs.fifoIndex] = 0.0f;

                if (++cs.fifoIndex >= fftSize) cs.fifoIndex = 0;
                if (++cs.hopCounter >= hopSize) { cs.hopCounter = 0; processFrame (cs, ch); }
            }
        }
    }

private:
    struct ChannelState
    {
        std::vector<float> inputFifo, outputFifo;
        int fifoIndex = 0, hopCounter = 0;
    };

    void processFrame (ChannelState& cs, int ch)
    {
        const int idx = cs.fifoIndex;

        std::fill (fftData.begin(), fftData.end(), 0.0f);
        for (int i = 0; i < fftSize; ++i)
            fftData[(size_t) i] = cs.inputFifo[(size_t) ((idx + i) % fftSize)] * window[(size_t) i];

        fft.performRealOnlyForwardTransform (fftData.data());

        for (int k = 0; k < numBins; ++k)
        {
            const float re = fftData[(size_t) (2 * k)];
            const float im = fftData[(size_t) (2 * k + 1)];
            mags[(size_t) k] = std::sqrt (re * re + im * im);
        }

        computeBinGains (mags.data(), gains.data(), numBins, dspSampleRate, ch);

        for (int k = 0; k < numBins; ++k)
        {
            fftData[(size_t) (2 * k)]     *= gains[(size_t) k];
            fftData[(size_t) (2 * k + 1)] *= gains[(size_t) k];
        }

        fft.performRealOnlyInverseTransform (fftData.data());

        for (int i = 0; i < fftSize; ++i)
            cs.outputFifo[(size_t) ((idx + i) % fftSize)] += fftData[(size_t) i] * window[(size_t) i] * normScale;
    }

    int fftOrderVal, overlapFactor;
    int fftSize = 1024, hopSize = 256, numBins = 513;
    juce::dsp::FFT fft;

    std::vector<float> window;
    std::vector<float> fftData;   // 2*fftSize scratch
    std::vector<float> mags, gains;
    float normScale = 1.0f;
    std::vector<ChannelState> chans;
};

} // namespace dcr::builtin

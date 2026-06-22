#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

namespace dcr::builtin
{

    // C++ port of the AtmosMeter StereoFreqAnalyzer (the DSP behind the Stereo 3D
    // Pan Scatter meter). Reduces an L/R window to a log-spaced set of frequency
    // bins, each carrying a stereo pan, a signed phase-coherence, and a
    // normalized intensity. Runs on the UI/timer thread (editor side), not the
    // audio thread.
    //
    //   pan        = (|R| - |L|) / (|R| + |L|)        -1 left … +1 right
    //   coherence  = Re(L · conj R) / (|L|·|R|)       -1 anti … +1 in-phase
    //   intensity  = dB-window-mapped combined energy  0 … 1
    class StereoFreqAnalyzer
    {
    public:
        struct Frame
        {
            std::vector<float> freqs; // centre frequency (Hz) per bin
            std::vector<float> pans;
            std::vector<float> cohs;
            std::vector<float> ints;
        };

        StereoFreqAnalyzer (double sampleRate,
                            int fftSizeIn = 2048,
                            int maxPoints = 256,
                            float lowestHz = 20.0f)
            : sr (sampleRate),
              fftSize (fftSizeIn),
              fft ((int) std::log2 ((double) fftSizeIn))
        {
            const float nyq = (float) (sr * 0.5);
            binPos.resize ((size_t) maxPoints);
            freqs.resize ((size_t) maxPoints);
            for (int i = 0; i < maxPoints; ++i)
            {
                const float t = maxPoints > 1 ? (float) i / (float) (maxPoints - 1) : 0.0f;
                const float f = lowestHz * std::pow (nyq / lowestHz, t); // log sweep
                freqs[(size_t) i] = f;
                binPos[(size_t) i] = f * (float) fftSize / (float) sr; // fractional FFT bin
            }

            window.resize ((size_t) fftSize);
            float wsum = 0.0f;
            for (int i = 0; i < fftSize; ++i)
            {
                window[(size_t) i] = 0.5f * (1.0f - std::cos (2.0f * (float) M_PI * (float) i / (float) (fftSize - 1)));
                wsum += window[(size_t) i];
            }
            magNorm = wsum > 0.0f ? 2.0f / wsum : 1.0f; // ~full-scale sine -> ~1.0

            scratchL.assign ((size_t) (2 * fftSize), 0.0f);
            scratchR.assign ((size_t) (2 * fftSize), 0.0f);

            prevP.assign ((size_t) maxPoints, 0.0f);
            prevC.assign ((size_t) maxPoints, 0.0f);
            prevI.assign ((size_t) maxPoints, 0.0f);
        }

        int numBins() const noexcept { return (int) binPos.size(); }
        int windowSize() const noexcept { return fftSize; }

        // Live, set per-tick from the editor's parameters.
        void setIntensityRange (float floorDb, float ceilDb) noexcept
        {
            ceilDB_ = ceilDb;
            floorDB_ = std::min (floorDb, ceilDb - 1.0f);
        }
        void setSmoothing (float alpha) noexcept
        {
            smoothAlpha_ = std::max (0.05f, std::min (1.0f, alpha));
        }

        // L / R must each point to at least `windowSize()` samples (the most
        // recent window). Fills `out`.
        void process (const float* L, const float* R, Frame& out)
        {
            const int M = fftSize;
            std::fill (scratchL.begin(), scratchL.end(), 0.0f);
            std::fill (scratchR.begin(), scratchR.end(), 0.0f);
            for (int i = 0; i < M; ++i)
            {
                scratchL[(size_t) i] = L[i] * window[(size_t) i];
                scratchR[(size_t) i] = R[i] * window[(size_t) i];
            }
            fft.performRealOnlyForwardTransform (scratchL.data());
            fft.performRealOnlyForwardTransform (scratchR.data());

            const int N = (int) binPos.size();
            out.freqs = freqs;
            out.pans.resize ((size_t) N);
            out.cohs.resize ((size_t) N);
            out.ints.resize ((size_t) N);

            const int lastK = M / 2;
            constexpr float eps = 1e-12f;
            const float floorDB = floorDB_, ceilDB = ceilDB_;
            const float span = std::max (1.0f, ceilDB - floorDB);
            const float floorLin = std::pow (10.0f, floorDB / 20.0f);
            const float a = smoothAlpha_;
            const float ia = 1.0f - a;

            for (int i = 0; i < N; ++i)
            {
                const float pos = std::max (0.0f, std::min ((float) lastK, binPos[(size_t) i]));
                const int k0 = (int) pos;
                const int k1 = std::min (lastK, k0 + 1);
                const float w = pos - (float) k0;
                const float iw = 1.0f - w;

                const float lRe = iw * scratchL[(size_t) (2 * k0)] + w * scratchL[(size_t) (2 * k1)];
                const float lIm = iw * scratchL[(size_t) (2 * k0 + 1)] + w * scratchL[(size_t) (2 * k1 + 1)];
                const float rRe = iw * scratchR[(size_t) (2 * k0)] + w * scratchR[(size_t) (2 * k1)];
                const float rIm = iw * scratchR[(size_t) (2 * k0 + 1)] + w * scratchR[(size_t) (2 * k1 + 1)];

                const float mL = std::sqrt (lRe * lRe + lIm * lIm);
                const float mR = std::sqrt (rRe * rRe + rIm * rIm);

                float pan = (mR - mL) / (mR + mL + eps);

                const float reLR = lRe * rRe + lIm * rIm;
                float c = reLR / (mL * mR + eps);
                c = std::max (-1.0f, std::min (1.0f, c));

                const float linNorm = std::sqrt (mL * mL + mR * mR) * magNorm;
                float v = 0.0f;
                if (linNorm > floorLin * magNorm)
                {
                    const float db = 20.0f * std::log10 (std::max (1e-9f, linNorm));
                    v = std::max (0.0f, std::min (1.0f, (db - floorDB) / span));
                }

                // One-pole smoothing toward the previous frame (alpha == 1 → off).
                if (a < 0.999f)
                {
                    pan = a * pan + ia * prevP[(size_t) i];
                    c = a * c + ia * prevC[(size_t) i];
                    v = a * v + ia * prevI[(size_t) i];
                }
                prevP[(size_t) i] = pan;
                prevC[(size_t) i] = c;
                prevI[(size_t) i] = v;

                out.pans[(size_t) i] = pan;
                out.cohs[(size_t) i] = c;
                out.ints[(size_t) i] = v;
            }
        }

    private:
        double sr;
        int fftSize;
        juce::dsp::FFT fft;
        float magNorm = 1.0f;
        std::vector<float> binPos, freqs, window, scratchL, scratchR;
        float floorDB_ = -60.0f, ceilDB_ = 0.0f, smoothAlpha_ = 1.0f;
        std::vector<float> prevP, prevC, prevI;
    };

} // namespace dcr::builtin

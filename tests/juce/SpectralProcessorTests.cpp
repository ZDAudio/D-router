// Regression net for the shared STFT/WOLA engine (SpectralProcessor) that the
// De-esser Pro mode, Auto-EQ and Resonance Suppressor all sit on.  Uses a
// minimal subclass with a flat bin-gain curve so any failure points at the
// analysis/synthesis plumbing itself, not at a subclass's gain logic.
#include "DSP/Builtin/SpectralProcessor.h"

#include <juce_core/juce_core.h>

namespace
{
    // Applies one constant linear gain to every bin.  gain = 1 makes the whole
    // processor an identity-with-latency, which is the WOLA property the
    // reconstruction tests pin down.
    struct FlatGainSpectral : dcr::builtin::SpectralProcessor
    {
        explicit FlatGainSpectral (float gainToApply, int fftOrder = 10, int overlap = 4)
            : SpectralProcessor ("test.flatgain", "FlatGain", {}, fftOrder, overlap),
              flatGain (gainToApply)
        {
        }

        void computeBinGains (const float*, float* gains, int numBins, double, int) override
        {
            for (int k = 0; k < numBins; ++k)
                gains[(size_t) k] = flatGain;
        }

        using SpectralProcessor::getFftSize; // expose for the tests
        using SpectralProcessor::logSmooth;

        float flatGain;
    };

    // Deterministic full-scale-ish noise so runs are reproducible.
    void fillNoise (juce::AudioBuffer<float>& buf, int channel, juce::int64 seed)
    {
        juce::Random rng (seed);
        auto* p = buf.getWritePointer (channel);
        for (int i = 0; i < buf.getNumSamples(); ++i)
            p[i] = rng.nextFloat() * 1.6f - 0.8f;
    }

    // Pushes `input` through the processor in odd-sized blocks (so the test also
    // covers block boundaries that don't divide the FFT hop) and returns the
    // full output stream.
    juce::AudioBuffer<float> processInBlocks (FlatGainSpectral& proc, const juce::AudioBuffer<float>& input, int blockSize)
    {
        juce::AudioBuffer<float> out (input.getNumChannels(), input.getNumSamples());
        juce::MidiBuffer midi;

        for (int start = 0; start < input.getNumSamples(); start += blockSize)
        {
            const int n = juce::jmin (blockSize, input.getNumSamples() - start);
            juce::AudioBuffer<float> block (input.getNumChannels(), n);
            for (int ch = 0; ch < input.getNumChannels(); ++ch)
                block.copyFrom (ch, 0, input, ch, start, n);

            proc.processBlock (block, midi);

            for (int ch = 0; ch < input.getNumChannels(); ++ch)
                out.copyFrom (ch, start, block, ch, 0, n);
        }
        return out;
    }

    // SNR (dB) of out[n] vs gain * in[n - delay], measured past the WOLA
    // warm-up so every compared sample has its full set of overlapped frames.
    double residualSnrDb (const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>& out, int channel, int delay, float gain, int firstSample)
    {
        const float* x = in.getReadPointer (channel);
        const float* y = out.getReadPointer (channel);

        double sig = 0.0, err = 0.0;
        for (int n = firstSample; n < in.getNumSamples(); ++n)
        {
            const double want = (double) gain * x[n - delay];
            sig += want * want;
            err += (y[n] - want) * (y[n] - want);
        }
        return 10.0 * std::log10 (sig / juce::jmax (1.0e-30, err));
    }
} // namespace

struct SpectralProcessorTests : juce::UnitTest
{
    SpectralProcessorTests() : juce::UnitTest ("SpectralProcessor (STFT/WOLA)") {}

    void runTest() override
    {
        constexpr double sr = 48000.0;

        beginTest ("reports one FFT frame of latency");
        {
            FlatGainSpectral proc (1.0f); // default order 10 -> 1024
            proc.prepareToPlay (sr, 512);
            expectEquals (proc.getLatencySamples(), 1024);
        }

        beginTest ("unity gains reconstruct the input (delayed one frame)");
        {
            FlatGainSpectral proc (1.0f);
            proc.prepareToPlay (sr, 512);
            const int fftSize = proc.getFftSize();

            juce::AudioBuffer<float> in (2, fftSize * 8);
            fillNoise (in, 0, 0x5eed0001);
            fillNoise (in, 1, 0x5eed0002);

            // 479 doesn't divide the hop, so frames straddle block boundaries.
            const auto out = processInBlocks (proc, in, 479);

            for (int ch = 0; ch < 2; ++ch)
            {
                const double snr = residualSnrDb (in, out, ch, fftSize, 1.0f, fftSize * 2);
                logMessage ("  ch" + juce::String (ch) + " reconstruction SNR: " + juce::String (snr, 1) + " dB");
                expectGreaterThan (snr, 60.0);
            }
        }

        beginTest ("flat 0.5 gain scales the reconstruction linearly");
        {
            FlatGainSpectral proc (0.5f);
            proc.prepareToPlay (sr, 512);
            const int fftSize = proc.getFftSize();

            juce::AudioBuffer<float> in (2, fftSize * 8);
            fillNoise (in, 0, 0x5eed0003);
            fillNoise (in, 1, 0x5eed0004);

            const auto out = processInBlocks (proc, in, 479);
            const double snr = residualSnrDb (in, out, 0, fftSize, 0.5f, fftSize * 2);
            expectGreaterThan (snr, 60.0);
        }

        beginTest ("zero gains produce exact silence");
        {
            FlatGainSpectral proc (0.0f);
            proc.prepareToPlay (sr, 512);

            juce::AudioBuffer<float> in (2, proc.getFftSize() * 4);
            fillNoise (in, 0, 0x5eed0005);
            fillNoise (in, 1, 0x5eed0006);

            const auto out = processInBlocks (proc, in, 479);
            expectEquals (out.getMagnitude (0, out.getNumSamples()), 0.0f);
        }

        beginTest ("channels stay independent (silent channel stays silent)");
        {
            FlatGainSpectral proc (1.0f);
            proc.prepareToPlay (sr, 512);

            juce::AudioBuffer<float> in (2, proc.getFftSize() * 4);
            fillNoise (in, 0, 0x5eed0007);
            in.clear (1, 0, in.getNumSamples());

            const auto out = processInBlocks (proc, in, 479);
            expectGreaterThan ((double) out.getRMSLevel (0, proc.getFftSize() * 2, proc.getFftSize()), 0.01);
            expectEquals (out.getMagnitude (1, 0, out.getNumSamples()), 0.0f);
        }

        beginTest ("logSmooth preserves a constant curve");
        {
            std::vector<float> src ((size_t) 513, 0.7f), dst ((size_t) 513, 0.0f);
            FlatGainSpectral::logSmooth (src.data(), dst.data(), 513, 3.0f);
            for (auto v : dst)
                expectWithinAbsoluteError (v, 0.7f, 1.0e-4f);
        }

        beginTest ("non-default FFT order still reconstructs (order 11, overlap 4)");
        {
            FlatGainSpectral proc (1.0f, 11, 4);
            proc.prepareToPlay (sr, 512);
            const int fftSize = proc.getFftSize();
            expectEquals (proc.getLatencySamples(), fftSize);

            juce::AudioBuffer<float> in (2, fftSize * 8);
            fillNoise (in, 0, 0x5eed0008);
            fillNoise (in, 1, 0x5eed0009);

            const auto out = processInBlocks (proc, in, 479);
            const double snr = residualSnrDb (in, out, 0, fftSize, 1.0f, fftSize * 2);
            logMessage ("  order-11 reconstruction SNR: " + juce::String (snr, 1) + " dB");
            expectGreaterThan (snr, 60.0);
        }
    }
};

static SpectralProcessorTests spectralProcessorTests;

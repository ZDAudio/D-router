#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <array>
#include <atomic>
#include <cmath>

namespace dcr::builtin
{

    // ===========================================================================
    // PPM Meter -- a precision quasi-peak meter (pass-through audio).  Supplements
    // the matrix's fast peak meters with proper broadcast PPM ballistics: a defined
    // integration (attack) time and a slow standardised fallback, plus a peak-hold
    // dot.  Per-channel readouts are published as atomics for PpmMeterEditor.
    // ===========================================================================
    class PpmMeterProcessor : public BuiltinProcessor
    {
    public:
        static constexpr int kMaxMeterChannels = 16;

        PpmMeterProcessor() : BuiltinProcessor (ids::ppm, "PPM Meter", createLayout()) {}

        static APVTS::ParameterLayout createLayout()
        {
            APVTS::ParameterLayout l;
            // Ballistics standard.  Index maps to kStandards[] below.
            l.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "standard", 1 }, "Standard", juce::StringArray { "EBU (IEC IIb)", "BBC (IEC IIa)", "DIN 45406", "Nordic", "Digital peak" }, 0));
            // dBFS that corresponds to 0 dBu / alignment level, for the dBu scale.
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "align", 1 }, "Align 0dBu", juce::NormalisableRange<float> (-24.0f, -9.0f, 0.1f), -18.0f, juce::AudioParameterFloatAttributes().withLabel ("dBFS")));
            return l;
        }

        // ---- live readouts for the editor (dBFS) -------------------------------
        int getMeterChannels() const noexcept { return juce::jlimit (1, kMaxMeterChannels, preparedChannels); }
        float getQuasiPeakDb (int ch) const noexcept
        {
            return (ch >= 0 && ch < kMaxMeterChannels) ? qpDb[(size_t) ch].load (std::memory_order_relaxed) : -100.0f;
        }
        float getPeakHoldDb (int ch) const noexcept
        {
            return (ch >= 0 && ch < kMaxMeterChannels) ? holdDb[(size_t) ch].load (std::memory_order_relaxed) : -100.0f;
        }
        float getAlignDbFs() const noexcept { return param ("align"); }

        juce::AudioProcessorEditor* createEditor() override; // PpmMeterEditor.h

    protected:
        struct Standard
        {
            const char* name;
            float attackMs;
            float fallbackDbPerSec;
        };
        static const Standard& standardAt (int i)
        {
            // attackMs = time-constant of the rectified-peak integrator;
            // fallbackDbPerSec = slow dB-linear decay (the PPM "return time").
            static const Standard kStandards[5] = {
                { "EBU", 10.0f, 11.76f }, // 20 dB in 1.7 s
                { "BBC", 10.0f, 8.57f }, // 24 dB in 2.8 s
                { "DIN", 10.0f, 13.33f }, // 20 dB in 1.5 s
                { "Nordic", 5.0f, 13.33f }, // faster integration
                { "Peak", 0.2f, 26.0f }, // near-true digital peak, quick fall
            };
            return kStandards[juce::jlimit (0, 4, i)];
        }

        void prepareDsp (double sr, int, int) override
        {
            dspSampleRate = sr;
            for (auto& v : qp)
                v = 0.0f;
            for (auto& v : hold)
                v = 0.0f;
            for (auto& v : qpDb)
                v.store (-100.0f, std::memory_order_relaxed);
            for (auto& v : holdDb)
                v.store (-100.0f, std::memory_order_relaxed);
            holdCounter.fill (0);
        }

        void processDsp (juce::AudioBuffer<float>& buffer) override
        {
            const int ns = buffer.getNumSamples();
            const int nch = juce::jmin (buffer.getNumChannels(), kMaxMeterChannels);

            const auto& st = standardAt ((int) param ("standard"));
            const float attackCoeff = 1.0f - std::exp (-1.0f / (float) (st.attackMs * 0.001 * dspSampleRate));
            // dB-linear fallback -> per-sample multiplicative factor.
            const float decayMul = std::pow (10.0f, -(st.fallbackDbPerSec) / (20.0f * (float) dspSampleRate));
            const int holdSamples = (int) (1.2 * dspSampleRate); // 1.2 s peak hold

            for (int ch = 0; ch < nch; ++ch)
            {
                const float* x = buffer.getReadPointer (ch);
                float env = qp[(size_t) ch];
                float peak = hold[(size_t) ch];
                int hc = holdCounter[(size_t) ch];

                for (int i = 0; i < ns; ++i)
                {
                    const float r = std::abs (x[i]);
                    if (r > env)
                        env += (r - env) * attackCoeff; // integrate up
                    else
                        env *= decayMul; // PPM fallback

                    if (env >= peak)
                    {
                        peak = env;
                        hc = holdSamples;
                    }
                    else if (--hc <= 0)
                    {
                        peak *= decayMul;
                        hc = 0;
                    }
                }

                qp[(size_t) ch] = env;
                hold[(size_t) ch] = peak;
                holdCounter[(size_t) ch] = hc;
                qpDb[(size_t) ch].store (juce::Decibels::gainToDecibels (env, -100.0f), std::memory_order_relaxed);
                holdDb[(size_t) ch].store (juce::Decibels::gainToDecibels (peak, -100.0f), std::memory_order_relaxed);
            }
            // Audio is passed through unchanged (this is a meter).
        }

    private:
        std::array<float, kMaxMeterChannels> qp {}; // running quasi-peak (linear)
        std::array<float, kMaxMeterChannels> hold {}; // peak-hold (linear)
        std::array<int, kMaxMeterChannels> holdCounter {};
        std::array<std::atomic<float>, kMaxMeterChannels> qpDb {};
        std::array<std::atomic<float>, kMaxMeterChannels> holdDb {};
    };

} // namespace dcr::builtin

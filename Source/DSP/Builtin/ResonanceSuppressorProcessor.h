#pragma once

#include "DSP/Builtin/ResonanceMath.h"
#include "DSP/Builtin/SpectralNodeMath.h"
#include "DSP/Builtin/SpectralProcessor.h"

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace dcr::builtin
{

    // ===========================================================================
    // Resonance Suppressor -- finds bins that stick out above the smooth spectral
    // baseline (resonances / harshness) and dynamically ducks them, with
    // attack/release per bin so it only acts while the resonance rings.  Built on
    // the shared STFT engine.
    //
    // Peer of the Spectral Auto-EQ: a log-frequency interactive editor drives a
    // 31-node *threshold-offset* curve (per-frequency dB added to the global Depth
    // threshold -- drag a region down to chase resonances harder there, up to
    // protect it).  The curve is handed to the audio thread lock-free (per-node
    // atomics + a generation counter) and persisted as a <CURVE> child of the APVTS
    // state.  A Resolution control (1/3..1/24 oct) sets both the baseline broadness
    // (how wide a hump still counts as a resonance) and the editor's display
    // banding.  The deterministic math (node log-grid, threshold->reduction,
    // baseline strength) lives JUCE-free in ResonanceMath.h and is unit-tested.
    // ===========================================================================
    class ResonanceSuppressorProcessor : public SpectralProcessor
    {
    public:
        static constexpr int kNumNodes = 31; // threshold-offset nodes, log grid 20 Hz..20 kHz
        static constexpr int kMaxDisp = 384; // upper bound on 1/24-oct display bands (20 Hz..20 kHz)

        ResonanceSuppressorProcessor()
            : SpectralProcessor (ids::resonance, "Resonance Suppressor", createLayout(), /*fftOrder*/ 11, /*overlap*/ 4)
        {
            for (auto& n : nodes)
                n.store (0.0f, std::memory_order_relaxed);
        }

        static APVTS::ParameterLayout createLayout()
        {
            APVTS::ParameterLayout l;
            l.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "res", 1 }, "Resolution", juce::StringArray { "1/3 oct", "1/6 oct", "1/12 oct", "1/24 oct" }, 1));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "depth", 1 }, "Depth", juce::NormalisableRange<float> (0.0f, 12.0f, 0.1f), 4.0f, juce::AudioParameterFloatAttributes().withLabel ("dB")));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "sharpness", 1 }, "Sharpness", juce::NormalisableRange<float> (0.1f, 2.0f, 0.01f), 0.8f));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "reduction", 1 }, "Max reduction", juce::NormalisableRange<float> (1.0f, 24.0f, 0.1f), 12.0f, juce::AudioParameterFloatAttributes().withLabel ("dB")));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "attack", 1 }, "Attack", juce::NormalisableRange<float> (1.0f, 100.0f, 0.1f, 0.5f), 8.0f, juce::AudioParameterFloatAttributes().withLabel ("ms")));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "release", 1 }, "Release", juce::NormalisableRange<float> (10.0f, 500.0f, 1.0f, 0.4f), 120.0f, juce::AudioParameterFloatAttributes().withLabel ("ms")));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "minfreq", 1 }, "Min freq", juce::NormalisableRange<float> (20.0f, 2000.0f, 1.0f, 0.3f), 150.0f, juce::AudioParameterFloatAttributes().withLabel ("Hz")));
            return l;
        }

        juce::AudioProcessorEditor* createEditor() override; // ResonanceSuppressorEditor.h

        // ---- display interface (read by the editor; racy-but-harmless) ---------
        // Bands are 1/N-octave, log-spaced; index them with getDisplayFreq() for an
        // x position, getDisplayMagDb() for the spectrum, getDisplayBaseDb() for the
        // smooth baseline, getDisplayGainDb() for the applied reduction (<= 0).
        int getDisplaySize() const noexcept { return dispCount.load(); }
        float getDisplayFreq (int i) const noexcept { return inDisp (i) ? dispFreq[(size_t) i] : 20.0f; }
        float getDisplayMagDb (int i) const noexcept { return inDisp (i) ? dispMag[(size_t) i] : -100.0f; }
        float getDisplayBaseDb (int i) const noexcept { return inDisp (i) ? dispBase[(size_t) i] : -100.0f; }
        float getDisplayGainDb (int i) const noexcept { return inDisp (i) ? dispGain[(size_t) i] : 0.0f; }

        // ---- threshold-offset nodes (read/write by the editor) -----------------
        static float nodeFreq (int i) noexcept { return resonance::nodeFreq (i, kNumNodes); }
        float getNodeDb (int i) const noexcept
        {
            return (i >= 0 && i < kNumNodes) ? nodes[(size_t) i].load (std::memory_order_relaxed) : 0.0f;
        }
        void setNodeDb (int i, float db) noexcept
        {
            if (i < 0 || i >= kNumNodes)
                return;
            nodes[(size_t) i].store (dcr::spectral::sanitizeNodeDb (db), std::memory_order_relaxed);
            curveGen.fetch_add (1, std::memory_order_release); // publish to the audio thread
        }
        void resetNode (int i) noexcept { setNodeDb (i, 0.0f); }

        // ---- label info for the editor -----------------------------------------
        int labelFftSize() const noexcept { return getFftSize(); }
        int labelOverlapPct() const noexcept { return 100 - 100 / juce::jmax (1, getOverlapFactor()); }
        const char* labelWindow() const noexcept { return getWindowName(); }
        int resolutionN() const noexcept { return nForRes ((int) param ("res")); }

        // ---- state: APVTS XML + a <CURVE> child holding the node dB values ------
        void getStateInformation (juce::MemoryBlock& dest) override
        {
            auto xml = apvts.copyState().createXml();
            if (xml == nullptr)
                return;
            auto* c = xml->createNewChildElement ("CURVE");
            for (int i = 0; i < kNumNodes; ++i)
                c->setAttribute ("n" + juce::String (i), (double) nodes[(size_t) i].load (std::memory_order_relaxed));
            copyXmlToBinary (*xml, dest);
        }
        void setStateInformation (const void* data, int size) override
        {
            auto xml = getXmlFromBinary (data, size);
            if (xml == nullptr)
                return;
            if (auto* c = xml->getChildByName ("CURVE"))
                for (int i = 0; i < kNumNodes; ++i)
                    // Untrusted blob: clamp + reject NaN/Inf before it reaches the audio thread.
                    nodes[(size_t) i].store (dcr::spectral::sanitizeNodeDb (c->getDoubleAttribute ("n" + juce::String (i), 0.0)),
                        std::memory_order_relaxed);
            curveGen.fetch_add (1, std::memory_order_release);
            // The <CURVE> child is ours, not a parameter -- strip it before the
            // APVTS adopts the tree so it doesn't accumulate on the next save.
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.removeChild (tree.getChildWithName ("CURVE"), nullptr);
            apvts.replaceState (tree);
        }

    protected:
        void prepareSpectral (double sr, int numBins, int numChannels) override
        {
            nyquist = (float) (sr * 0.5);
            binHz = (float) (sr / (double) getFftSize());

            const int nch = juce::jmax (1, numChannels); // matches the base's channel FIFO count
            redDb.assign ((size_t) nch, std::vector<float> ((size_t) numBins, 0.0f));
            baseWork.assign ((size_t) numBins, 0.0f);
            magDbWork.assign ((size_t) numBins, -100.0f);
            cumSum.assign ((size_t) numBins + 1, 0.0f);
            curveOffset.assign ((size_t) numBins, 0.0f);

            // per-bin -> node interpolation map (depends only on sr/fftSize)
            nodeLo.assign ((size_t) numBins, 0);
            nodeFrac.assign ((size_t) numBins, 0.0f);
            buildNodeMap (numBins);

            // display-band scaffolding, sized for the finest resolution so a runtime
            // resolution change never allocates.
            dispFreq.assign ((size_t) kMaxDisp, 20.0f);
            dispBinLo.assign ((size_t) kMaxDisp, 0);
            dispBinHi.assign ((size_t) kMaxDisp, 0);
            dispBinCtr.assign ((size_t) kMaxDisp, 0);
            dispMag.fill (-100.0f);
            dispBase.fill (-100.0f);
            dispGain.fill (0.0f);

            frameSec = (double) getHopSize() / sr; // seconds per STFT hop
            cachedResN = -1; // force display rebuild on first frame
            rebuildCurveFromNodes (numBins);
            lastCurveGen = curveGen.load (std::memory_order_acquire);
        }

        void computeBinGains (const float* mags, float* gains, int numBins, double /*sr*/, int channel) override
        {
            const int resN = nForRes ((int) param ("res"));
            const float depth = param ("depth");
            const float sharp = param ("sharpness");
            const float maxRed = param ("reduction");
            const float attMs = juce::jmax (1.0f, param ("attack"));
            const float relMs = juce::jmax (1.0f, param ("release"));
            const float minFreq = param ("minfreq");
            const float attCoeff = 1.0f - std::exp (-(float) frameSec / (attMs * 0.001f));
            const float relCoeff = 1.0f - std::exp (-(float) frameSec / (relMs * 0.001f));
            const float strength = resonance::baseStrengthForRes (resN);
            const int minBin = juce::jlimit (1, numBins - 1, (int) (minFreq / juce::jmax (1.0f, binHz)));

            // Rebuild display bands only when the resolution changes.
            if (resN != cachedResN)
            {
                rebuildDisplayBands (numBins, resN);
                cachedResN = resN;
            }
            // Rebuild the per-bin threshold offset only when the editor moved a node.
            const int gen = curveGen.load (std::memory_order_acquire);
            if (gen != lastCurveGen)
            {
                rebuildCurveFromNodes (numBins);
                lastCurveGen = gen;
            }

            for (int k = 0; k < numBins; ++k)
                magDbWork[(size_t) k] = juce::Decibels::gainToDecibels (mags[k], -120.0f);

            // Broad baseline = the "smooth" spectrum a resonance pokes through; its
            // tightness is set by the Resolution control.
            logSmooth (magDbWork.data(), baseWork.data(), numBins, strength);

            auto& rch = redDb[(size_t) channel];
            for (int k = 0; k < numBins; ++k)
            {
                float targetRed = 0.0f; // <= 0
                if (k >= minBin)
                {
                    const float thr = depth + curveOffset[(size_t) k];
                    targetRed = resonance::targetReductionDb (magDbWork[(size_t) k], baseWork[(size_t) k], thr, sharp, maxRed);
                }

                // Attack when ducking harder (target more negative), release when easing.
                float& r = rch[(size_t) k];
                const float c = (targetRed < r) ? attCoeff : relCoeff;
                r += (targetRed - r) * c;
                gains[k] = juce::Decibels::decibelsToGain (r);
            }

            if (channel == 0)
                updateDisplay (numBins);
        }

    private:
        static int nForRes (int idx) noexcept
        {
            switch (juce::jlimit (0, 3, idx))
            {
                case 0:
                    return 3;
                case 1:
                    return 6;
                case 2:
                    return 12;
                default:
                    return 24;
            }
        }
        bool inDisp (int i) const noexcept { return i >= 0 && i < dispCount.load(); }

        // per-bin -> node interpolation weights (sr/fftSize dependent only)
        void buildNodeMap (int numBins)
        {
            for (int k = 0; k < numBins; ++k)
            {
                const float f = juce::jmax (1.0f, (float) k / (float) (numBins - 1) * nyquist);
                const auto ni = resonance::nodeInterp (f, kNumNodes);
                nodeLo[(size_t) k] = ni.lo;
                nodeFrac[(size_t) k] = ni.frac;
            }
        }

        void rebuildCurveFromNodes (int numBins)
        {
            float nv[kNumNodes];
            for (int i = 0; i < kNumNodes; ++i)
                nv[i] = nodes[(size_t) i].load (std::memory_order_relaxed);
            for (int k = 0; k < numBins; ++k)
            {
                const int lo = nodeLo[(size_t) k];
                curveOffset[(size_t) k] = nv[lo] + (nv[lo + 1] - nv[lo]) * nodeFrac[(size_t) k];
            }
        }

        // 1/N-octave log-spaced display bands across 20 Hz .. min(nyq, 20 kHz).
        void rebuildDisplayBands (int numBins, int N)
        {
            const float step = std::pow (2.0f, 1.0f / (float) N);
            const float halfO = 1.0f / (2.0f * (float) N);
            const float loMul = std::pow (2.0f, -halfO), hiMul = std::pow (2.0f, halfO);
            const float fTop = juce::jmin (nyquist, 20000.0f);
            int count = 0;
            for (float fc = 20.0f; fc <= fTop && count < kMaxDisp; fc *= step, ++count)
            {
                dispFreq[(size_t) count] = fc;
                dispBinLo[(size_t) count] = juce::jlimit (0, numBins - 1, (int) std::floor (fc * loMul / binHz));
                dispBinHi[(size_t) count] = juce::jlimit (dispBinLo[(size_t) count], numBins - 1, (int) std::ceil (fc * hiMul / binHz));
                dispBinCtr[(size_t) count] = juce::jlimit (0, numBins - 1, (int) std::lround (fc / binHz));
            }
            dispCount.store (count);
        }

        void updateDisplay (int numBins)
        {
            // Cumulative sum of ch0 magnitudes for O(1) per-band means.
            cumSum[0] = 0.0f;
            for (int k = 0; k < numBins; ++k)
                cumSum[(size_t) (k + 1)] = cumSum[(size_t) k] + magDbWork[(size_t) k];

            const int n = dispCount.load();
            for (int i = 0; i < n; ++i)
            {
                const int lo = dispBinLo[(size_t) i], hi = dispBinHi[(size_t) i];
                dispMag[(size_t) i] = (cumSum[(size_t) (hi + 1)] - cumSum[(size_t) lo]) / (float) (hi - lo + 1);
                dispBase[(size_t) i] = baseWork[(size_t) dispBinCtr[(size_t) i]];
                dispGain[(size_t) i] = redDb[0][(size_t) dispBinCtr[(size_t) i]];
            }
        }

        // --- DSP scratch (all pre-sized in prepareSpectral) ---------------------
        std::vector<std::vector<float>> redDb; // [channel][bin] smoothed reduction dB (<= 0)
        std::vector<float> baseWork, magDbWork, cumSum, curveOffset;
        std::vector<int> nodeLo;
        std::vector<float> nodeFrac;
        double frameSec = 0.01;
        float nyquist = 24000.0f, binHz = 23.4375f;
        int cachedResN = -1;

        // --- display snapshots (ch0) --------------------------------------------
        std::vector<float> dispFreq;
        std::vector<int> dispBinLo, dispBinHi, dispBinCtr;
        std::array<float, kMaxDisp> dispMag {};
        std::array<float, kMaxDisp> dispBase {};
        std::array<float, kMaxDisp> dispGain {};
        std::atomic<int> dispCount { 0 };

        // --- threshold-offset curve (editor <-> audio thread, lock-free) --------
        std::array<std::atomic<float>, kNumNodes> nodes;
        std::atomic<int> curveGen { 0 };
        int lastCurveGen = 0;
    };

} // namespace dcr::builtin

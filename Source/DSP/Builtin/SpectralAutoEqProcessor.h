#pragma once

#include "DSP/Builtin/SpectralNodeMath.h"
#include "DSP/Builtin/SpectralProcessor.h"

#include <array>
#include <atomic>
#include <cmath>
#include <vector>

namespace dcr::builtin
{

    // ===========================================================================
    // Spectral Auto-EQ -- continuously balances tone by pushing each fractional-
    // octave band toward a *reference* curve.  Two reference modes (the "mode"
    // param):
    //   * Offset : reference = broadband self-trend + user target.  Program-adaptive
    //              and gentle; a flat target reproduces the classic "flatten each
    //              band toward the signal's own trend" behaviour.
    //   * Match  : reference = the user target shape, level-anchored to the program
    //              (classic match-EQ -- imposes the drawn tonal balance, ignoring
    //              absolute level so it doesn't over-correct quiet passages).
    // Peaks above the reference are tamed, dips below are recovered; a tilt control
    // brightens / darkens; the whole correction is time-smoothed per bin so it
    // rides the program without pumping.
    //
    // The analysis/correction band width is selectable (1/3 .. 1/24 octave via the
    // "res" param) and the same resolution drives the editor's display banding.
    // The target curve is 31 draggable nodes on a log-frequency grid (20 Hz..20 kHz)
    // edited by SpectralAutoEqEditor; values are handed to the audio thread lock-free
    // (per-node atomics + a generation counter) and persisted as a <TARGET> child of
    // the APVTS state.  Built on the shared STFT engine.
    // ===========================================================================
    class SpectralAutoEqProcessor : public SpectralProcessor
    {
    public:
        static constexpr int kNumNodes = 31; // target nodes, log grid 20 Hz..20 kHz (~1/3 oct apart)
        static constexpr int kMaxDisp = 384; // upper bound on 1/24-oct display bands (20 Hz..20 kHz)

        SpectralAutoEqProcessor()
            : SpectralProcessor (ids::autoeq, "Spectral Auto-EQ", createLayout(), /*fftOrder*/ 10, /*overlap*/ 4)
        {
            for (auto& n : nodes)
                n.store (0.0f, std::memory_order_relaxed);
        }

        static APVTS::ParameterLayout createLayout()
        {
            APVTS::ParameterLayout l;
            l.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "mode", 1 }, "Target mode", juce::StringArray { "Offset", "Match" }, 0));
            l.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "res", 1 }, "Resolution", juce::StringArray { "1/3 oct", "1/6 oct", "1/12 oct", "1/24 oct" }, 1));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "tame", 1 }, "Tame", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.45f));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "recover", 1 }, "Recover", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.30f));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "tilt", 1 }, "Brighten", juce::NormalisableRange<float> (-1.0f, 1.0f, 0.01f), 0.0f));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "range", 1 }, "Max change", juce::NormalisableRange<float> (1.0f, 18.0f, 0.1f), 9.0f, juce::AudioParameterFloatAttributes().withLabel ("dB")));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "amount", 1 }, "Amount", juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.6f));
            l.add (std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { "speed", 1 }, "Speed", juce::NormalisableRange<float> (5.0f, 500.0f, 1.0f, 0.4f), 80.0f, juce::AudioParameterFloatAttributes().withLabel ("ms")));
            return l;
        }

        juce::AudioProcessorEditor* createEditor() override; // SpectralAutoEqEditor.h

        // ---- display interface (read by the editor; racy-but-harmless) ---------
        // Bands are 1/N-octave, log-spaced; index them with getDisplayFreq() for an
        // x position, getDisplayMagDb() for the spectrum, getDisplayGainDb() for the
        // applied correction.
        int getDisplaySize() const noexcept { return dispCount.load(); }
        float getDisplayFreq (int i) const noexcept { return inDisp (i) ? dispFreq[(size_t) i] : 20.0f; }
        float getDisplayMagDb (int i) const noexcept { return inDisp (i) ? dispMag[(size_t) i] : -100.0f; }
        float getDisplayGainDb (int i) const noexcept { return inDisp (i) ? dispGain[(size_t) i] : 0.0f; }

        // ---- target nodes (read/write by the editor) ---------------------------
        static float nodeFreq (int i) noexcept
        {
            const float t = (float) juce::jlimit (0, kNumNodes - 1, i) / (float) (kNumNodes - 1);
            return 20.0f * std::pow (1000.0f, t); // 20 Hz .. 20 kHz, log-spaced
        }
        float getNodeDb (int i) const noexcept
        {
            return (i >= 0 && i < kNumNodes) ? nodes[(size_t) i].load (std::memory_order_relaxed) : 0.0f;
        }
        void setNodeDb (int i, float db) noexcept
        {
            if (i < 0 || i >= kNumNodes)
                return;
            nodes[(size_t) i].store (dcr::spectral::sanitizeNodeDb (db), std::memory_order_relaxed);
            targetGen.fetch_add (1, std::memory_order_release); // publish to the audio thread
        }
        void resetNode (int i) noexcept { setNodeDb (i, 0.0f); }

        // ---- label info for the editor -----------------------------------------
        int labelFftSize() const noexcept { return getFftSize(); }
        int labelOverlapPct() const noexcept { return 100 - 100 / juce::jmax (1, getOverlapFactor()); }
        const char* labelWindow() const noexcept { return getWindowName(); }
        int resolutionN() const noexcept { return nForRes ((int) param ("res")); }
        int targetMode() const noexcept { return (int) param ("mode"); }

        // ---- state: APVTS XML + a <TARGET> child holding the node dB values ----
        void getStateInformation (juce::MemoryBlock& dest) override
        {
            auto xml = apvts.copyState().createXml();
            if (xml == nullptr)
                return;
            auto* t = xml->createNewChildElement ("TARGET");
            for (int i = 0; i < kNumNodes; ++i)
                t->setAttribute ("n" + juce::String (i), (double) nodes[(size_t) i].load (std::memory_order_relaxed));
            copyXmlToBinary (*xml, dest);
        }
        void setStateInformation (const void* data, int size) override
        {
            auto xml = getXmlFromBinary (data, size);
            if (xml == nullptr)
                return;
            if (auto* t = xml->getChildByName ("TARGET"))
                for (int i = 0; i < kNumNodes; ++i)
                    // Untrusted blob: clamp + reject NaN/Inf before it reaches the audio thread.
                    nodes[(size_t) i].store (dcr::spectral::sanitizeNodeDb (t->getDoubleAttribute ("n" + juce::String (i), 0.0)),
                        std::memory_order_relaxed);
            targetGen.fetch_add (1, std::memory_order_release);
            // The <TARGET> child is ours, not a parameter -- strip it before the
            // APVTS adopts the tree so it doesn't accumulate on the next save.
            auto tree = juce::ValueTree::fromXml (*xml);
            tree.removeChild (tree.getChildWithName ("TARGET"), nullptr);
            apvts.replaceState (tree);
        }

    protected:
        void prepareSpectral (double sr, int numBins, int numChannels) override
        {
            nyquist = (float) (sr * 0.5);
            binHz = (float) (sr / (double) getFftSize());

            const int nch = juce::jmax (1, numChannels); // matches the base's channel FIFO count
            gainDb.assign ((size_t) nch, std::vector<float> ((size_t) numBins, 0.0f));
            envWork.assign ((size_t) numBins, 0.0f);
            refWork.assign ((size_t) numBins, 0.0f);
            magDbWork.assign ((size_t) numBins, -100.0f);
            cumSum.assign ((size_t) numBins + 1, 0.0f);
            targetDb.assign ((size_t) numBins, 0.0f);
            bandLo.assign ((size_t) numBins, 0);
            bandHi.assign ((size_t) numBins, 0);

            // per-bin -> target-node interpolation map (depends only on sr/fftSize)
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
            dispGain.fill (0.0f);

            smoothBaseSec = (double) getHopSize() / sr;

            cachedResN = -1; // force band/display rebuild on first frame
            rebuildTargetFromNodes (numBins);
            lastTargetGen = targetGen.load (std::memory_order_acquire);
        }

        void computeBinGains (const float* mags, float* gains, int numBins, double /*sr*/, int channel) override
        {
            const int mode = (int) param ("mode");
            const int resN = nForRes ((int) param ("res"));
            const float tame = param ("tame");
            const float recover = param ("recover");
            const float tilt = param ("tilt");
            const float rangeDb = param ("range");
            const float amount = param ("amount");
            const float speedMs = juce::jmax (5.0f, param ("speed"));
            const float tcoeff = 1.0f - std::exp (-(float) smoothBaseSec / (speedMs * 0.001f));

            // Rebuild band edges + display bands only when the resolution changes.
            if (resN != cachedResN)
            {
                rebuildBandEdges (numBins, resN);
                rebuildDisplayBands (numBins, resN);
                cachedResN = resN;
            }
            // Rebuild the per-bin target only when the editor moved a node.
            const int gen = targetGen.load (std::memory_order_acquire);
            if (gen != lastTargetGen)
            {
                rebuildTargetFromNodes (numBins);
                lastTargetGen = gen;
            }

            // magnitudes -> dB, then a cumulative sum for O(1) fractional-octave means.
            for (int k = 0; k < numBins; ++k)
                magDbWork[(size_t) k] = juce::Decibels::gainToDecibels (mags[k], -120.0f);
            cumSum[0] = 0.0f;
            for (int k = 0; k < numBins; ++k)
                cumSum[(size_t) (k + 1)] = cumSum[(size_t) k] + magDbWork[(size_t) k];

            // 1/N-octave envelope (mean dB over each bin's band).
            for (int k = 0; k < numBins; ++k)
            {
                const int lo = bandLo[(size_t) k], hi = bandHi[(size_t) k];
                envWork[(size_t) k] = (cumSum[(size_t) (hi + 1)] - cumSum[(size_t) lo])
                                      / (float) (hi - lo + 1);
            }

            // Offset rides the broad self-trend; Match rides a level anchor so the
            // target imposes a shape rather than an absolute level.
            float levelAnchor = 0.0f;
            if (mode == 0)
                logSmooth (magDbWork.data(), refWork.data(), numBins, 1.5f); // broad trend
            else
                levelAnchor = computeLevelAnchor (numBins);

            auto& gch = gainDb[(size_t) channel];

            for (int k = 0; k < numBins; ++k)
            {
                const float reference = (mode == 0) ? (refWork[(size_t) k] + targetDb[(size_t) k])
                                                    : (targetDb[(size_t) k] + levelAnchor);
                const float dev = envWork[(size_t) k] - reference; // +peak / -dip vs reference
                float corr = (dev > 0.0f) ? -dev * tame : -dev * recover; // push toward reference

                // Tilt: +/- dB across the spectrum (log-frequency).
                const float f = (float) k / (float) (numBins - 1) * nyquist;
                const float oct = std::log2 (juce::jmax (20.0f, f) / 1000.0f);
                corr += tilt * 1.5f * oct;

                corr = juce::jlimit (-rangeDb, rangeDb, corr * amount);

                float& g = gch[(size_t) k];
                g += (corr - g) * tcoeff; // time-smooth per bin
                gains[k] = juce::Decibels::decibelsToGain (g);
            }

            if (channel == 0)
                updateDisplay();
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

        // per-bin -> target node interpolation weights (sr/fftSize dependent only)
        void buildNodeMap (int numBins)
        {
            const float denom = std::log (1000.0f);
            for (int k = 0; k < numBins; ++k)
            {
                const float f = juce::jmax (1.0f, (float) k / (float) (numBins - 1) * nyquist);
                float ir = std::log (juce::jlimit (20.0f, 20000.0f, f) / 20.0f) / denom * (float) (kNumNodes - 1);
                ir = juce::jlimit (0.0f, (float) (kNumNodes - 1), ir);
                const int lo = juce::jlimit (0, kNumNodes - 2, (int) std::floor (ir));
                nodeLo[(size_t) k] = lo;
                nodeFrac[(size_t) k] = juce::jlimit (0.0f, 1.0f, ir - (float) lo);
            }
        }

        void rebuildTargetFromNodes (int numBins)
        {
            float nv[kNumNodes];
            for (int i = 0; i < kNumNodes; ++i)
                nv[i] = nodes[(size_t) i].load (std::memory_order_relaxed);
            for (int k = 0; k < numBins; ++k)
            {
                const int lo = nodeLo[(size_t) k];
                targetDb[(size_t) k] = nv[lo] + (nv[lo + 1] - nv[lo]) * nodeFrac[(size_t) k];
            }
        }

        // For each bin, the [lo,hi] bin span of its 1/N-octave band.
        void rebuildBandEdges (int numBins, int N)
        {
            const float halfOct = 1.0f / (2.0f * (float) N);
            const float loMul = std::pow (2.0f, -halfOct), hiMul = std::pow (2.0f, halfOct);
            for (int k = 0; k < numBins; ++k)
            {
                const float f = (float) k / (float) (numBins - 1) * nyquist;
                int lo = juce::jlimit (0, numBins - 1, (int) std::floor (f * loMul / binHz));
                int hi = juce::jlimit (lo, numBins - 1, (int) std::ceil (f * hiMul / binHz));
                bandLo[(size_t) k] = lo;
                bandHi[(size_t) k] = hi;
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

        // Log-weighted mean of (envelope - target) over ~40 Hz..16 kHz: the level the
        // Match-mode target should sit at so only its *shape* is imposed.
        float computeLevelAnchor (int numBins) const
        {
            const int kMin = juce::jlimit (1, numBins - 1, (int) (40.0f / binHz));
            const int kMax = juce::jlimit (kMin, numBins - 1, (int) (16000.0f / binHz));
            double accum = 0.0, wsum = 0.0;
            for (int k = kMin; k <= kMax; ++k)
            {
                const double w = 1.0 / (double) (k + 1); // ~equal weight per octave
                accum += w * (double) (envWork[(size_t) k] - targetDb[(size_t) k]);
                wsum += w;
            }
            return wsum > 0.0 ? (float) (accum / wsum) : 0.0f;
        }

        void updateDisplay()
        {
            // Editor display snapshot.  Called ONLY for channel 0 (see computeBinGains) and it
            // depends on that: cumSum still holds channel 0's magnitudes (rebuilt per channel at
            // the top of computeBinGains, ch0 first) and gainDb[0] is channel 0's correction.  If
            // the channel loop is ever reordered or this runs off ch0, snapshot the source here.
            const int n = dispCount.load();
            for (int i = 0; i < n; ++i)
            {
                const int lo = dispBinLo[(size_t) i], hi = dispBinHi[(size_t) i];
                dispMag[(size_t) i] = (cumSum[(size_t) (hi + 1)] - cumSum[(size_t) lo]) / (float) (hi - lo + 1);
                dispGain[(size_t) i] = gainDb[0][(size_t) dispBinCtr[(size_t) i]];
            }
        }

        // --- DSP scratch (all pre-sized in prepareSpectral) ---------------------
        std::vector<std::vector<float>> gainDb; // [channel][bin] smoothed correction dB
        std::vector<float> envWork, refWork, magDbWork, cumSum, targetDb;
        std::vector<int> bandLo, bandHi, nodeLo;
        std::vector<float> nodeFrac;
        double smoothBaseSec = 0.005;
        float nyquist = 24000.0f, binHz = 46.875f;
        int cachedResN = -1;

        // --- display snapshots (ch0) --------------------------------------------
        std::vector<float> dispFreq;
        std::vector<int> dispBinLo, dispBinHi, dispBinCtr;
        std::array<float, kMaxDisp> dispMag {};
        std::array<float, kMaxDisp> dispGain {};
        std::atomic<int> dispCount { 0 };

        // --- target curve (editor <-> audio thread, lock-free) ------------------
        std::array<std::atomic<float>, kNumNodes> nodes;
        std::atomic<int> targetGen { 0 };
        int lastTargetGen = 0;
    };

} // namespace dcr::builtin

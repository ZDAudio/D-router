#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <memory>
#include <vector>

// ============================================================================
// Built-in DSP modules.
//
// Each module is a juce::AudioPluginInstance subclass so it drops straight
// into the existing PluginHost / MultiChannelPluginHost slot machinery and is
// instantiated / serialized / restored through the same AudioPluginFormatManager
// pipeline as scanned AUs -- see InternalPluginFormat.  Parameters live in an
// APVTS so we get free state (getStateInformation) and a free
// GenericAudioProcessorEditor UI.
//
// Channel-agnostic: the per-channel mono host feeds 1-2 channels; an output/
// input group host feeds N.  prepareToPlay re-sizes the per-channel DSP state
// to whatever channel count it's handed.
// ============================================================================

namespace dcr::builtin
{

// Stable identifiers persisted in snapshots (PluginDescription::fileOrIdentifier).
namespace ids
{
    static constexpr const char* gain       = "builtin:gain";
    static constexpr const char* filter     = "builtin:filter";
    static constexpr const char* eq         = "builtin:eq";
    static constexpr const char* compressor = "builtin:compressor";
    static constexpr const char* gate       = "builtin:gate";
    static constexpr const char* limiter    = "builtin:limiter";
    static constexpr const char* reverb     = "builtin:reverb";
    static constexpr const char* delay      = "builtin:delay";
    static constexpr const char* tone       = "builtin:tone";
}

// ---------------------------------------------------------------------------
// MultiBiquad: one IIR biquad replicated across N channels, all sharing a
// single coefficients object (read-only during processing, so sharing is safe).
// ---------------------------------------------------------------------------
class MultiBiquad
{
public:
    void prepare (double sr, int numChannels)
    {
        sampleRate = sr;
        filters.clear();
        for (int i = 0; i < numChannels; ++i)
        {
            auto f = std::make_unique<juce::dsp::IIR::Filter<float>>();
            juce::dsp::ProcessSpec spec { sr, (juce::uint32) 8192, 1 };
            f->prepare (spec);
            filters.push_back (std::move (f));
        }
    }

    void reset() { for (auto& f : filters) f->reset(); }

    void setCoefficients (juce::dsp::IIR::Coefficients<float>::Ptr c)
    {
        coeffs = c;
        for (auto& f : filters) f->coefficients = c;
    }

    bool hasCoefficients() const noexcept { return coeffs != nullptr; }

    void process (juce::AudioBuffer<float>& buf, int numSamples)
    {
        const int nch = juce::jmin (buf.getNumChannels(), (int) filters.size());
        for (int ch = 0; ch < nch; ++ch)
        {
            auto* d = buf.getWritePointer (ch);
            auto& f = *filters[(size_t) ch];
            for (int i = 0; i < numSamples; ++i)
                d[i] = f.processSample (d[i]);
        }
    }

    double sampleRate = 48000.0;

private:
    std::vector<std::unique_ptr<juce::dsp::IIR::Filter<float>>> filters;
    juce::dsp::IIR::Coefficients<float>::Ptr coeffs;
};

// ---------------------------------------------------------------------------
// Base class -- handles all the AudioPluginInstance boilerplate + APVTS.
// ---------------------------------------------------------------------------
class BuiltinProcessor : public juce::AudioPluginInstance
{
public:
    using APVTS = juce::AudioProcessorValueTreeState;

    BuiltinProcessor (juce::String identifier, juce::String displayName,
                      APVTS::ParameterLayout layout)
        : juce::AudioPluginInstance (BusesProperties()
              .withInput  ("In",  juce::AudioChannelSet::stereo(), true)
              .withOutput ("Out", juce::AudioChannelSet::stereo(), true)),
          id (std::move (identifier)),
          dispName (std::move (displayName)),
          apvts (*this, nullptr, "PARAMS", std::move (layout))
    {
    }

    // ----- channel layout: accept any matching, non-zero in==out -----------
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override
    {
        const auto in  = layouts.getMainInputChannels();
        const auto out = layouts.getMainOutputChannels();
        return in > 0 && in == out;
    }

    // ----- prepare / process ------------------------------------------------
    void prepareToPlay (double sr, int blockSize) override
    {
        preparedChannels = juce::jmax (2, getTotalNumInputChannels(),
                                          getTotalNumOutputChannels());
        prepareDsp (sr, blockSize, preparedChannels);
    }

    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals noDenormals;
        processDsp (buffer);
    }

    using juce::AudioProcessor::processBlock;

    // ----- editor: generic auto-UI from the APVTS --------------------------
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override
    {
        return new juce::GenericAudioProcessorEditor (*this);
    }

    // ----- trivial metadata -------------------------------------------------
    const juce::String getName() const override { return dispName; }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi()  const override { return false; }
    bool producesMidi() const override { return false; }
    int  getNumPrograms() override { return 1; }
    int  getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    // ----- state: APVTS XML -------------------------------------------------
    void getStateInformation (juce::MemoryBlock& dest) override
    {
        if (auto xml = apvts.copyState().createXml())
            copyXmlToBinary (*xml, dest);
    }
    void setStateInformation (const void* data, int size) override
    {
        if (auto xml = getXmlFromBinary (data, size))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    // ----- AudioPluginInstance ---------------------------------------------
    void fillInPluginDescription (juce::PluginDescription& d) const override
    {
        d.name              = dispName;
        d.descriptiveName   = dispName;
        d.pluginFormatName  = "Internal";
        d.category          = "Built-in";
        d.manufacturerName  = "ZDAudio";
        d.version           = "1.0";
        d.fileOrIdentifier  = id;
        d.lastFileModTime   = {};
        d.uniqueId          = (int) id.hashCode();
        d.deprecatedUid     = (int) id.hashCode();
        d.isInstrument      = false;
        d.numInputChannels  = 2;
        d.numOutputChannels = 2;
    }

protected:
    // Subclasses implement the DSP.
    virtual void prepareDsp (double sr, int blockSize, int numChannels) = 0;
    virtual void processDsp (juce::AudioBuffer<float>& buffer) = 0;

    float param (juce::StringRef pid) const noexcept
    {
        if (auto* a = apvts.getRawParameterValue (pid)) return a->load();
        return 0.0f;
    }

    juce::String id, dispName;
    APVTS apvts;
    int   preparedChannels = 2;
    double dspSampleRate = 48000.0;
};

// ===========================================================================
// 1. Gain / Utility -- gain (dB), polarity invert, mute.
// ===========================================================================
class GainUtility : public BuiltinProcessor
{
public:
    GainUtility() : BuiltinProcessor (ids::gain, "Gain / Utility", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "gain", 1 }, "Gain",
            juce::NormalisableRange<float> (-60.0f, 24.0f, 0.1f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { "invert", 1 }, "Invert Phase", false));
        l.add (std::make_unique<juce::AudioParameterBool>(
            juce::ParameterID { "mute", 1 }, "Mute", false));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int) override
    {
        dspSampleRate = sr;
        smoothed.reset (sr, 0.02);
        smoothed.setCurrentAndTargetValue (currentTargetGain());
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        smoothed.setTargetValue (currentTargetGain());
        const int ns = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();
        for (int i = 0; i < ns; ++i)
        {
            const float g = smoothed.getNextValue();
            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }

private:
    float currentTargetGain() const noexcept
    {
        if (param ("mute") > 0.5f) return 0.0f;
        const float db = param ("gain");
        float lin = db <= -60.0f ? 0.0f : std::pow (10.0f, db * 0.05f);
        if (param ("invert") > 0.5f) lin = -lin;
        return lin;
    }
    juce::SmoothedValue<float> smoothed;
};

// ===========================================================================
// 2. Filter -- high-pass or low-pass, Butterworth, 12/24/36/48 dB/oct.
// ===========================================================================
class FilterProcessor : public BuiltinProcessor
{
public:
    FilterProcessor() : BuiltinProcessor (ids::filter, "Filter (HP / LP)", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "type", 1 }, "Type",
            juce::StringArray { "High-pass", "Low-pass" }, 0));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "freq", 1 }, "Frequency",
            juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.25f), 100.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));
        l.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "slope", 1 }, "Slope",
            juce::StringArray { "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct" }, 1));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int numChannels) override
    {
        dspSampleRate = sr;
        chans = numChannels;
        for (auto& s : stages) s.prepare (sr, numChannels);
        lastType = -1; lastFreq = -1.0f; lastSlope = -1;   // force recompute
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        updateCoeffsIfNeeded();
        const int ns = buffer.getNumSamples();
        for (int s = 0; s < activeStages; ++s)
            stages[(size_t) s].process (buffer, ns);
    }

private:
    void updateCoeffsIfNeeded()
    {
        const int   type  = (int) param ("type");
        const float freq  = juce::jlimit (20.0f, 20000.0f, param ("freq"));
        const int   slope = (int) param ("slope");
        if (type == lastType && std::abs (freq - lastFreq) < 0.5f && slope == lastSlope)
            return;
        lastType = type; lastFreq = freq; lastSlope = slope;

        const int order = (slope + 1) * 2;     // 0->2, 1->4, 2->6, 3->8
        auto coeffsArray = (type == 0)
            ? juce::dsp::FilterDesign<float>::designIIRHighpassHighOrderButterworthMethod
                  (freq, dspSampleRate, order)
            : juce::dsp::FilterDesign<float>::designIIRLowpassHighOrderButterworthMethod
                  (freq, dspSampleRate, order);

        activeStages = juce::jmin ((int) coeffsArray.size(), (int) stages.size());
        for (int i = 0; i < activeStages; ++i)
            stages[(size_t) i].setCoefficients (coeffsArray[i]);
    }

    static constexpr int kMaxStages = 4;       // 48 dB/oct = 4 biquads
    std::array<MultiBiquad, kMaxStages> stages;
    int  activeStages = 2;
    int  chans = 2;
    int  lastType = -1, lastSlope = -1;
    float lastFreq = -1.0f;
};

// ===========================================================================
// 3. Parametric EQ -- 5 bands (bell / shelf / pass), each independently on.
// ===========================================================================
class ParametricEQ : public BuiltinProcessor
{
public:
    static constexpr int kNumBands = 5;

    ParametricEQ() : BuiltinProcessor (ids::eq, "Parametric EQ", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        const float defFreqs[kNumBands] = { 80.0f, 250.0f, 1000.0f, 4000.0f, 12000.0f };
        for (int b = 0; b < kNumBands; ++b)
        {
            const juce::String p = "b" + juce::String (b) + "_";
            const juce::String label = "Band " + juce::String (b + 1) + " ";
            l.add (std::make_unique<juce::AudioParameterBool>(
                juce::ParameterID { p + "on", 1 }, label + "On", b == 2));
            l.add (std::make_unique<juce::AudioParameterChoice>(
                juce::ParameterID { p + "type", 1 }, label + "Type",
                juce::StringArray { "Bell", "Low Shelf", "High Shelf", "High-pass", "Low-pass" },
                b == 0 ? 1 : (b == kNumBands - 1 ? 2 : 0)));
            l.add (std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID { p + "freq", 1 }, label + "Freq",
                juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.25f), defFreqs[b],
                juce::AudioParameterFloatAttributes().withLabel ("Hz")));
            l.add (std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID { p + "gain", 1 }, label + "Gain",
                juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f,
                juce::AudioParameterFloatAttributes().withLabel ("dB")));
            l.add (std::make_unique<juce::AudioParameterFloat>(
                juce::ParameterID { p + "q", 1 }, label + "Q",
                juce::NormalisableRange<float> (0.1f, 10.0f, 0.01f, 0.3f), 0.707f));
        }
        return l;
    }

protected:
    void prepareDsp (double sr, int, int numChannels) override
    {
        dspSampleRate = sr;
        for (auto& b : bands) b.prepare (sr, numChannels);
        for (auto& c : cache) c = {};
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns = buffer.getNumSamples();
        for (int b = 0; b < kNumBands; ++b)
        {
            if (param (pid (b, "on")) < 0.5f) continue;
            updateBandIfNeeded (b);
            if (bands[(size_t) b].hasCoefficients())
                bands[(size_t) b].process (buffer, ns);
        }
    }

private:
    juce::String pid (int b, const char* suffix) const
    {
        return "b" + juce::String (b) + "_" + suffix;
    }

    struct BandCache { int type = -1; float freq = -1, gain = -999, q = -1; };

    void updateBandIfNeeded (int b)
    {
        const int   type = (int) param (pid (b, "type").toRawUTF8());
        const float freq = juce::jlimit (20.0f, 20000.0f, param (pid (b, "freq").toRawUTF8()));
        const float gain = param (pid (b, "gain").toRawUTF8());
        const float q    = juce::jlimit (0.1f, 10.0f, param (pid (b, "q").toRawUTF8()));

        auto& c = cache[(size_t) b];
        if (type == c.type && std::abs (freq - c.freq) < 0.5f
            && std::abs (gain - c.gain) < 0.01f && std::abs (q - c.q) < 0.001f)
            return;
        c = { type, freq, gain, q };

        const float gainLin = std::pow (10.0f, gain * 0.05f);
        juce::dsp::IIR::Coefficients<float>::Ptr co;
        switch (type)
        {
            case 1: co = juce::dsp::IIR::Coefficients<float>::makeLowShelf  (dspSampleRate, freq, q, gainLin); break;
            case 2: co = juce::dsp::IIR::Coefficients<float>::makeHighShelf (dspSampleRate, freq, q, gainLin); break;
            case 3: co = juce::dsp::IIR::Coefficients<float>::makeHighPass  (dspSampleRate, freq, q); break;
            case 4: co = juce::dsp::IIR::Coefficients<float>::makeLowPass   (dspSampleRate, freq, q); break;
            default: co = juce::dsp::IIR::Coefficients<float>::makePeakFilter (dspSampleRate, freq, q, gainLin); break;
        }
        bands[(size_t) b].setCoefficients (co);
    }

    std::array<MultiBiquad, kNumBands> bands;
    std::array<BandCache,   kNumBands> cache;
};

// ===========================================================================
// 4. Compressor -- channel-linked, soft-knee, with makeup gain.
// ===========================================================================
class CompressorProcessor : public BuiltinProcessor
{
public:
    CompressorProcessor() : BuiltinProcessor (ids::compressor, "Compressor", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "threshold", 1 }, "Threshold",
            juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -18.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "ratio", 1 }, "Ratio",
            juce::NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.4f), 3.0f, juce::AudioParameterFloatAttributes().withLabel (": 1")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "attack", 1 }, "Attack",
            juce::NormalisableRange<float> (0.1f, 200.0f, 0.1f, 0.4f), 10.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "release", 1 }, "Release",
            juce::NormalisableRange<float> (5.0f, 1000.0f, 1.0f, 0.4f), 120.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "knee", 1 }, "Knee",
            juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), 6.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "makeup", 1 }, "Makeup",
            juce::NormalisableRange<float> (-12.0f, 24.0f, 0.1f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int) override
    {
        dspSampleRate = sr;
        envelope = 0.0f;
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();

        const float threshold = param ("threshold");
        const float ratio     = juce::jmax (1.0f, param ("ratio"));
        const float knee      = juce::jmax (0.0f, param ("knee"));
        const float makeupLin = std::pow (10.0f, param ("makeup") * 0.05f);

        const float attMs = juce::jmax (0.1f, param ("attack"));
        const float relMs = juce::jmax (1.0f, param ("release"));
        const float attCoeff = std::exp (-1.0f / (float) (attMs * 0.001 * dspSampleRate));
        const float relCoeff = std::exp (-1.0f / (float) (relMs * 0.001 * dspSampleRate));

        for (int i = 0; i < ns; ++i)
        {
            // Channel-linked detector: peak across all channels this sample.
            float peak = 0.0f;
            for (int ch = 0; ch < nch; ++ch)
                peak = juce::jmax (peak, std::abs (buffer.getReadPointer (ch)[i]));

            // Envelope follower (attack when rising, release when falling).
            const float coeff = (peak > envelope) ? attCoeff : relCoeff;
            envelope = coeff * envelope + (1.0f - coeff) * peak;

            const float levelDb = juce::Decibels::gainToDecibels (envelope, -100.0f);

            // Soft-knee static gain computer.
            float overDb = levelDb - threshold;
            float grDb = 0.0f;                    // gain reduction (<= 0)
            if (knee > 0.0f && overDb > -knee * 0.5f && overDb < knee * 0.5f)
            {
                const float x = overDb + knee * 0.5f;
                grDb = -((1.0f - 1.0f / ratio) * (x * x) / (2.0f * knee));
            }
            else if (overDb >= knee * 0.5f)
            {
                grDb = -((overDb) * (1.0f - 1.0f / ratio));
            }

            const float gain = std::pow (10.0f, grDb * 0.05f) * makeupLin;
            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] *= gain;
        }
    }

private:
    float envelope = 0.0f;
};

// ===========================================================================
// 5. Noise Gate -- channel-linked, attack / hold / release, attenuation range.
// ===========================================================================
class NoiseGateProcessor : public BuiltinProcessor
{
public:
    NoiseGateProcessor() : BuiltinProcessor (ids::gate, "Noise Gate", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "threshold", 1 }, "Threshold",
            juce::NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -40.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "attack", 1 }, "Attack",
            juce::NormalisableRange<float> (0.1f, 100.0f, 0.1f, 0.4f), 1.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "hold", 1 }, "Hold",
            juce::NormalisableRange<float> (0.0f, 500.0f, 1.0f, 0.5f), 50.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "release", 1 }, "Release",
            juce::NormalisableRange<float> (5.0f, 2000.0f, 1.0f, 0.4f), 200.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "range", 1 }, "Range",
            juce::NormalisableRange<float> (-100.0f, 0.0f, 0.1f), -80.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int) override
    {
        dspSampleRate = sr;
        gain = 0.0f;
        holdCounter = 0;
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();

        const float threshLin = std::pow (10.0f, param ("threshold") * 0.05f);
        const float rangeLin  = std::pow (10.0f, param ("range") * 0.05f);
        const float attCoeff  = std::exp (-1.0f / (float) (juce::jmax (0.1f, param ("attack"))  * 0.001 * dspSampleRate));
        const float relCoeff  = std::exp (-1.0f / (float) (juce::jmax (1.0f, param ("release")) * 0.001 * dspSampleRate));
        const int   holdSamps = (int) (juce::jmax (0.0f, param ("hold")) * 0.001f * (float) dspSampleRate);

        for (int i = 0; i < ns; ++i)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < nch; ++ch)
                peak = juce::jmax (peak, std::abs (buffer.getReadPointer (ch)[i]));

            float target;
            if (peak >= threshLin)      { target = 1.0f; holdCounter = holdSamps; }
            else if (holdCounter > 0)   { target = 1.0f; --holdCounter; }
            else                          target = rangeLin;

            const float coeff = (target > gain) ? attCoeff : relCoeff;
            gain = coeff * gain + (1.0f - coeff) * target;

            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] *= gain;
        }
    }

private:
    float gain = 0.0f;
    int   holdCounter = 0;
};

// ===========================================================================
// 6. Limiter -- channel-linked brickwall, instant attack, smooth release.
// ===========================================================================
class LimiterProcessor : public BuiltinProcessor
{
public:
    LimiterProcessor() : BuiltinProcessor (ids::limiter, "Limiter", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "ceiling", 1 }, "Ceiling",
            juce::NormalisableRange<float> (-24.0f, 0.0f, 0.1f), -0.3f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "release", 1 }, "Release",
            juce::NormalisableRange<float> (1.0f, 1000.0f, 1.0f, 0.4f), 100.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "gain", 1 }, "Input Gain",
            juce::NormalisableRange<float> (0.0f, 24.0f, 0.1f), 0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int) override { dspSampleRate = sr; gain = 1.0f; }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();

        const float ceilLin  = std::pow (10.0f, param ("ceiling") * 0.05f);
        const float inLin     = std::pow (10.0f, param ("gain") * 0.05f);
        const float relCoeff = std::exp (-1.0f / (float) (juce::jmax (1.0f, param ("release")) * 0.001 * dspSampleRate));

        for (int i = 0; i < ns; ++i)
        {
            // Apply input gain first, then find the linked peak.
            float peak = 0.0f;
            for (int ch = 0; ch < nch; ++ch)
            {
                buffer.getWritePointer (ch)[i] *= inLin;
                peak = juce::jmax (peak, std::abs (buffer.getReadPointer (ch)[i]));
            }

            const float desired = peak > ceilLin ? (ceilLin / peak) : 1.0f;
            if (desired < gain) gain = desired;                     // instant attack
            else                gain = relCoeff * gain + (1.0f - relCoeff) * 1.0f;

            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] *= gain;
        }
    }

private:
    float gain = 1.0f;
};

// ===========================================================================
// 7. Reverb -- Freeverb (juce::Reverb), one mono tank per channel.
// ===========================================================================
class ReverbProcessor : public BuiltinProcessor
{
public:
    ReverbProcessor() : BuiltinProcessor (ids::reverb, "Reverb", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "size", 1 }, "Room Size",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.5f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "damping", 1 }, "Damping",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.5f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "wet", 1 }, "Wet",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.33f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "dry", 1 }, "Dry",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 1.0f));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int numChannels) override
    {
        reverbs.clear();
        for (int i = 0; i < numChannels; ++i)
        {
            auto r = std::make_unique<juce::Reverb>();
            r->setSampleRate (sr);
            r->reset();
            reverbs.push_back (std::move (r));
        }
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        juce::Reverb::Parameters p;
        p.roomSize   = param ("size");
        p.damping    = param ("damping");
        p.wetLevel   = param ("wet");
        p.dryLevel   = param ("dry");
        p.width      = 1.0f;
        p.freezeMode = 0.0f;

        const int ns  = buffer.getNumSamples();
        const int nch = juce::jmin (buffer.getNumChannels(), (int) reverbs.size());
        for (int ch = 0; ch < nch; ++ch)
        {
            reverbs[(size_t) ch]->setParameters (p);
            reverbs[(size_t) ch]->processMono (buffer.getWritePointer (ch), ns);
        }
    }

private:
    std::vector<std::unique_ptr<juce::Reverb>> reverbs;
};

// ===========================================================================
// 8. Delay -- per-channel feedback delay with wet/dry mix.
// ===========================================================================
class DelayProcessor : public BuiltinProcessor
{
public:
    DelayProcessor() : BuiltinProcessor (ids::delay, "Delay", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "time", 1 }, "Time",
            juce::NormalisableRange<float> (1.0f, 2000.0f, 1.0f, 0.4f), 300.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "feedback", 1 }, "Feedback",
            juce::NormalisableRange<float> (0.0f, 0.95f, 0.01f), 0.35f));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "mix", 1 }, "Mix",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.3f));
        return l;
    }

protected:
    void prepareDsp (double sr, int blockSize, int numChannels) override
    {
        dspSampleRate = sr;
        maxDelaySamps = (int) (2.0 * sr) + 8;
        line.setMaximumDelayInSamples (maxDelaySamps);
        line.prepare ({ sr, (juce::uint32) juce::jmax (1, blockSize),
                        (juce::uint32) juce::jmax (1, numChannels) });
        line.reset();
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();

        const float timeSamps = juce::jlimit (1.0f, (float) (maxDelaySamps - 1),
                                              param ("time") * 0.001f * (float) dspSampleRate);
        const float fb  = juce::jlimit (0.0f, 0.95f, param ("feedback"));
        const float mix = juce::jlimit (0.0f, 1.0f,  param ("mix"));
        line.setDelay (timeSamps);

        for (int i = 0; i < ns; ++i)
            for (int ch = 0; ch < nch; ++ch)
            {
                const float in = buffer.getReadPointer (ch)[i];
                const float d  = line.popSample (ch);
                line.pushSample (ch, in + d * fb);
                buffer.getWritePointer (ch)[i] = in * (1.0f - mix) + d * mix;
            }
    }

private:
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> line { 96008 };
    int maxDelaySamps = 96000;
};

// ===========================================================================
// 9. Tone Generator -- sine / white / pink test signal (replaces the input).
//    Handy in a router for verifying a signal path end-to-end.
// ===========================================================================
class ToneProcessor : public BuiltinProcessor
{
public:
    ToneProcessor() : BuiltinProcessor (ids::tone, "Tone Generator", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "mode", 1 }, "Mode",
            juce::StringArray { "Off (pass-through)", "Sine", "White Noise", "Pink Noise" }, 0));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "freq", 1 }, "Frequency",
            juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.25f), 1000.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "level", 1 }, "Level",
            juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -12.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int) override { dspSampleRate = sr; phase = 0.0; pink = 0.0f; }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int mode = (int) param ("mode");
        if (mode == 0) return;                       // pass-through

        const int ns  = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();
        const float lvl = std::pow (10.0f, param ("level") * 0.05f);
        const double inc = juce::jlimit (20.0f, 20000.0f, param ("freq")) / dspSampleRate;

        for (int i = 0; i < ns; ++i)
        {
            float s = 0.0f;
            if (mode == 1)                            // sine
            {
                s = std::sin (phase * juce::MathConstants<double>::twoPi);
                phase += inc;
                if (phase >= 1.0) phase -= 1.0;
            }
            else if (mode == 2)                       // white
            {
                s = rng.nextFloat() * 2.0f - 1.0f;
            }
            else                                      // pink (one-pole coloured approx)
            {
                const float w = rng.nextFloat() * 2.0f - 1.0f;
                pink = 0.98f * pink + 0.02f * w;
                s = pink * 3.0f;
            }
            s *= lvl;
            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] = s;   // replace -- it's a source
        }
    }

private:
    double      phase = 0.0;
    float       pink  = 0.0f;
    juce::Random rng;
};

} // namespace dcr::builtin

#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
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
    static constexpr const char* tremolo    = "builtin:tremolo";
    static constexpr const char* width      = "builtin:width";
    static constexpr const char* deesser    = "builtin:deesser";
    static constexpr const char* strip      = "builtin:strip";
    static constexpr const char* mbcomp     = "builtin:mbcomp";
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

// Shared EQ-band coefficient builder (type: 0 bell, 1 low-shelf, 2 high-shelf,
// 3 high-pass, 4 low-pass).  Used by ParametricEQ, ChannelStrip and their
// editors so the response curves match the DSP exactly.
inline juce::dsp::IIR::Coefficients<float>::Ptr makeEqBandCoeffs (int type, double sr,
                                                                  float freq, float q, float gainDb)
{
    const float g = std::pow (10.0f, gainDb * 0.05f);
    switch (type)
    {
        case 1:  return juce::dsp::IIR::Coefficients<float>::makeLowShelf  (sr, freq, q, g);
        case 2:  return juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, freq, q, g);
        case 3:  return juce::dsp::IIR::Coefficients<float>::makeHighPass  (sr, freq, q);
        case 4:  return juce::dsp::IIR::Coefficients<float>::makeLowPass   (sr, freq, q);
        default: return juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, freq, q, g);
    }
}

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
        // Register the rate/size so getSampleRate()/getBlockSize() report the
        // truth.  The DSP uses the `sr` argument directly, but the editors'
        // response curves call getSampleRate() -- without this it returns 0,
        // the curve falls back to an 8 kHz assumption, and every band's
        // response is drawn aliased above 4 kHz (spurious high-freq notches).
        setRateAndBufferSizeDetails (sr, blockSize);
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

    // Exposed so custom editors can read/write parameters.
    APVTS& getValueTreeState() noexcept { return apvts; }

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

    // Custom editor with a draggable frequency-response curve (defined in
    // ParametricEqEditor.h; createEditor is implemented in
    // InternalPluginFormat.cpp to avoid a header cycle).
    juce::AudioProcessorEditor* createEditor() override;

    // Parameter id scheme -- shared by the processor and the editor.
    static juce::String paramId (int band, juce::StringRef suffix)
    {
        return "b" + juce::String (band) + "_" + suffix;
    }

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
    juce::String pid (int b, const char* suffix) const { return paramId (b, suffix); }

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

    // Custom editor: transfer curve + gain-reduction meter (defined in
    // CompressorEditor.h; createEditor implemented in InternalPluginFormat.cpp).
    juce::AudioProcessorEditor* createEditor() override;

    // Live gain reduction in dB (<= 0), for the GR meter.  Peak over the
    // last processed block.
    float getGainReductionDb() const noexcept { return grReadout.load (std::memory_order_relaxed); }

protected:
    void prepareDsp (double sr, int, int) override
    {
        dspSampleRate = sr;
        envelope = 0.0f;
        grReadout.store (0.0f, std::memory_order_relaxed);
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

        float blockMinGr = 0.0f;   // most-negative gain reduction this block

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
            blockMinGr = juce::jmin (blockMinGr, grDb);

            const float gain = std::pow (10.0f, grDb * 0.05f) * makeupLin;
            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] *= gain;
        }

        grReadout.store (blockMinGr, std::memory_order_relaxed);
    }

private:
    float envelope = 0.0f;
    std::atomic<float> grReadout { 0.0f };
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

// ===========================================================================
// 10. Tremolo -- amplitude LFO (sine / triangle / square).
// ===========================================================================
class TremoloProcessor : public BuiltinProcessor
{
public:
    TremoloProcessor() : BuiltinProcessor (ids::tremolo, "Tremolo", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "rate", 1 }, "Rate",
            juce::NormalisableRange<float> (0.1f, 20.0f, 0.01f, 0.4f), 5.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "depth", 1 }, "Depth",
            juce::NormalisableRange<float> (0.0f, 1.0f, 0.01f), 0.5f));
        l.add (std::make_unique<juce::AudioParameterChoice>(
            juce::ParameterID { "shape", 1 }, "Shape",
            juce::StringArray { "Sine", "Triangle", "Square" }, 0));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int) override { dspSampleRate = sr; phase = 0.0; }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();
        const float depth = juce::jlimit (0.0f, 1.0f, param ("depth"));
        const int   shape = (int) param ("shape");
        const double inc  = juce::jlimit (0.1f, 20.0f, param ("rate")) / dspSampleRate;

        for (int i = 0; i < ns; ++i)
        {
            float lfo;                          // -1 .. 1
            if (shape == 1)      lfo = 4.0f * std::abs ((float) phase - 0.5f) - 1.0f;   // triangle
            else if (shape == 2) lfo = (phase < 0.5 ? 1.0f : -1.0f);                    // square
            else                 lfo = std::sin ((float) phase * juce::MathConstants<float>::twoPi);

            phase += inc; if (phase >= 1.0) phase -= 1.0;

            const float gain = 1.0f - depth * (0.5f - 0.5f * lfo);   // [1-depth .. 1]
            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] *= gain;
        }
    }

private:
    double phase = 0.0;
};

// ===========================================================================
// 11. Stereo Width -- M/S widener on channel pairs (no-op on mono).
// ===========================================================================
class StereoWidthProcessor : public BuiltinProcessor
{
public:
    StereoWidthProcessor() : BuiltinProcessor (ids::width, "Stereo Width", createLayout()) {}

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "width", 1 }, "Width",
            juce::NormalisableRange<float> (0.0f, 2.0f, 0.01f), 1.0f));
        return l;
    }

protected:
    void prepareDsp (double, int, int) override {}

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const float w   = juce::jlimit (0.0f, 2.0f, param ("width"));
        const int   ns  = buffer.getNumSamples();
        const int   nch = buffer.getNumChannels();
        // Process consecutive L/R pairs; an odd trailing channel is left alone.
        for (int base = 0; base + 1 < nch; base += 2)
        {
            auto* L = buffer.getWritePointer (base);
            auto* R = buffer.getWritePointer (base + 1);
            for (int i = 0; i < ns; ++i)
            {
                const float mid  = 0.5f * (L[i] + R[i]);
                const float side = 0.5f * (L[i] - R[i]) * w;
                L[i] = mid + side;
                R[i] = mid - side;
            }
        }
    }
};

// ===========================================================================
// 12. De-esser -- splits a high band, ducks it when sibilance exceeds the
//     threshold (channel-linked detector, clamped to a max reduction range).
// ===========================================================================
class DeEsserProcessor : public BuiltinProcessor
{
public:
    DeEsserProcessor() : BuiltinProcessor (ids::deesser, "De-esser", createLayout()) {}

    // Custom editor (DeEsserEditor.h) -- band level + GR meter.
    juce::AudioProcessorEditor* createEditor() override;

    // Live readouts for the editor (peak over the last block).
    float getGainReductionDb() const noexcept { return grReadout .load (std::memory_order_relaxed); }
    float getBandLevelDb()     const noexcept { return bandLevel .load (std::memory_order_relaxed); }

    static APVTS::ParameterLayout createLayout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "freq", 1 }, "Frequency",
            juce::NormalisableRange<float> (2000.0f, 16000.0f, 1.0f, 0.5f), 6000.0f,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "threshold", 1 }, "Threshold",
            juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -30.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "range", 1 }, "Range",
            juce::NormalisableRange<float> (-24.0f, 0.0f, 0.1f), -12.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "attack", 1 }, "Attack",
            juce::NormalisableRange<float> (0.1f, 20.0f, 0.1f, 0.5f), 2.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        l.add (std::make_unique<juce::AudioParameterFloat>(
            juce::ParameterID { "release", 1 }, "Release",
            juce::NormalisableRange<float> (5.0f, 200.0f, 1.0f, 0.5f), 60.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));
        return l;
    }

protected:
    void prepareDsp (double sr, int, int numChannels) override
    {
        dspSampleRate = sr;
        hp.clear();
        for (int i = 0; i < numChannels; ++i)
        {
            auto f = std::make_unique<juce::dsp::IIR::Filter<float>>();
            f->prepare ({ sr, (juce::uint32) 8192, 1 });
            hp.push_back (std::move (f));
        }
        highScratch.assign ((size_t) numChannels, 0.0f);
        lowScratch .assign ((size_t) numChannels, 0.0f);
        env = 0.0f; grGain = 1.0f; lastFreq = -1.0f;
        grReadout.store (0.0f, std::memory_order_relaxed);
        bandLevel.store (-100.0f, std::memory_order_relaxed);
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = juce::jmin (buffer.getNumChannels(), (int) hp.size());
        if (nch == 0) return;

        const float freq = juce::jlimit (2000.0f, 16000.0f, param ("freq"));
        if (std::abs (freq - lastFreq) > 0.5f)
        {
            lastFreq = freq;
            auto co = juce::dsp::IIR::Coefficients<float>::makeHighPass (dspSampleRate, freq, 0.707f);
            for (auto& f : hp) f->coefficients = co;
        }

        const float threshold = param ("threshold");
        const float rangeDb   = juce::jmin (0.0f, param ("range"));
        const float ratio     = 4.0f;   // fixed, strong band ratio
        const float attCoeff  = std::exp (-1.0f / (float) (juce::jmax (0.1f, param ("attack"))  * 0.001 * dspSampleRate));
        const float relCoeff  = std::exp (-1.0f / (float) (juce::jmax (1.0f, param ("release")) * 0.001 * dspSampleRate));

        float blockMaxLevel = -100.0f, blockMinGr = 0.0f;

        for (int i = 0; i < ns; ++i)
        {
            float peak = 0.0f;
            for (int ch = 0; ch < nch; ++ch)
            {
                const float x = buffer.getReadPointer (ch)[i];
                const float h = hp[(size_t) ch]->processSample (x);
                highScratch[(size_t) ch] = h;
                lowScratch [(size_t) ch] = x - h;
                peak = juce::jmax (peak, std::abs (h));
            }

            const float coeff = (peak > env) ? attCoeff : relCoeff;
            env = coeff * env + (1.0f - coeff) * peak;

            const float levelDb = juce::Decibels::gainToDecibels (env, -100.0f);
            blockMaxLevel = juce::jmax (blockMaxLevel, levelDb);
            const float over    = levelDb - threshold;
            float grDb = over > 0.0f ? -(over * (1.0f - 1.0f / ratio)) : 0.0f;
            grDb = juce::jmax (grDb, rangeDb);                    // clamp reduction
            const float targetGain = std::pow (10.0f, grDb * 0.05f);

            const float gc = (targetGain < grGain) ? attCoeff : relCoeff;
            grGain = gc * grGain + (1.0f - gc) * targetGain;
            blockMinGr = juce::jmin (blockMinGr, juce::Decibels::gainToDecibels (grGain, -48.0f));

            for (int ch = 0; ch < nch; ++ch)
                buffer.getWritePointer (ch)[i] = lowScratch[(size_t) ch]
                                               + highScratch[(size_t) ch] * grGain;
        }

        bandLevel.store (blockMaxLevel, std::memory_order_relaxed);
        grReadout.store (blockMinGr,    std::memory_order_relaxed);
    }

private:
    std::vector<std::unique_ptr<juce::dsp::IIR::Filter<float>>> hp;
    std::vector<float> highScratch, lowScratch;
    float env = 0.0f, grGain = 1.0f, lastFreq = -1.0f;
    std::atomic<float> grReadout { 0.0f };
    std::atomic<float> bandLevel { -100.0f };
};

// ===========================================================================
// 13. Channel Strip -- console-style all-in-one: HP filter -> Gate -> 4-band
//     EQ -> Compressor -> Output gain, in one plugin with one editor.
// ===========================================================================
class ChannelStripProcessor : public BuiltinProcessor
{
public:
    static constexpr int kEqBands = 4;

    ChannelStripProcessor() : BuiltinProcessor (ids::strip, "Channel Strip", createLayout()) {}

    juce::AudioProcessorEditor* createEditor() override;   // ChannelStripEditor.h

    float getCompGainReductionDb() const noexcept { return compGr.load (std::memory_order_relaxed); }

    static juce::String eqId (int b, juce::StringRef s) { return "eq" + juce::String (b) + "_" + s; }

    static APVTS::ParameterLayout createLayout()
    {
        using BoolP   = juce::AudioParameterBool;
        using FloatP  = juce::AudioParameterFloat;
        using ChoiceP = juce::AudioParameterChoice;
        using Group   = juce::AudioProcessorParameterGroup;
        auto fa = [] (const char* unit) { return juce::AudioParameterFloatAttributes().withLabel (unit); };

        APVTS::ParameterLayout l;

        l.add (std::make_unique<Group> ("hp", "1  HP Filter", "|",
            std::make_unique<BoolP> (juce::ParameterID { "hp_on", 1 }, "HP On", false),
            std::make_unique<FloatP> (juce::ParameterID { "hp_freq", 1 }, "HP Freq",
                juce::NormalisableRange<float> (20.0f, 1000.0f, 1.0f, 0.3f), 80.0f, fa ("Hz"))));

        l.add (std::make_unique<Group> ("gate", "2  Gate", "|",
            std::make_unique<BoolP> (juce::ParameterID { "g_on", 1 }, "Gate On", false),
            std::make_unique<FloatP> (juce::ParameterID { "g_thresh", 1 }, "Gate Threshold",
                juce::NormalisableRange<float> (-80.0f, 0.0f, 0.1f), -40.0f, fa ("dB")),
            std::make_unique<FloatP> (juce::ParameterID { "g_range", 1 }, "Gate Range",
                juce::NormalisableRange<float> (-100.0f, 0.0f, 0.1f), -60.0f, fa ("dB")),
            std::make_unique<FloatP> (juce::ParameterID { "g_rel", 1 }, "Gate Release",
                juce::NormalisableRange<float> (5.0f, 2000.0f, 1.0f, 0.4f), 150.0f, fa ("ms"))));

        // EQ group (4 bands).
        auto eqGroup = std::make_unique<Group> ("eq", "3  EQ", "|");
        const float defFreqs[kEqBands] = { 120.0f, 800.0f, 3000.0f, 9000.0f };
        for (int b = 0; b < kEqBands; ++b)
        {
            const juce::String lab = "EQ" + juce::String (b + 1) + " ";
            eqGroup->addChild (
                std::make_unique<BoolP> (juce::ParameterID { eqId (b, "on"), 1 }, lab + "On", b == 1 || b == 2),
                std::make_unique<ChoiceP> (juce::ParameterID { eqId (b, "type"), 1 }, lab + "Type",
                    juce::StringArray { "Bell", "Low Shelf", "High Shelf", "High-pass", "Low-pass" },
                    b == 0 ? 1 : (b == kEqBands - 1 ? 2 : 0)),
                std::make_unique<FloatP> (juce::ParameterID { eqId (b, "freq"), 1 }, lab + "Freq",
                    juce::NormalisableRange<float> (20.0f, 20000.0f, 1.0f, 0.25f), defFreqs[b], fa ("Hz")),
                std::make_unique<FloatP> (juce::ParameterID { eqId (b, "gain"), 1 }, lab + "Gain",
                    juce::NormalisableRange<float> (-18.0f, 18.0f, 0.1f), 0.0f, fa ("dB")),
                std::make_unique<FloatP> (juce::ParameterID { eqId (b, "q"), 1 }, lab + "Q",
                    juce::NormalisableRange<float> (0.1f, 10.0f, 0.01f, 0.3f), 0.707f));
        }
        l.add (std::move (eqGroup));

        l.add (std::make_unique<Group> ("comp", "4  Compressor", "|",
            std::make_unique<BoolP> (juce::ParameterID { "c_on", 1 }, "Comp On", false),
            std::make_unique<FloatP> (juce::ParameterID { "c_thresh", 1 }, "Comp Threshold",
                juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -18.0f, fa ("dB")),
            std::make_unique<FloatP> (juce::ParameterID { "c_ratio", 1 }, "Comp Ratio",
                juce::NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.4f), 3.0f, fa (": 1")),
            std::make_unique<FloatP> (juce::ParameterID { "c_attack", 1 }, "Comp Attack",
                juce::NormalisableRange<float> (0.1f, 200.0f, 0.1f, 0.4f), 10.0f, fa ("ms")),
            std::make_unique<FloatP> (juce::ParameterID { "c_rel", 1 }, "Comp Release",
                juce::NormalisableRange<float> (5.0f, 1000.0f, 1.0f, 0.4f), 120.0f, fa ("ms")),
            std::make_unique<FloatP> (juce::ParameterID { "c_makeup", 1 }, "Comp Makeup",
                juce::NormalisableRange<float> (-12.0f, 24.0f, 0.1f), 0.0f, fa ("dB"))));

        l.add (std::make_unique<Group> ("out", "5  Output", "|",
            std::make_unique<FloatP> (juce::ParameterID { "out_gain", 1 }, "Output Gain",
                juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, fa ("dB"))));

        return l;
    }

protected:
    void prepareDsp (double sr, int, int numChannels) override
    {
        dspSampleRate = sr;
        hpf.prepare (sr, numChannels);
        for (auto& b : eqBands) b.prepare (sr, numChannels);
        for (auto& c : eqCache) c = {};
        gateEnv = 0.0f; gateGain = 0.0f;
        compEnv = 0.0f;
        compGr.store (0.0f, std::memory_order_relaxed);
        lastHpFreq = -1.0f;
        outSmoothed.reset (sr, 0.02);
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = buffer.getNumChannels();

        // --- 1. HP filter ---------------------------------------------------
        if (param ("hp_on") > 0.5f)
        {
            const float f = juce::jlimit (20.0f, 1000.0f, param ("hp_freq"));
            if (std::abs (f - lastHpFreq) > 0.5f)
            {
                lastHpFreq = f;
                hpf.setCoefficients (juce::dsp::IIR::Coefficients<float>::makeHighPass (dspSampleRate, f, 0.707f));
            }
            if (hpf.hasCoefficients()) hpf.process (buffer, ns);
        }

        // --- 2. Gate --------------------------------------------------------
        if (param ("g_on") > 0.5f)
        {
            const float threshLin = std::pow (10.0f, param ("g_thresh") * 0.05f);
            const float rangeLin  = std::pow (10.0f, param ("g_range") * 0.05f);
            const float relCoeff  = std::exp (-1.0f / (float) (juce::jmax (1.0f, param ("g_rel")) * 0.001 * dspSampleRate));
            const float attCoeff  = std::exp (-1.0f / (float) (1.0 * 0.001 * dspSampleRate));   // 1 ms fixed
            for (int i = 0; i < ns; ++i)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < nch; ++ch) peak = juce::jmax (peak, std::abs (buffer.getReadPointer (ch)[i]));
                const float target = (peak >= threshLin) ? 1.0f : rangeLin;
                const float coeff = (target > gateGain) ? attCoeff : relCoeff;
                gateGain = coeff * gateGain + (1.0f - coeff) * target;
                for (int ch = 0; ch < nch; ++ch) buffer.getWritePointer (ch)[i] *= gateGain;
            }
        }

        // --- 3. EQ ----------------------------------------------------------
        for (int b = 0; b < kEqBands; ++b)
        {
            if (param (eqId (b, "on").toRawUTF8()) < 0.5f) continue;
            const int   type = (int) param (eqId (b, "type").toRawUTF8());
            const float freq = juce::jlimit (20.0f, 20000.0f, param (eqId (b, "freq").toRawUTF8()));
            const float gain = param (eqId (b, "gain").toRawUTF8());
            const float q    = juce::jlimit (0.1f, 10.0f, param (eqId (b, "q").toRawUTF8()));
            auto& c = eqCache[(size_t) b];
            if (type != c.type || std::abs (freq - c.freq) > 0.5f
                || std::abs (gain - c.gain) > 0.01f || std::abs (q - c.q) > 0.001f)
            {
                c = { type, freq, gain, q };
                eqBands[(size_t) b].setCoefficients (makeEqBandCoeffs (type, dspSampleRate, freq, q, gain));
            }
            if (eqBands[(size_t) b].hasCoefficients()) eqBands[(size_t) b].process (buffer, ns);
        }

        // --- 4. Compressor --------------------------------------------------
        if (param ("c_on") > 0.5f)
        {
            const float threshold = param ("c_thresh");
            const float ratio     = juce::jmax (1.0f, param ("c_ratio"));
            const float makeupLin = std::pow (10.0f, param ("c_makeup") * 0.05f);
            const float attCoeff  = std::exp (-1.0f / (float) (juce::jmax (0.1f, param ("c_attack")) * 0.001 * dspSampleRate));
            const float relCoeff  = std::exp (-1.0f / (float) (juce::jmax (1.0f, param ("c_rel"))    * 0.001 * dspSampleRate));
            float blockMinGr = 0.0f;
            for (int i = 0; i < ns; ++i)
            {
                float peak = 0.0f;
                for (int ch = 0; ch < nch; ++ch) peak = juce::jmax (peak, std::abs (buffer.getReadPointer (ch)[i]));
                const float coeff = (peak > compEnv) ? attCoeff : relCoeff;
                compEnv = coeff * compEnv + (1.0f - coeff) * peak;
                const float over = juce::Decibels::gainToDecibels (compEnv, -100.0f) - threshold;
                const float grDb = over > 0.0f ? -(over * (1.0f - 1.0f / ratio)) : 0.0f;
                blockMinGr = juce::jmin (blockMinGr, grDb);
                const float gain = std::pow (10.0f, grDb * 0.05f) * makeupLin;
                for (int ch = 0; ch < nch; ++ch) buffer.getWritePointer (ch)[i] *= gain;
            }
            compGr.store (blockMinGr, std::memory_order_relaxed);
        }
        else compGr.store (0.0f, std::memory_order_relaxed);

        // --- 5. Output gain -------------------------------------------------
        outSmoothed.setTargetValue (std::pow (10.0f, param ("out_gain") * 0.05f));
        for (int i = 0; i < ns; ++i)
        {
            const float g = outSmoothed.getNextValue();
            for (int ch = 0; ch < nch; ++ch) buffer.getWritePointer (ch)[i] *= g;
        }
    }

private:
    struct BandCache { int type = -1; float freq = -1, gain = -999, q = -1; };

    MultiBiquad hpf;
    std::array<MultiBiquad, kEqBands> eqBands;
    std::array<BandCache,   kEqBands> eqCache;
    float gateEnv = 0.0f, gateGain = 0.0f, compEnv = 0.0f, lastHpFreq = -1.0f;
    std::atomic<float> compGr { 0.0f };
    juce::SmoothedValue<float> outSmoothed;
};

// ===========================================================================
// 14. Multiband Compressor -- 4 bands split by 3 Linkwitz-Riley crossovers,
//     each band compressed independently (channel-linked), then summed.
// ===========================================================================
class MultibandCompProcessor : public BuiltinProcessor
{
public:
    static constexpr int kBands = 4;

    MultibandCompProcessor() : BuiltinProcessor (ids::mbcomp, "Multiband Compressor", createLayout()) {}

    juce::AudioProcessorEditor* createEditor() override;   // MultibandCompEditor.h

    float getBandGrDb (int b) const noexcept
    {
        return (b >= 0 && b < kBands) ? bandGr[(size_t) b].load (std::memory_order_relaxed) : 0.0f;
    }
    float getCrossover (int i) const   // i = 0..2
    {
        if (i == 0) return paramOf ("x1");
        if (i == 1) return paramOf ("x2");
        return paramOf ("x3");
    }

    static juce::String bandId (int b, juce::StringRef s) { return "b" + juce::String (b) + "_" + s; }

    static APVTS::ParameterLayout createLayout()
    {
        using BoolP  = juce::AudioParameterBool;
        using FloatP = juce::AudioParameterFloat;
        using Group  = juce::AudioProcessorParameterGroup;
        auto fa = [] (const char* u) { return juce::AudioParameterFloatAttributes().withLabel (u); };

        APVTS::ParameterLayout l;

        l.add (std::make_unique<Group> ("xo", "Crossovers", "|",
            std::make_unique<FloatP> (juce::ParameterID { "x1", 1 }, "Low / Low-Mid",
                juce::NormalisableRange<float> (40.0f, 500.0f, 1.0f, 0.4f), 120.0f, fa ("Hz")),
            std::make_unique<FloatP> (juce::ParameterID { "x2", 1 }, "Low-Mid / High-Mid",
                juce::NormalisableRange<float> (200.0f, 4000.0f, 1.0f, 0.4f), 1000.0f, fa ("Hz")),
            std::make_unique<FloatP> (juce::ParameterID { "x3", 1 }, "High-Mid / High",
                juce::NormalisableRange<float> (2000.0f, 16000.0f, 1.0f, 0.4f), 6000.0f, fa ("Hz"))));

        const char* bandNames[kBands] = { "Low", "Low-Mid", "High-Mid", "High" };
        for (int b = 0; b < kBands; ++b)
        {
            const juce::String gname = juce::String (b + 1) + "  Band: " + bandNames[b];
            l.add (std::make_unique<Group> (bandId (b, "grp").toStdString(), gname.toStdString(), "|",
                std::make_unique<BoolP>  (juce::ParameterID { bandId (b, "on"), 1 }, "On", true),
                std::make_unique<FloatP> (juce::ParameterID { bandId (b, "thresh"), 1 }, "Threshold",
                    juce::NormalisableRange<float> (-60.0f, 0.0f, 0.1f), -18.0f, fa ("dB")),
                std::make_unique<FloatP> (juce::ParameterID { bandId (b, "ratio"), 1 }, "Ratio",
                    juce::NormalisableRange<float> (1.0f, 20.0f, 0.1f, 0.4f), 3.0f, fa (": 1")),
                std::make_unique<FloatP> (juce::ParameterID { bandId (b, "attack"), 1 }, "Attack",
                    juce::NormalisableRange<float> (0.1f, 200.0f, 0.1f, 0.4f), 10.0f, fa ("ms")),
                std::make_unique<FloatP> (juce::ParameterID { bandId (b, "rel"), 1 }, "Release",
                    juce::NormalisableRange<float> (5.0f, 1000.0f, 1.0f, 0.4f), 120.0f, fa ("ms")),
                std::make_unique<FloatP> (juce::ParameterID { bandId (b, "makeup"), 1 }, "Makeup",
                    juce::NormalisableRange<float> (-12.0f, 24.0f, 0.1f), 0.0f, fa ("dB"))));
        }

        l.add (std::make_unique<Group> ("out", "Output", "|",
            std::make_unique<FloatP> (juce::ParameterID { "out_gain", 1 }, "Output Gain",
                juce::NormalisableRange<float> (-24.0f, 24.0f, 0.1f), 0.0f, fa ("dB"))));

        return l;
    }

protected:
    void prepareDsp (double sr, int blockSize, int numChannels) override
    {
        dspSampleRate = sr;
        chans = numChannels;
        juce::dsp::ProcessSpec spec { sr, (juce::uint32) juce::jmax (1, blockSize),
                                      (juce::uint32) juce::jmax (1, numChannels) };
        lr1.prepare (spec); lr2.prepare (spec); lr3.prepare (spec);
        for (int b = 0; b < kBands; ++b)
        {
            bandScratch[(size_t) b].assign ((size_t) numChannels, 0.0f);
            bandEnv[(size_t) b] = 0.0f;
            bandGr[(size_t) b].store (0.0f, std::memory_order_relaxed);
        }
        outSmoothed.reset (sr, 0.02);
    }

    void processDsp (juce::AudioBuffer<float>& buffer) override
    {
        const int ns  = buffer.getNumSamples();
        const int nch = juce::jmin (buffer.getNumChannels(), chans);
        if (nch == 0) return;

        // Ordered crossover frequencies.
        float x1 = juce::jlimit (40.0f, 500.0f,   paramOf ("x1"));
        float x2 = juce::jlimit (x1 + 1.0f, 4000.0f, paramOf ("x2"));
        float x3 = juce::jlimit (x2 + 1.0f, 18000.0f, paramOf ("x3"));
        lr1.setCutoffFrequency (x1);
        lr2.setCutoffFrequency (x2);
        lr3.setCutoffFrequency (x3);

        // Per-band params.
        struct BP { float thr, ratio, makeup, attCoeff, relCoeff; bool on; };
        BP bp[kBands];
        for (int b = 0; b < kBands; ++b)
        {
            bp[b].on     = paramOf (bandId (b, "on").toRawUTF8()) > 0.5f;
            bp[b].thr    = paramOf (bandId (b, "thresh").toRawUTF8());
            bp[b].ratio  = juce::jmax (1.0f, paramOf (bandId (b, "ratio").toRawUTF8()));
            bp[b].makeup = std::pow (10.0f, paramOf (bandId (b, "makeup").toRawUTF8()) * 0.05f);
            bp[b].attCoeff = std::exp (-1.0f / (float) (juce::jmax (0.1f, paramOf (bandId (b, "attack").toRawUTF8())) * 0.001 * dspSampleRate));
            bp[b].relCoeff = std::exp (-1.0f / (float) (juce::jmax (1.0f, paramOf (bandId (b, "rel").toRawUTF8()))    * 0.001 * dspSampleRate));
        }

        float blockMinGr[kBands] = { 0.0f, 0.0f, 0.0f, 0.0f };

        for (int i = 0; i < ns; ++i)
        {
            float bandPeak[kBands] = { 0.0f, 0.0f, 0.0f, 0.0f };
            for (int ch = 0; ch < nch; ++ch)
            {
                const float x = buffer.getReadPointer (ch)[i];
                float low0, high0, low1, high1, low2, high2;
                lr1.processSample (ch, x,     low0, high0);
                lr2.processSample (ch, high0, low1, high1);
                lr3.processSample (ch, high1, low2, high2);
                bandScratch[0][(size_t) ch] = low0;
                bandScratch[1][(size_t) ch] = low1;
                bandScratch[2][(size_t) ch] = low2;
                bandScratch[3][(size_t) ch] = high2;
                for (int b = 0; b < kBands; ++b)
                    bandPeak[b] = juce::jmax (bandPeak[b], std::abs (bandScratch[(size_t) b][(size_t) ch]));
            }

            float bandGain[kBands];
            for (int b = 0; b < kBands; ++b)
            {
                const float coeff = (bandPeak[b] > bandEnv[(size_t) b]) ? bp[b].attCoeff : bp[b].relCoeff;
                bandEnv[(size_t) b] = coeff * bandEnv[(size_t) b] + (1.0f - coeff) * bandPeak[b];

                float grDb = 0.0f;
                if (bp[b].on)
                {
                    const float over = juce::Decibels::gainToDecibels (bandEnv[(size_t) b], -100.0f) - bp[b].thr;
                    if (over > 0.0f) grDb = -(over * (1.0f - 1.0f / bp[b].ratio));
                    blockMinGr[b] = juce::jmin (blockMinGr[b], grDb);
                }
                bandGain[b] = std::pow (10.0f, grDb * 0.05f) * (bp[b].on ? bp[b].makeup : 1.0f);
            }

            for (int ch = 0; ch < nch; ++ch)
            {
                float o = 0.0f;
                for (int b = 0; b < kBands; ++b)
                    o += bandScratch[(size_t) b][(size_t) ch] * bandGain[b];
                buffer.getWritePointer (ch)[i] = o;
            }
        }

        for (int b = 0; b < kBands; ++b)
            bandGr[(size_t) b].store (blockMinGr[b], std::memory_order_relaxed);

        // Output gain.
        outSmoothed.setTargetValue (std::pow (10.0f, paramOf ("out_gain") * 0.05f));
        for (int i = 0; i < ns; ++i)
        {
            const float g = outSmoothed.getNextValue();
            for (int ch = 0; ch < nch; ++ch) buffer.getWritePointer (ch)[i] *= g;
        }
    }

private:
    float paramOf (juce::StringRef id) const { return param (id); }

    juce::dsp::LinkwitzRileyFilter<float> lr1, lr2, lr3;
    std::array<std::vector<float>, kBands> bandScratch;
    std::array<float, kBands> bandEnv { { 0.0f, 0.0f, 0.0f, 0.0f } };
    std::array<std::atomic<float>, kBands> bandGr;
    juce::SmoothedValue<float> outSmoothed;
    int chans = 2;
};

} // namespace dcr::builtin

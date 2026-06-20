#include "DSP/Builtin/InternalPluginFormat.h"
#include "DSP/Builtin/BuiltinProcessors.h"
#include "DSP/Builtin/ParametricEqEditor.h"
#include "DSP/Builtin/CompressorEditor.h"
#include "DSP/Builtin/DeEsserEditor.h"
#include "DSP/Builtin/ChannelStripEditor.h"
#include "DSP/Builtin/MultibandCompEditor.h"
#include "DSP/Builtin/LevelRiderEditor.h"
#include "DSP/Builtin/PpmMeterProcessor.h"
#include "DSP/Builtin/PpmMeterEditor.h"
#include "DSP/Builtin/SpectralAutoEqProcessor.h"
#include "DSP/Builtin/SpectralAutoEqEditor.h"
#include "DSP/Builtin/ResonanceSuppressorProcessor.h"
#include "DSP/Builtin/SpectralCurveEditor.h"

namespace dcr::builtin
{

// Out-of-line so the processors (in BuiltinProcessors.h) can return their
// editor types (separate headers) without a header cycle.
juce::AudioProcessorEditor* ParametricEQ::createEditor()        { return new ParametricEqEditor (*this); }
juce::AudioProcessorEditor* CompressorProcessor::createEditor() { return new CompressorEditor (*this); }
juce::AudioProcessorEditor* DeEsserProcessor::createEditor()    { return new DeEsserEditor (*this); }
juce::AudioProcessorEditor* ChannelStripProcessor::createEditor() { return new ChannelStripEditor (*this); }
juce::AudioProcessorEditor* MultibandCompProcessor::createEditor() { return new MultibandCompEditor (*this); }
juce::AudioProcessorEditor* LevelerProcessor::createEditor()       { return new LevelRiderEditor (*this); }
juce::AudioProcessorEditor* PpmMeterProcessor::createEditor()      { return new PpmMeterEditor (*this); }
juce::AudioProcessorEditor* SpectralAutoEqProcessor::createEditor()        { return new SpectralAutoEqEditor (*this); }
juce::AudioProcessorEditor* ResonanceSuppressorProcessor::createEditor()   { return new SpectralCurveEditor<ResonanceSuppressorProcessor> (*this); }

namespace
{
    std::unique_ptr<BuiltinProcessor> makeById (const juce::String& id)
    {
        if (id == ids::gain)       return std::make_unique<GainUtility>();
        if (id == ids::filter)     return std::make_unique<FilterProcessor>();
        if (id == ids::eq)         return std::make_unique<ParametricEQ>();
        if (id == ids::compressor) return std::make_unique<CompressorProcessor>();
        if (id == ids::gate)       return std::make_unique<NoiseGateProcessor>();
        if (id == ids::limiter)    return std::make_unique<LimiterProcessor>();
        if (id == ids::reverb)     return std::make_unique<ReverbProcessor>();
        if (id == ids::delay)      return std::make_unique<DelayProcessor>();
        if (id == ids::tone)       return std::make_unique<ToneProcessor>();
        if (id == ids::tremolo)    return std::make_unique<TremoloProcessor>();
        if (id == ids::width)      return std::make_unique<StereoWidthProcessor>();
        if (id == ids::deesser)    return std::make_unique<DeEsserProcessor>();
        if (id == ids::strip)      return std::make_unique<ChannelStripProcessor>();
        if (id == ids::mbcomp)     return std::make_unique<MultibandCompProcessor>();
        if (id == ids::leveler)    return std::make_unique<LevelerProcessor>();
        if (id == ids::ppm)        return std::make_unique<PpmMeterProcessor>();
        if (id == ids::autoeq)     return std::make_unique<SpectralAutoEqProcessor>();
        if (id == ids::resonance)  return std::make_unique<ResonanceSuppressorProcessor>();
        return nullptr;
    }
}

juce::Array<juce::PluginDescription> InternalPluginFormat::getBuiltinDescriptions()
{
    juce::Array<juce::PluginDescription> out;
    const char* allIds[] = {
        ids::gain, ids::filter, ids::eq, ids::compressor,
        ids::gate, ids::limiter, ids::reverb, ids::delay, ids::tone,
        ids::tremolo, ids::width, ids::deesser, ids::strip, ids::mbcomp,
        ids::leveler, ids::ppm, ids::autoeq, ids::resonance
    };
    for (auto* id : allIds)
        if (auto p = makeById (id))
        {
            juce::PluginDescription d;
            p->fillInPluginDescription (d);
            out.add (d);
        }
    return out;
}

void InternalPluginFormat::findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                                                const juce::String& fileOrIdentifier)
{
    if (auto p = makeById (fileOrIdentifier))
    {
        auto d = std::make_unique<juce::PluginDescription>();
        p->fillInPluginDescription (*d);
        results.add (d.release());
    }
}

juce::String InternalPluginFormat::getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier)
{
    if (auto p = makeById (fileOrIdentifier))
        return p->getName();
    return fileOrIdentifier;
}

void InternalPluginFormat::createPluginInstance (const juce::PluginDescription& desc,
                                                 double initialSampleRate, int initialBufferSize,
                                                 PluginCreationCallback callback)
{
    auto inst = makeById (desc.fileOrIdentifier);
    if (inst == nullptr)
    {
        callback (nullptr, "Unknown built-in plugin: " + desc.fileOrIdentifier);
        return;
    }
    inst->prepareToPlay (initialSampleRate, initialBufferSize);
    callback (std::move (inst), {});
}

} // namespace dcr::builtin

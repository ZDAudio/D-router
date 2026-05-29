#include "DSP/Builtin/InternalPluginFormat.h"
#include "DSP/Builtin/BuiltinProcessors.h"

namespace dcr::builtin
{

namespace
{
    std::unique_ptr<BuiltinProcessor> makeById (const juce::String& id)
    {
        if (id == ids::gain)       return std::make_unique<GainUtility>();
        if (id == ids::filter)     return std::make_unique<FilterProcessor>();
        if (id == ids::eq)         return std::make_unique<ParametricEQ>();
        if (id == ids::compressor) return std::make_unique<CompressorProcessor>();
        return nullptr;
    }
}

juce::Array<juce::PluginDescription> InternalPluginFormat::getBuiltinDescriptions()
{
    juce::Array<juce::PluginDescription> out;
    const char* allIds[] = { ids::gain, ids::filter, ids::eq, ids::compressor };
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

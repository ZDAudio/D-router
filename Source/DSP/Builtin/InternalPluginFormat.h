#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace dcr::builtin
{

// AudioPluginFormat that exposes D-Router's built-in DSP modules so they're
// created / serialized / restored through the same AudioPluginFormatManager
// pipeline as scanned Audio Units.  Register once via
// AudioPluginFormatManager::addFormat (new InternalPluginFormat()).
class InternalPluginFormat : public juce::AudioPluginFormat
{
public:
    InternalPluginFormat() = default;

    // Descriptions for the "Built-in" section of the FX load menu.
    static juce::Array<juce::PluginDescription> getBuiltinDescriptions();

    // ----- AudioPluginFormat ------------------------------------------------
    juce::String getName() const override { return "Internal"; }

    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results,
                              const juce::String& fileOrIdentifier) override;

    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override
    {
        return fileOrIdentifier.startsWith ("builtin:");
    }

    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;

    bool pluginNeedsRescanning (const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist  (const juce::PluginDescription& d) override
    {
        return d.fileOrIdentifier.startsWith ("builtin:");
    }
    bool canScanForPlugins() const override { return false; }
    bool isTrivialToScan()   const override { return true; }

    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath&, bool, bool) override { return {}; }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }

    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override
    {
        return false;
    }

protected:
    void createPluginInstance (const juce::PluginDescription& desc,
                               double initialSampleRate, int initialBufferSize,
                               PluginCreationCallback callback) override;
};

} // namespace dcr::builtin

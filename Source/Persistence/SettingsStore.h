#pragma once

#include <juce_core/juce_core.h>

#include "Engine/EngineSettings.h"

namespace dcr
{

    // Reads/writes EngineSettings to ~/Library/Application Support/dcorerouter/settings.xml.
    struct SettingsStore
    {
        static juce::File getFile();
        static EngineSettings load(); // returns defaults if file absent
        static bool save (const EngineSettings& s);
    };

} // namespace dcr

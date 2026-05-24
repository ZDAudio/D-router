#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_core/juce_core.h>

#include <vector>

#include "Engine/AudioEngine.h"

namespace dcr {

// A snapshot of the entire app's routing state. Plain data; no engine logic.
struct Snapshot
{
    double engineSampleRate = 48000.0;
    int    engineBlockSize  = 128;

    std::vector<AudioEngine::DeviceSpec> devices;

    std::vector<float> inputTrim;    // linear, per global input
    std::vector<float> outputTrim;   // linear, per global output
    std::vector<unsigned char> inputMute;
    std::vector<unsigned char> outputMute;
    std::vector<unsigned char> inputSolo;

    struct Crosspoint { int outputCh = 0, inputCh = 0; float gain = 0.0f; };
    std::vector<Crosspoint> crosspoints;   // sparse - only entries with gain != 0

    struct Group
    {
        juce::String       name;
        juce::String       layoutName;       // "Stereo", "5.1", "7.1.4", ...
        std::vector<int>   memberChannels;
        float              faderDb = 0.0f;
        bool               muted   = false;
    };
    std::vector<Group> outputGroups;
    std::vector<Group> inputGroups;
};

// Disk persistence of Snapshots as XML ValueTrees.
class SnapshotStore
{
public:
    static juce::File getDirectory();         // ~/Library/Application Support/dcorerouter/snapshots
    static juce::File getLastUsedFile();      // ~/Library/Application Support/dcorerouter/last.xml

    static bool save (const juce::File& file, const Snapshot& s);
    static bool load (const juce::File& file, Snapshot& outSnap);

    // ValueTree <-> Snapshot
    static juce::ValueTree toValueTree   (const Snapshot& s);
    static Snapshot        fromValueTree (const juce::ValueTree& tree);
};

} // namespace dcr

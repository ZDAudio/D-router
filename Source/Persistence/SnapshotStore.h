#pragma once

#include <juce_core/juce_core.h>
#include <juce_data_structures/juce_data_structures.h>

#include <vector>

#include "Engine/AudioEngine.h"

namespace dcr
{

    // A snapshot of the entire app's routing state. Plain data; no engine logic.
    struct Snapshot
    {
        double engineSampleRate = 48000.0;
        int engineBlockSize = 128;

        std::vector<AudioEngine::DeviceSpec> devices;

        std::vector<float> inputTrim; // linear, per global input
        std::vector<float> outputTrim; // linear, per global output
        std::vector<unsigned char> inputMute;
        std::vector<unsigned char> outputMute;
        std::vector<unsigned char> inputSolo;

        struct Crosspoint
        {
            int outputCh = 0, inputCh = 0;
            float gain = 0.0f;
        };
        std::vector<Crosspoint> crosspoints; // sparse - only entries with gain != 0

        struct Group
        {
            juce::String name;
            juce::String layoutName; // "Stereo", "5.1", "7.1.4", ...
            std::vector<int> memberChannels;
            float faderDb = 0.0f;
            bool muted = false;
            int faderMode = 0; // 0 = VCA (default), 1 = Router
        };
        std::vector<Group> outputGroups;
        std::vector<Group> inputGroups;

        // ===== Plugin chain persistence =========================================
        // Each PluginSlotState carries everything needed to re-instantiate one
        // plugin: the AU PluginDescription serialized as XML (looked up via the
        // AudioPluginFormatManager on restore) and the plugin's own persistent
        // state from getStateInformation(), base64'd.
        struct PluginSlotState
        {
            juce::String descriptionXml;
            juce::String stateB64;
            bool bypassed = false;
            bool isEmpty() const noexcept { return descriptionXml.isEmpty(); }
        };
        struct ChannelChain
        {
            int globalIdx = 0;
            bool isInput = true;
            std::vector<PluginSlotState> slots; // size = PluginHost::kNumSlots
        };
        struct GroupChain
        {
            int groupIdx = 0;
            bool isInput = false;
            std::vector<PluginSlotState> slots; // size = OutputGroup::kNumPluginSlots
        };
        std::vector<ChannelChain> channelChains;
        std::vector<GroupChain> groupChains;

        // Per-direction list of device names whose channels are folded in the
        // matrix view.  Stored as device-name strings rather than indices so
        // adding/removing a device in the future doesn't silently shift the
        // collapse mapping.
        std::vector<juce::String> collapsedInputDevices;
        std::vector<juce::String> collapsedOutputDevices;
    };

    // Disk persistence of Snapshots as XML ValueTrees.
    class SnapshotStore
    {
    public:
        // Bumped only on an incompatible snapshot-schema change.  toValueTree()
        // stamps it; load() rejects anything newer.
        static constexpr int kVersion = 1;

        // Why a load failed -- lets crash-recovery tell "no snapshot yet" (fine,
        // start blank) apart from "file is corrupt / from a newer build" (worth
        // telling the user) instead of collapsing both into a bare false.
        enum class LoadResult {
            Ok,
            NoFile,
            ParseError,
            UnsupportedVersion,
            Corrupt
        };

        static juce::File getDirectory(); // ~/Library/Application Support/dcorerouter/snapshots
        static juce::File getLastUsedFile(); // ~/Library/Application Support/dcorerouter/last.xml

        static bool save (const juce::File& file, const Snapshot& s);
        static LoadResult load (const juce::File& file, Snapshot& outSnap);

        // ValueTree <-> Snapshot
        static juce::ValueTree toValueTree (const Snapshot& s);
        static Snapshot fromValueTree (const juce::ValueTree& tree);
    };

} // namespace dcr

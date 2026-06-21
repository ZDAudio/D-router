#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "DSP/MultiChannelPluginHost.h"

namespace dcr
{

    // One bus master group.  Two fader behaviours, selectable per group:
    //
    //   VCA (default, backward compatible):
    //     Moving the group fader moves each member channel's own trim by the same
    //     dB delta (linked-fader).  Member faders visibly move together.  Group
    //     mute is ORed onto each member's mute.
    //   Router:
    //     Each member keeps its own independent trim, untouched by the group fader.
    //     The group fader is a SEPARATE gain stage multiplied on top
    //     (final = channelTrim x groupGain).  Group mute zeroes that overlay stage;
    //     member mute state is left alone.  The manager exposes the per-channel
    //     overlay gain via channelRouterGain[] for the RT thread to read.
    //   Group plugin:
    //     A single multi-channel insert that runs after per-channel plugins on
    //     the gathered N-channel bus, before the result is scattered back.
    struct OutputGroup
    {
        enum class FaderMode { VCA,
            Router };

        juce::String name { "Group" };
        juce::AudioChannelSet channelSet { juce::AudioChannelSet::stereo() };
        std::vector<int> memberChannels; // size = channelSet.size(); -1 = unfilled slot

        // Group fader expressed in dB.  VCA: the group position (delta reference);
        // member trims are kept in sync by the manager when this changes.  Router:
        // the overlay gain position read into channelRouterGain.
        std::atomic<float> faderDb { 0.0f };
        std::atomic<bool> muted { false };
        std::atomic<FaderMode> faderMode { FaderMode::VCA };

        // Fixed insert chain.  Slots are processed in order; empty / bypassed
        // slots are skipped.
        static constexpr int kNumPluginSlots = 5;
        std::array<std::unique_ptr<MultiChannelPluginHost>, kNumPluginSlots> pluginSlots;

        OutputGroup()
        {
            for (auto& p : pluginSlots)
                p = std::make_unique<MultiChannelPluginHost>();
        }
    };

} // namespace dcr

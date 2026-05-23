#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "DSP/MultiChannelPluginHost.h"

namespace dcr {

// One bus master / linked-fader group.
//
//   Linked-fader semantics:
//     Moving the group fader moves each member channel's outputTrim by the
//     same dB delta (Pro Tools "Group" behaviour, not VCA modulation).
//   Group mute:
//     ORed onto each member's mute - while group mute is on, members are
//     silenced regardless of their own mute state.
//   Group plugin:
//     A single multi-channel insert that runs after per-channel plugins on
//     the gathered N-channel bus, before the result is scattered back.
struct OutputGroup
{
    juce::String           name        { "Group" };
    juce::AudioChannelSet  channelSet  { juce::AudioChannelSet::stereo() };
    std::vector<int>       memberChannels;   // size = channelSet.size(); -1 = unfilled slot

    // Group fader expressed in dB; member trims are kept in sync by the
    // manager when this changes.
    std::atomic<float>     faderDb { 0.0f };
    std::atomic<bool>      muted   { false };

    // Fixed insert chain.  Slots are processed in order; empty / bypassed
    // slots are skipped.
    static constexpr int kNumPluginSlots = 5;
    std::array<std::unique_ptr<MultiChannelPluginHost>, kNumPluginSlots> pluginSlots;

    OutputGroup()
    {
        for (auto& p : pluginSlots) p = std::make_unique<MultiChannelPluginHost>();
    }
};

} // namespace dcr

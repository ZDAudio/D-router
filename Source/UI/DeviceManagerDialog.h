#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

#include "Engine/AudioEngine.h"

namespace dcr {

// Modal-ish device picker. Lists every CoreAudio device and lets the user
// tick which directions (input / output) participate in routing.
class DeviceManagerDialog : public juce::Component
{
public:
    DeviceManagerDialog (AudioEngine& engine,
                         const std::vector<AudioEngine::DeviceSpec>& currentSelection);

    // Called on OK with the new selection. If cancelled, called with std::nullopt.
    std::function<void (std::optional<std::vector<AudioEngine::DeviceSpec>>)> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;

    static void launch (AudioEngine& engine,
                        const std::vector<AudioEngine::DeviceSpec>& currentSelection,
                        std::function<void (std::optional<std::vector<AudioEngine::DeviceSpec>>)> callback);

private:
    struct Row
    {
        juce::String name;
        bool hasInput  = false;
        bool hasOutput = false;
        juce::ToggleButton inputBtn;
        juce::ToggleButton outputBtn;
        juce::Label nameLabel;
    };

    AudioEngine& engine;
    juce::OwnedArray<Row> rows;
    juce::TextButton okButton     { "OK" };
    juce::TextButton cancelButton { "Cancel" };
    juce::Label      title        { {}, "Select devices to route" };
    juce::Viewport   viewport;
    juce::Component  rowsHolder;
};

} // namespace dcr

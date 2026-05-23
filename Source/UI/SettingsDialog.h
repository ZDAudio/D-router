#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>

#include "Engine/EngineSettings.h"

namespace dcr {

// Small "i" circle with a tooltip - hover to see description + recommended value.
class InfoIcon : public juce::Component, public juce::SettableTooltipClient
{
public:
    InfoIcon() = default;
    void paint (juce::Graphics&) override;
};


// All-knobs editor for EngineSettings.  No magic numbers anywhere in the
// engine -- everything tunable lives here.  Changes to audio-path fields
// require restarting the audio engine (caller responsibility).
class SettingsDialog : public juce::Component
{
public:
    explicit SettingsDialog (const EngineSettings& initial);

    // Called with edited settings on Apply, std::nullopt on Cancel.
    std::function<void (std::optional<EngineSettings>)> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;

    static void launch (const EngineSettings& initial,
                        std::function<void (std::optional<EngineSettings>)> cb);

private:
    void addSection      (const juce::String& heading);
    void addIntField     (const juce::String& name, int& target, int minVal, int maxVal,
                          const juce::String& unitHint, const juce::String& tooltip);
    void addDoubleField  (const juce::String& name, double& target, double minVal, double maxVal,
                          const juce::String& unitHint, const juce::String& tooltip);
    void addFloatField   (const juce::String& name, float& target, float minVal, float maxVal,
                          const juce::String& unitHint, const juce::String& tooltip);
    void addUIntComboField (const juce::String& name, unsigned int& target,
                            const juce::StringArray& labels,
                            const std::vector<unsigned int>& values,
                            const juce::String& tooltip);
    void attachInfoIcon (const juce::String& tooltip);

    EngineSettings working;
    std::vector<std::function<void()>> applyActions;
    int nextRowY = 0;

    juce::OwnedArray<juce::Label>     labels;
    juce::OwnedArray<juce::TextEditor> editors;
    juce::OwnedArray<juce::ComboBox>  combos;
    juce::OwnedArray<juce::Label>     sectionHeads;
    juce::OwnedArray<InfoIcon>        infoIcons;
    juce::TooltipWindow               tooltipWindow { this, 350 };

    juce::TextButton applyButton  { "Apply" };
    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton resetButton  { "Reset to defaults" };

    juce::Viewport viewport;
    juce::Component fieldsHolder;
};

} // namespace dcr

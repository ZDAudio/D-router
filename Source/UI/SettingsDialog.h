#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <optional>
#include <vector>

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

    // Three-way close:
    //   settings has value, persistToDisk = true   -> user clicked SAVE
    //   settings has value, persistToDisk = false  -> user clicked APPLY (session only)
    //   settings == std::nullopt                   -> user clicked CANCEL
    std::function<void (std::optional<EngineSettings> settings, bool persistToDisk)> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;

    static void launch (const EngineSettings& initial,
                        std::function<void (std::optional<EngineSettings>, bool)> cb);

private:
    // Section divider with an optional plain-language subtitle.  The
    // subtitle renders beneath the bold heading in a dim grey -- a single
    // line of "what this section actually controls" so the user doesn't
    // have to mouse-hover every info icon just to orient themselves.
    void addSection      (const juce::String& heading,
                          const juce::String& subtitle = {});
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
    // ComboBox over a curated list of int / double values.  Replaces the
    // free-form text editor for ring / pre-fill / block-size knobs so the
    // user can only pick values the engine is known to handle without
    // crashing.  If `target` doesn't match any list entry, snaps to the
    // closest value.
    void addIntChoiceField    (const juce::String& name, int&    target,
                               const std::vector<int>&    values,
                               const juce::String& unitHint,
                               const juce::String& tooltip);
    void addDoubleChoiceField (const juce::String& name, double& target,
                               const std::vector<double>& values,
                               const juce::String& unitHint,
                               const juce::String& tooltip);
    void addHexColorField (const juce::String& name, unsigned int& target,
                           const juce::String& tooltip);
    // A "Buffer safety" preset slider (rightmost = safest, leftmost = most
    // aggressive) PLUS the five precise ring/pre-fill ComboBoxes it drives.
    // Two-way linked: dragging the slider sets all five combos; editing any
    // combo by hand flips the slider's readout to "Custom".
    void addBufferSafetySection (const juce::String& tooltip);
    void applyBufferPreset (int level);      // slider -> the five combos
    void updateBufferSliderState();          // the five combos -> slider readout
    void attachInfoIcon (const juce::String& tooltip);

    EngineSettings working;
    std::vector<std::function<void()>> applyActions;
    int nextRowY = 0;

    juce::OwnedArray<juce::Label>     labels;
    juce::OwnedArray<juce::TextEditor> editors;
    juce::OwnedArray<juce::ComboBox>  combos;
    juce::OwnedArray<juce::Slider>    sliders;

    // Buffer-safety preset wiring (non-owning pointers into the arrays above).
    juce::Slider*   bufferSafetySlider     = nullptr;
    juce::Label*    bufferSafetyStateLabel = nullptr;
    std::array<juce::ComboBox*, 5>  ringCombos {};
    std::array<std::vector<int>, 5> ringValues {};
    bool            syncingBuffers = false;   // guards the two-way update
    juce::OwnedArray<juce::Label>     sectionHeads;
    juce::OwnedArray<InfoIcon>        infoIcons;
    juce::TooltipWindow               tooltipWindow { this, 350 };

    juce::TextButton applyButton  { "Apply" };
    juce::TextButton saveButton   { "Save" };
    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton resetButton  { "Reset to defaults" };

    juce::Viewport viewport;
    juce::Component fieldsHolder;
};

} // namespace dcr

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "UI/LevelMeter.h"

namespace dcr {

class AudioEngine;
class MultiChannelPluginHost;

// Bottom panel (or floating window) listing one card per output group.
// Each card has: name, linked fader, mute, meter, members, "pop out"
// button, and a 5-slot plugin chain (each slot has B / name / X widgets).
class OutputGroupPanel : public juce::Component, private juce::Timer
{
public:
    explicit OutputGroupPanel (AudioEngine& engine);
    ~OutputGroupPanel() override;

    // Call after the engine restarts or groups are added/removed/renamed.
    void rebuild();

    // Button on each card asks the host (MainComponent) to detach/re-attach
    // this panel into a separate window.
    std::function<void()> onPopOutRequested;
    void setDetached (bool d) { detached = d; for (auto* c : cards) c->updatePopOut (d); }

    // Fires when the mouse hovers into / out of a group card.  Argument is
    // the group's member output channels (or empty when no card is hovered).
    std::function<void (const std::vector<int>&)> onGroupHover;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    AudioEngine& engine;
    bool detached = false;

    // One row in a slot column.
    struct SlotRow
    {
        int slotIdx = 0;
        juce::TextButton bypass { "B" };
        juce::TextButton name   { "+ insert" };
        juce::TextButton remove { "X" };
    };

    struct Card : public juce::Component
    {
        int groupIdx = 0;
        OutputGroupPanel* panel = nullptr;

        juce::Label      name;
        juce::Slider     fader { juce::Slider::LinearVertical, juce::Slider::NoTextBox };
        juce::TextButton mute  { "M" };
        juce::TextButton popOut { "Win" };
        LevelMeter       meter { LevelMeter::Orientation::Vertical };
        juce::Label      members;

        std::array<std::unique_ptr<SlotRow>, 5> slots;
        std::array<std::unique_ptr<juce::DocumentWindow>, 5> editorWindows;

        void buildFor (OutputGroupPanel& p, int gIdx);
        void resized() override;
        void updatePopOut (bool detached);

        MultiChannelPluginHost* getSlotHost (int slotIdx) const;
        void onSlotNameClicked (int slotIdx);
        void onSlotRemoveClicked (int slotIdx);
        void refreshSlotAppearance (int slotIdx);
        void openEditorFor (int slotIdx);
        void closeEditorFor (int slotIdx);
    };
    juce::OwnedArray<Card> cards;
    int lastHoveredCardIdx = -1;

    // Cards live inside a Viewport so we get horizontal scrolling when the
    // panel is narrower than the total cards width.
    juce::Viewport   cardsViewport;
    juce::Component  cardsHolder;

    std::unique_ptr<juce::FileChooser> pluginFileChooser;
    int pendingLoadCardIdx = -1;
    int pendingLoadSlotIdx = -1;

    void requestLoadPlugin (int cardIdx, int slotIdx);
};

} // namespace dcr

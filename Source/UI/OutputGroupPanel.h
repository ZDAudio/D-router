#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "UI/DragSlotButton.h"
#include "UI/LevelMeter.h"

namespace dcr {

class AudioEngine;
class MultiChannelPluginHost;
struct OutputGroup;

// Bottom panel (or floating window) listing one card per output group.
// Each card has: name, linked fader, mute, meter, members, "pop out"
// button, and a 5-slot plugin chain (each slot has B / name / X widgets).
class OutputGroupPanel : public juce::Component, private juce::Timer
{
public:
    enum class Direction { Outputs, Inputs };

    explicit OutputGroupPanel (AudioEngine& engine, Direction dir = Direction::Outputs);
    ~OutputGroupPanel() override;

    Direction getDirection() const noexcept { return direction; }

    // Call after the engine restarts or groups are added/removed/renamed.
    void rebuild();

    // Single header button (top-right) asks the host (MainComponent) to
    // detach/re-attach this panel into a separate window.
    std::function<void()> onPopOutRequested;
    void setDetached (bool d);

    // The header button itself, owned here so MainComponent can interrogate it.
    juce::TextButton popOutBtn { "->" };

    void visibilityChanged() override;

    // Fires when the mouse hovers into / out of a group card.  Argument is
    // the group's member output channels (or empty when no card is hovered).
    std::function<void (const std::vector<int>&)> onGroupHover;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    AudioEngine& engine;
    Direction    direction;
    bool detached = false;

    // Direction-aware accessors used by the card widgets.  All other code can
    // ignore which side it's painting.
    int          mgrNumGroups() const;
    OutputGroup* mgrGetGroup (int idx);
    const OutputGroup* mgrGetGroup (int idx) const;
    int          mgrCreateGroup (juce::String name, juce::AudioChannelSet cs);
    void         mgrRemoveGroup (int idx);
    void         mgrAssignChannel (int gIdx, int slot, int globalCh);
    void         mgrMoveFader (int gIdx, float db);
    void         mgrSetMute   (int gIdx, bool m);
    float        srcPeak (int globalCh) const;
    juce::String resolveChannelName (int globalCh) const;

    // One row in a slot column.  `name` is a DragSlotButton so the user can
    // drag it onto another row's name to reorder the plugin chain within the
    // owning Card (which acts as the DragAndDropContainer).
    struct SlotRow
    {
        int slotIdx = 0;
        juce::TextButton bypass { "B" };
        DragSlotButton   name;
        juce::TextButton remove { "X" };
    };

    struct Card : public juce::Component,
                  public juce::DragAndDropContainer
    {
        int groupIdx = 0;
        OutputGroupPanel* panel = nullptr;

        juce::Label      name;
        juce::Slider     fader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
        juce::TextButton mute  { "M" };
        juce::TextButton popOut { "Win" };
        LevelMeter       meter { LevelMeter::Orientation::Vertical };
        juce::Label      members;

        std::array<std::unique_ptr<SlotRow>, 5> slots;
        std::array<std::unique_ptr<juce::DocumentWindow>, 5> editorWindows;

        void buildFor (OutputGroupPanel& p, int gIdx);
        void paint (juce::Graphics&) override;
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
    int slotRefreshCounter = 0;

    // Cards live inside a Viewport so we get horizontal scrolling when the
    // panel is narrower than the total cards width.
    juce::Viewport   cardsViewport;
    juce::Component  cardsHolder;

    // Small text shown when panel is embedded - hover-to-highlight only works
    // when the panel is popped out (so the matrix is visible alongside).
    juce::Label      hoverHintLabel;
    juce::Label      panelTitle;

    std::unique_ptr<juce::FileChooser> pluginFileChooser;
    int pendingLoadCardIdx = -1;
    int pendingLoadSlotIdx = -1;

    void requestLoadPlugin (int cardIdx, int slotIdx);
};

} // namespace dcr

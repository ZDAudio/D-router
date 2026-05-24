#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <vector>

#include "DSP/PluginHost.h"
#include "UI/CrosspointGrid.h"
#include "UI/DragSlotButton.h"
#include "UI/LevelMeter.h"

namespace dcr {

class AudioEngine;

// Grid of crosspoints with channel strips on the rails.
//   Left rail per input  row : [name label] [trim rotary] [horizontal meter]
//   Top  rail per output col : [rotated name] [trim rotary] [vertical meter]
// MatrixView is meant to be hosted in a juce::Viewport for scrolling.
class MatrixView : public juce::Component,
                   private juce::Timer,
                   private juce::ScrollBar::Listener
{
public:
    explicit MatrixView (AudioEngine& engine);
    ~MatrixView() override;

    // Re-read engine state and rebuild all children.
    void rebuildFromEngine();

    // Pause/resume meter polling - call before touching engine on another thread.
    void pauseUpdates()  { stopTimer(); }
    void resumeUpdates();

    // Close every open per-channel plugin editor window.  MUST be called from
    // the message thread BEFORE AudioEngine::stop() drops the PluginHosts,
    // otherwise the editor's destructor calls editorBeingDeleted() on an
    // already-destroyed AudioPluginInstance and segfaults.
    void closeAllPluginEditors();

    // Re-read every mute button's toggle state from the routing matrix.
    // Used by MainComponent's PANIC handler when it programmatically flips
    // mutes on the matrix side; this keeps the UI buttons in sync without
    // firing any onClick callbacks.
    void refreshMuteButtonStates();

    // Fires when the user clicks one of the per-row mute buttons (input or
    // output side).  MainComponent uses this to detect manual mute changes
    // while panic is active, so the saved pre-panic state can be discarded.
    std::function<void()> onUserMuteChanged;

    // Update the FX button's tint (grey/cyan/dim-red) for one channel.
    // Public so snapshot restore can refresh the appearance after each
    // async plugin reload.
    void updateFxButtonAppearance (bool isInput, int ch);

    // Highlight a set of output column indices.  Called when an OUTPUT group
    // card is hovered.  Pass an empty list to clear.
    void setHighlightedOutputs (std::vector<int> outs);

    // Same for INPUT group hover -> input rows highlighted.
    void setHighlightedInputs  (std::vector<int> ins);

    void paint (juce::Graphics&) override;
    void resized() override;

    // Layout constants
    static constexpr int cellSize       = 36;
    static constexpr int labelColWidth  = 310;   // widened to fit input FX button
    static constexpr int labelRowHeight = 184;

private:
    void timerCallback() override;
    juce::Slider* makeTrimSlider();
    void scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart) override;

    struct ChannelLabel
    {
        juce::String deviceName;
        int channelIndex = 0;        // 1-based for display
        bool startsNewGroup = true;
    };

    // Small floating component shown by CallOutBox when the user clicks a
    // per-channel "FX" button.  Three rows of [B] [slot name] [X] — same
    // layout as the per-group plugin chain in OutputGroupPanel::Card.  The
    // popup self-refreshes every 150 ms so it picks up async plugin loads
    // without needing a manual hook from the loader callback.
    //
    // Inherits DragAndDropContainer so the user can drag a slot's name
    // button onto another slot's name button to reorder the chain.
    class FxChainPopupContent : public juce::Component,
                                public  juce::DragAndDropContainer,
                                private juce::Timer
    {
    public:
        FxChainPopupContent (MatrixView& owner, bool isInput, int ch);
        ~FxChainPopupContent() override;
        void resized() override;
        void paint (juce::Graphics&) override;
    private:
        void timerCallback() override;
        void refreshAll();
        void onSlotNameClicked   (int slotIdx);
        void onSlotBypassClicked (int slotIdx);
        void onSlotRemoveClicked (int slotIdx);

        struct Row
        {
            juce::TextButton bypass { "B" };
            // 'name' is a DragSlotButton so the user can drag it onto another
            // row's name button to reorder the plugin chain.
            DragSlotButton   name;
            juce::TextButton remove { "X" };
        };
        std::array<Row, 3> rows;     // kNumSlots in PluginHost == 3
        juce::Label        header;
        MatrixView& owner;
        bool isInput;
        int  ch;
    };

    // Excel-style freezing sub-components
    class LeftRailContent : public juce::Component
    {
    public:
        explicit LeftRailContent (MatrixView& owner) : owner (owner) {}
        void paint (juce::Graphics& g) override;
    private:
        MatrixView& owner;
    };

    class TopRailContent : public juce::Component
    {
    public:
        explicit TopRailContent (MatrixView& owner) : owner (owner) {}
        void paint (juce::Graphics& g) override;
    private:
        MatrixView& owner;
    };

    class CornerCell : public juce::Component
    {
    public:
        explicit CornerCell (MatrixView& owner) : owner (owner) {}
        void paint (juce::Graphics& g) override;
    private:
        MatrixView& owner;
    };

    void paintLeftRail (juce::Graphics& g);
    void paintTopRail (juce::Graphics& g);
    void paintCornerCell (juce::Graphics& g);
    void layoutLeftRail();
    void layoutTopRail();

    AudioEngine& engine;
    std::vector<ChannelLabel> inputLabels;
    std::vector<ChannelLabel> outputLabels;

    // Per-input-row widgets
    juce::OwnedArray<juce::Label>        inputNames;
    juce::OwnedArray<juce::Slider>       inputTrims;
    juce::OwnedArray<LevelMeter>         inputMeters;
    juce::OwnedArray<juce::TextButton>   inputMuteBtns;
    juce::OwnedArray<juce::TextButton>   inputSoloBtns;

    // Per-output-column widgets
    juce::OwnedArray<juce::Slider>       outputTrims;
    juce::OwnedArray<LevelMeter>         outputMeters;
    juce::OwnedArray<juce::TextButton>   outputMuteBtns;
    juce::OwnedArray<juce::TextButton>   outputFxBtns;

    std::unique_ptr<CrosspointGrid> grid;
    std::vector<int> highlightedOutputs;
    std::vector<int> highlightedInputs;

    // Scroll areas
    juce::Viewport gridViewport;
    juce::Viewport leftRailViewport;
    juce::Viewport topRailViewport;

    LeftRailContent leftRailContent { *this };
    TopRailContent  topRailContent  { *this };
    CornerCell      cornerCell      { *this };

    // Direction-aware FX helpers.  isInput = true addresses input plugin
    // hosts (engine.getInputPluginHost), false the output ones.
    void openFxMenuFor (bool isInput, int ch);
    void loadPluginInto (bool isInput, int ch, int slotIdx);
    void showEditorFor  (bool isInput, int ch, int slotIdx);
    void closeEditorFor (bool isInput, int ch, int slotIdx);
    // updateFxButtonAppearance lives in the public section now -- it's called
    // from MainComponent's snapshot-restore async callbacks.

    // Per-slot editor windows keyed by (direction, channel, slot).  A simple
    // 2D array per direction since both channel count and slot count are
    // small.
    std::vector<std::array<std::unique_ptr<juce::DocumentWindow>, 3>> outputEditorWindows;
    std::vector<std::array<std::unique_ptr<juce::DocumentWindow>, 3>> inputEditorWindows;

    juce::OwnedArray<juce::TextButton> inputFxBtns;
    std::unique_ptr<juce::FileChooser> pluginFileChooser;
};

} // namespace dcr

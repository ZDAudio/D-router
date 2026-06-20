#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <set>
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

    // Progress callbacks for the chunked-rebuild path.  MainComponent's
    // LoadingOverlay subscribes so the user sees "Loading routing... 24/114"
    // instead of a frozen window when devices have a lot of channels.
    // (totalSteps == 0 => indeterminate or trivial layout.)
    std::function<void (int doneSteps, int totalSteps)> onRebuildProgress;
    std::function<void()> onRebuildFinished;

    // ----- Multi-channel selection (FX broadcast) ----------------------------
    // Click on a channel-name label drives the selection set.  Shift-click
    // extends the range from the last clicked, Cmd-click toggles a single
    // channel, plain click selects only the clicked one.  Selection is per
    // direction (input list independent of output list).
    void handleChannelHeaderClick (bool isInput, int ch, const juce::ModifierKeys& mods);
    std::vector<int> getSelectedChannels (bool isInput) const;
    void clearChannelSelection (bool isInput);
    bool isChannelSelected (bool isInput, int ch) const;

    // Highlight a set of output column indices.  Called when an OUTPUT group
    // card is hovered.  Pass an empty list to clear.
    void setHighlightedOutputs (std::vector<int> outs);

    // Same for INPUT group hover -> input rows highlighted.
    void setHighlightedInputs  (std::vector<int> ins);

    // ----- Device collapse / expand ------------------------------------------
    // Per-direction, per-device.  A collapsed device shows in the rail as a
    // single virtual row (input) or column (output) with a [+] button to
    // re-expand.  The collapsed cell at the intersection of two
    // collapsed-or-expanded devices is rendered as a device-level aggregate
    // crosspoint: lit if ANY underlying channel-pair has a non-zero gain.
    // Engine state (gains, mutes, plugin chains) is preserved while
    // collapsed -- this is purely a UI-visibility toggle.
    void setDeviceCollapsed (bool isInput, const juce::String& deviceName, bool collapsed);
    bool isDeviceCollapsed  (bool isInput, const juce::String& deviceName) const;
    void collapseAllDevices (bool isInput, bool collapsed);

    // Snapshot integration.  MainComponent reads these on save and pushes
    // them back via setCollapsedDeviceNames on load.
    std::vector<juce::String> getCollapsedDeviceNames (bool isInput) const;
    void setCollapsedDeviceNames (bool isInput, std::vector<juce::String> names);

    // Fires whenever the collapse state changes so MainComponent can trigger
    // an auto-save -- analogous to onUserMuteChanged.
    std::function<void()> onCollapseStateChanged;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Layout constants
    static constexpr int cellSize       = 36;
    static constexpr int labelColWidth  = 310;   // widened to fit input FX button
    // Top-rail height needs to be tall enough to fit the rotated device +
    // channel labels at a readable size on a long multi-channel device
    // name.  At ~280 px the rotated string
    // gets ~250 px of arc length, which fits a 14 pt monospaced label
    // without truncation.
    static constexpr int labelRowHeight = 280;
    // Where in the header band the rotated text is drawn.  Pushed down to
    // sit just above the collapse button band.
    static constexpr int topRailWidgetsY = labelRowHeight - 148;   // 132

private:
    void timerCallback() override;
    juce::Slider* makeTrimSlider();
    void scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart) override;

public:
    // Public so the static free-function helper that maps engine channel
    // -> visible label index can name it from within MatrixView.cpp.  The
    // struct is otherwise treated as implementation-private.
    struct ChannelLabel
    {
        juce::String deviceName;
        int channelIndex = 0;        // 1-based for display; -1 when isCollapsedRow
        bool startsNewGroup = true;

        // When the device is collapsed, the rail has a single virtual row /
        // column for it instead of one per channel.  The crosspoint grid
        // treats this entry as a "device aggregate" -- the cell lights up
        // if ANY underlying channel-pair has a non-zero route.
        bool isCollapsedRow = false;
        int  firstChannel   = 0;     // global engine index of first channel
        int  channelCount   = 1;     // 1 for normal rows, N for collapsed
    };

private:

    // Small floating component shown by CallOutBox when the user clicks a
    // per-channel "FX" button.  Three rows of [B] [slot name] [X] — same
    // layout as the per-group plugin chain in OutputGroupPanel::Card.  The
    // popup self-refreshes every 150 ms so it picks up async plugin loads
    // without needing a manual hook from the loader callback.
    //
    // Inherits DragAndDropContainer so the user can drag a slot's name
    // button onto another slot's name button to reorder the chain.
    //
    // `targetChannels` is the list of channels every popup action applies
    // to.  Single-channel click -> [ch].  Click on one of N selected
    // channels -> all N (multi-broadcast mode).  Load / bypass / remove /
    // drag-reorder all fan out to every entry; editor-open uses only the
    // primary channel (no point opening 12 windows at once).
    class FxChainPopupContent : public juce::Component,
                                public  juce::DragAndDropContainer,
                                private juce::Timer
    {
    public:
        FxChainPopupContent (MatrixView& owner, bool isInput, int primaryCh,
                             std::vector<int> targetChannels);
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
        int  ch;            // primary -- used for editor-open + appearance refresh
        std::vector<int> targets;   // every channel actions broadcast to
    };

    // Excel-style freezing sub-components
    class LeftRailContent : public juce::Component
    {
    public:
        explicit LeftRailContent (MatrixView& owner) : owner (owner) {}
        void paint (juce::Graphics& g) override;
        // Forward wheel events to the grid viewport so the user can scroll
        // the matrix by spinning the wheel anywhere over the left rail --
        // including over the trim slider, mute / solo / fx buttons, the
        // channel name label, etc.  The trim slider's own scroll-to-adjust
        // is disabled (setScrollWheelEnabled(false) in makeTrimSlider) so
        // the wheel bubbles up here instead of nudging the trim.
        void mouseWheelMove (const juce::MouseEvent&,
                             const juce::MouseWheelDetails&) override;
        // Click in any gap between widgets clears the INPUT selection.
        // SelectableLabel children swallow their own clicks (those set
        // selection), so this only fires for true empty space.
        void mouseDown (const juce::MouseEvent&) override;
    private:
        MatrixView& owner;
    };

    class TopRailContent : public juce::Component
    {
    public:
        explicit TopRailContent (MatrixView& owner) : owner (owner) {}
        void paint (juce::Graphics& g) override;
        // Clicks in the rotated-label band drive output channel selection;
        // clicks below the label band on empty space CLEAR the output
        // selection (the channel-widget rows below own their own mouse
        // handling, so this only fires for true empty space there).
        void mouseDown (const juce::MouseEvent& e) override;
        // Wheel: re-map vertical to horizontal so a regular mouse wheel
        // scrolls outputs left/right naturally.  Trackpad horizontal
        // swipes stay as-is.
        void mouseWheelMove (const juce::MouseEvent&,
                             const juce::MouseWheelDetails&) override;
    private:
        MatrixView& owner;
    };

    class CornerCell : public juce::Component
    {
    public:
        explicit CornerCell (MatrixView& owner) : owner (owner) {}
        void paint (juce::Graphics& g) override;
        // Click on the top-left corner cell clears BOTH input and output
        // selections -- it's the natural "neutral" zone of the matrix.
        void mouseDown (const juce::MouseEvent&) override;
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

    // Sets of device names whose channels are currently collapsed in the
    // matrix view.  Independent per direction so a duplex device can have
    // its inputs hidden while its outputs are still expanded.
    std::set<juce::String> collapsedInputDevices;
    std::set<juce::String> collapsedOutputDevices;
    void notifyCollapseChanged();

    // One-shot: set by collapse/expand calls so the next rebuildFromEngine()
    // skips the same-physical-layout fast path and does a real structural
    // rebuild.  Without this the fast path sees the (not-yet-rebuilt) labels
    // still matching the engine's physical channel list and bails, so the
    // first collapse while everything is expanded silently does nothing.
    bool forceStructuralRebuild = false;

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

    // Expand/Collapse-all buttons sitting in the top-left corner legend,
    // next to the INPUTS (bottom-left) and OUTPUTS (top-right) labels.
    // These are the same actions previously buried in the "Layout..." menu.
    juce::TextButton inExpandAllBtn    { "Expand" };
    juce::TextButton inCollapseAllBtn  { "Collapse" };
    juce::TextButton outExpandAllBtn   { "Expand" };
    juce::TextButton outCollapseAllBtn { "Collapse" };

    // Shown over the (empty) grid area when no devices are selected, to guide
    // a first-time user to the Devices dialog.  Toggled by the rebuild.
    juce::Label emptyHint;
    void updateEmptyHintVisibility();

    // Direction-aware FX helpers.  isInput = true addresses input plugin
    // hosts (engine.getInputPluginHost), false the output ones.
    // ---- Rebuild helpers (chunked) ----
    // Returns true when the channel layout produced by the engine right
    // now matches what we already have widgets for -- in which case we
    // can skip the heavy widget recreation and just refresh values.
    bool samePhysicalLayout() const;
    void softRefreshFromEngine();
    // Lightweight per-channel trim/mute/solo slider resync, run from the meter
    // timer whenever the routing matrix's dirty generation changes.  Keeps the
    // trim sliders tracking engine-side changes the user didn't make directly --
    // a VCA group fader riding its members, multi-select links, snapshot
    // restore.  Skips any slider currently under the mouse so it never fights a
    // live drag.  lastTrimRefreshGen gates it so a static matrix costs nothing.
    void refreshTrimWidgetsFromEngine();
    uint64_t lastTrimRefreshGen = 0;
    void clearAllChannelWidgets();
    void buildLabelsFromEngine();
    void buildInputRowWidgets  (int n);
    void buildOutputColumnWidgets (int m);
    void continueRebuild();
    void finishRebuild();

    // Cancellable async rebuild state.  Generation bumps every time
    // rebuildFromEngine() starts -- pending continueRebuild() callAsyncs
    // from a previous invocation bail out at their entry check.
    struct RebuildState
    {
        uint64_t generation    = 0;
        bool     active        = false;
        int      nextInputIdx  = 0;
        int      nextOutputIdx = 0;
        int      totalInputs   = 0;
        int      totalOutputs  = 0;
        uint32_t startMs       = 0;   // for perf logging only
        int      chunkCount    = 0;
    };
    RebuildState rebuildState;
    uint64_t     rebuildGenerationCounter = 0;

    void openFxMenuFor (bool isInput, int ch);
    void loadPluginInto (bool isInput, int ch, int slotIdx);   // legacy single-channel
    // Multi-broadcast variant: pick a plugin once via the file chooser, then
    // instantiate it into the SAME slot on every channel in `channels`,
    // wiping whatever was previously there.
    void loadPluginIntoMulti (bool isInput, std::vector<int> channels, int slotIdx);
    // Instantiate one PluginDescription (built-in or AU) into `slotIdx` on
    // every channel in `channels`.  Shared tail of both the built-in menu
    // pick and the AU file-browse pick.
    void instantiateAndBroadcast (bool isInput, std::vector<int> channels, int slotIdx,
                                  juce::PluginDescription desc);
    void browseForAuAndBroadcast (bool isInput, std::vector<int> channels, int slotIdx);
    void showEditorFor  (bool isInput, int ch, int slotIdx);
    void closeEditorFor (bool isInput, int ch, int slotIdx);

    // Channel-selection state (drives FX broadcast).
    std::vector<int> selectedInputs;
    std::vector<int> selectedOutputs;
    int lastClickedInput  = -1;
    int lastClickedOutput = -1;
    // updateFxButtonAppearance lives in the public section now -- it's called
    // from MainComponent's snapshot-restore async callbacks.

    // Per-slot editor windows keyed by (direction, channel, slot).  A simple
    // 2D array per direction since both channel count and slot count are
    // small.
    std::vector<std::array<std::unique_ptr<juce::DocumentWindow>, 3>> outputEditorWindows;
    std::vector<std::array<std::unique_ptr<juce::DocumentWindow>, 3>> inputEditorWindows;

    juce::OwnedArray<juce::TextButton> inputFxBtns;

    // Per-row collapse / expand buttons.  inputCollapseBtns[n] is non-null
    // for two cases: (a) n is the first channel row of an expanded device
    // (shows "-" to collapse), or (b) n is a collapsed-device header row
    // (shows "+" to expand).  All other rows have nullptr.  Same for the
    // output (column) side.
    juce::OwnedArray<juce::TextButton> inputCollapseBtns;
    juce::OwnedArray<juce::TextButton> outputCollapseBtns;

    std::unique_ptr<juce::FileChooser> pluginFileChooser;
};

} // namespace dcr

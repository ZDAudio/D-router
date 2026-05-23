#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

#include "UI/CrosspointGrid.h"
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

    // Highlight a set of output column indices.  Called when a group card is
    // hovered.  Pass an empty list to clear.
    void setHighlightedOutputs (std::vector<int> outs);

    void paint (juce::Graphics&) override;
    void resized() override;

    // Layout constants
    static constexpr int cellSize       = 36;
    static constexpr int labelColWidth  = 280;
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

    // Scroll areas
    juce::Viewport gridViewport;
    juce::Viewport leftRailViewport;
    juce::Viewport topRailViewport;

    LeftRailContent leftRailContent { *this };
    TopRailContent  topRailContent  { *this };
    CornerCell      cornerCell      { *this };

    void openFxMenuFor (int outputCh);
    void loadPluginInto (int outputCh);
    void showEditorFor  (int outputCh);
    void closeEditorFor (int outputCh);
    void updateFxButtonAppearance (int outputCh);

    std::vector<std::unique_ptr<juce::DocumentWindow>> editorWindows;
    std::unique_ptr<juce::FileChooser> pluginFileChooser;
};

} // namespace dcr

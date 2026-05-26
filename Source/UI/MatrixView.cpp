#include "UI/MatrixView.h"

#include "Engine/AudioEngine.h"
#include "DSP/PluginHost.h"
#include "UI/PluginEditorWindow.h"
#include "Routing/RoutingMatrix.h"

#include <algorithm>
#include <cmath>

namespace dcr {

namespace
{
    // Channel-name label that surfaces a mouseDown callback.  Used in the
    // matrix rails to drive multi-track selection (shift / cmd modifiers).
    // We intentionally swallow the click so juce::Label's double-click
    // doesn't pop the rename editor.
    class SelectableLabel : public juce::Label
    {
    public:
        std::function<void (const juce::ModifierKeys&)> onMouseDown;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (onMouseDown) onMouseDown (e.mods);
        }
    };
}

namespace
{
    float dbToLin (float db) noexcept
    {
        return db <= -60.0f ? 0.0f : std::pow (10.0f, db * 0.05f);
    }
    float linToDb (float lin) noexcept
    {
        return lin <= 1.0e-6f ? -60.0f : 20.0f * std::log10 (lin);
    }
}

void MatrixView::LeftRailContent::paint (juce::Graphics& g)
{
    owner.paintLeftRail (g);
}

void MatrixView::LeftRailContent::mouseWheelMove (const juce::MouseEvent& e,
                                                  const juce::MouseWheelDetails& wheel)
{
    // Pump the wheel into the grid viewport.  We re-map the event so the
    // viewport sees a sensible mouse position inside its own coordinate
    // system -- not strictly needed for scrolling (juce::Viewport ignores
    // the position) but keeps any debug log truthful.
    auto remapped = e.getEventRelativeTo (&owner.gridViewport);
    owner.gridViewport.mouseWheelMove (remapped, wheel);
}

void MatrixView::LeftRailContent::mouseDown (const juce::MouseEvent&)
{
    // SelectableLabel children swallow their own clicks; this fires only
    // for gaps between widgets / right of the FX button.  Treat as
    // "clicked blank" -> clear the input selection.
    owner.clearChannelSelection (true);
}

void MatrixView::TopRailContent::paint (juce::Graphics& g)
{
    owner.paintTopRail (g);
}

void MatrixView::TopRailContent::mouseWheelMove (const juce::MouseEvent& e,
                                                 const juce::MouseWheelDetails& wheel)
{
    // On the OUTPUTS rail the natural axis is horizontal -- columns extend
    // left/right.  Re-map a vertical mouse wheel into horizontal scroll
    // so a regular scroll-wheel just works.  Trackpad horizontal swipes
    // already arrive with deltaX populated and stay as-is.
    juce::MouseWheelDetails w = wheel;
    if (std::abs (w.deltaX) < std::abs (w.deltaY))
    {
        w.deltaX = w.deltaY;
        w.deltaY = 0.0f;
    }
    auto remapped = e.getEventRelativeTo (&owner.gridViewport);
    owner.gridViewport.mouseWheelMove (remapped, w);
}

void MatrixView::CornerCell::mouseDown (const juce::MouseEvent&)
{
    // Top-left "neutral" cell -- click here to clear ALL selections.
    owner.clearChannelSelection (true);
    owner.clearChannelSelection (false);
}

void MatrixView::CornerCell::paint (juce::Graphics& g)
{
    owner.paintCornerCell (g);
}

MatrixView::MatrixView (AudioEngine& e) : engine (e)
{
    gridViewport.setScrollBarsShown (true, true);
    addAndMakeVisible (gridViewport);

    leftRailViewport.setScrollBarsShown (false, false);
    addAndMakeVisible (leftRailViewport);
    leftRailViewport.setViewedComponent (&leftRailContent, false);
    // Opaque rail content -- their paint() fills the full background.
    // Telling JUCE this means addAndMakeVisible for a child doesn't have
    // to invalidate the parent's region behind it (which would cost a
    // full-strip composite per child during rebuild).  Saves real time
    // when adding hundreds of channel widgets in a row.
    leftRailContent.setOpaque (true);

    topRailViewport.setScrollBarsShown (false, false);
    addAndMakeVisible (topRailViewport);
    topRailViewport.setViewedComponent (&topRailContent, false);
    topRailContent.setOpaque (true);

    addAndMakeVisible (cornerCell);

    // Register scrollbar listeners for synchronized scrolling (Excel-style freeze)
    gridViewport.getVerticalScrollBar().addListener (this);
    gridViewport.getHorizontalScrollBar().addListener (this);

    resumeUpdates();
}

MatrixView::~MatrixView()
{
    gridViewport.getVerticalScrollBar().removeListener (this);
    gridViewport.getHorizontalScrollBar().removeListener (this);
}

void MatrixView::resumeUpdates()
{
    const int hz = juce::jmax (1, engine.getSettings().meterTimerHz);
    startTimerHz (hz);
}

void MatrixView::setHighlightedOutputs (std::vector<int> outs)
{
    highlightedOutputs = std::move (outs);
    if (grid) grid->setHighlightedColumns (highlightedOutputs);
    topRailContent.repaint();
    repaint();
}

void MatrixView::setHighlightedInputs (std::vector<int> ins)
{
    highlightedInputs = std::move (ins);
    if (grid) grid->setHighlightedRows (highlightedInputs);
    leftRailContent.repaint();
    repaint();
}

juce::Slider* MatrixView::makeTrimSlider()
{
    auto* s = new juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag,
                                juce::Slider::NoTextBox);
    s->setRange (-60.0, 12.0, 0.1);
    s->setSkewFactorFromMidPoint (0.0);
    s->setValue (0.0, juce::dontSendNotification);
    s->setDoubleClickReturnValue (true, 0.0);
    s->setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromRGB (0, 255, 210));
    s->setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromRGB (40, 40, 48));
    s->setPopupDisplayEnabled (true, true, this);
    s->setTextValueSuffix (" dB");
    // Don't let scroll-wheel adjust the trim -- it eats the wheel events
    // that the user actually wants for scrolling the rail.  Drag still
    // adjusts; double-click resets.
    s->setScrollWheelEnabled (false);
    return s;
}

void MatrixView::rebuildFromEngine()
{
    // Channel counts may change.  Drop selection so it can't refer to a
    // channel that no longer exists -- a stale index would point at a
    // different device after device add/remove.
    selectedInputs.clear();
    selectedOutputs.clear();
    lastClickedInput  = -1;
    lastClickedOutput = -1;

    // FAST PATH: settings-only restart leaves the channel layout intact.
    // No reason to throw away 500+ widgets just to recreate identical ones --
    // just refresh their values from the engine and return.  Single-digit
    // milliseconds vs multiple seconds.
    if (! inputLabels.empty() && samePhysicalLayout())
    {
        softRefreshFromEngine();
        if (onRebuildFinished) onRebuildFinished();
        return;
    }

    clearAllChannelWidgets();
    buildLabelsFromEngine();

    const int nIn  = (int) inputLabels .size();
    const int nOut = (int) outputLabels.size();

    // Set size of rail content panels up-front so the viewport's scroll
    // ranges are right from the start; widgets fill into the canvas as
    // they get built asynchronously below.
    leftRailContent.setSize (labelColWidth, nIn * cellSize);
    topRailContent .setSize (nOut * cellSize, labelRowHeight);

    // HIDE the rail viewports + corner + grid viewport during the build.
    // Otherwise every addAndMakeVisible() queues a paint event for the
    // dirty rect of the new child; with ~600 widgets the backlog hogs
    // the message thread for seconds and starves the audio matrix thread
    // (which is why cold-start xruns spike to four-digit numbers and
    // why switching to a different tab "unfreezes" things -- JUCE drops
    // pending paints for invisible components).  The loading overlay is
    // an AlwaysOnTop sibling of MatrixView, so it stays visible
    // throughout.  Restored in finishRebuild().
    leftRailViewport.setVisible (false);
    topRailViewport .setVisible (false);
    gridViewport    .setVisible (false);
    cornerCell      .setVisible (false);

    // Async chunked build -- spreads ~600 component creations over many
    // message-thread ticks so the UI stays responsive on big device
    // configs (48 in + 66 out used to freeze the window for ~3 seconds).
    inputEditorWindows .resize ((size_t) nIn);
    outputEditorWindows.resize ((size_t) nOut);

    rebuildState.generation   = ++rebuildGenerationCounter;
    rebuildState.active       = true;
    rebuildState.nextInputIdx = 0;
    rebuildState.nextOutputIdx = 0;
    rebuildState.totalInputs  = nIn;
    rebuildState.totalOutputs = nOut;
    if (onRebuildProgress) onRebuildProgress (0, nIn + nOut);

    // Kick off the chunked build on the next tick so the current event
    // (mouse click etc.) finishes first.
    const auto gen = rebuildState.generation;
    juce::MessageManager::callAsync ([this, gen]
    {
        if (gen != rebuildState.generation) return;   // a newer rebuild has started
        continueRebuild();
    });
}

bool MatrixView::samePhysicalLayout() const
{
    int inIdx = 0, outIdx = 0;
    for (auto& d : engine.getDeviceInfo())
    {
        for (int c = 0; c < d.numInputChannels; ++c, ++inIdx)
        {
            if (inIdx >= (int) inputLabels.size())                return false;
            if (inputLabels[(size_t) inIdx].deviceName    != d.name) return false;
            if (inputLabels[(size_t) inIdx].channelIndex  != c + 1)  return false;
        }
        for (int c = 0; c < d.numOutputChannels; ++c, ++outIdx)
        {
            if (outIdx >= (int) outputLabels.size())               return false;
            if (outputLabels[(size_t) outIdx].deviceName    != d.name) return false;
            if (outputLabels[(size_t) outIdx].channelIndex  != c + 1)  return false;
        }
    }
    return inIdx == (int) inputLabels.size()
        && outIdx == (int) outputLabels.size();
}

void MatrixView::softRefreshFromEngine()
{
    auto& matrix = engine.getRoutingMatrix();
    const int nIn  = (int) inputTrims .size();
    const int nOut = (int) outputTrims.size();
    for (int n = 0; n < nIn; ++n)
    {
        if (n < inputTrims.size())
            inputTrims[n]->setValue (linToDb (matrix.getInputTrim (n)), juce::dontSendNotification);
        if (n < inputMuteBtns.size())
            inputMuteBtns[n]->setToggleState (matrix.getInputMute (n), juce::dontSendNotification);
        if (n < inputSoloBtns.size())
            inputSoloBtns[n]->setToggleState (matrix.getInputSolo (n), juce::dontSendNotification);
        updateFxButtonAppearance (true, n);
    }
    for (int m = 0; m < nOut; ++m)
    {
        if (m < outputTrims.size())
            outputTrims[m]->setValue (linToDb (matrix.getOutputTrim (m)), juce::dontSendNotification);
        if (m < outputMuteBtns.size())
            outputMuteBtns[m]->setToggleState (matrix.getOutputMute (m), juce::dontSendNotification);
        updateFxButtonAppearance (false, m);
    }
    if (grid != nullptr) grid->repaint();
}

void MatrixView::clearAllChannelWidgets()
{
    grid.reset();
    inputNames    .clear();
    inputTrims    .clear();
    inputMeters   .clear();
    inputMuteBtns .clear();
    inputSoloBtns .clear();
    inputFxBtns   .clear();
    outputTrims    .clear();
    outputMeters   .clear();
    outputMuteBtns .clear();
    outputFxBtns   .clear();
    // Close any open plugin editors -- engine restart invalidates instances.
    for (auto& row : outputEditorWindows) for (auto& w : row) if (w) w->setVisible (false);
    for (auto& row : inputEditorWindows)  for (auto& w : row) if (w) w->setVisible (false);
    outputEditorWindows.clear();
    inputEditorWindows.clear();
}

void MatrixView::buildLabelsFromEngine()
{
    inputLabels.clear();
    outputLabels.clear();
    for (auto& d : engine.getDeviceInfo())
    {
        for (int c = 0; c < d.numInputChannels; ++c)
            inputLabels.push_back ({ d.name, c + 1, c == 0 });
        for (int c = 0; c < d.numOutputChannels; ++c)
            outputLabels.push_back ({ d.name, c + 1, c == 0 });
    }
}

void MatrixView::continueRebuild()
{
    if (! rebuildState.active) return;

    // Bigger chunks now that each row sets its own bounds inline -- the
    // earlier 16-per-side cap existed because we were re-laying-out ALL
    // existing widgets every tick (O(N) work per tick).  Per-row layout
    // means the only per-tick cost is the new widget creation, so we
    // can comfortably do 64 per side.
    constexpr int kBatch = 64;

    int done = 0;
    while (done < kBatch && rebuildState.nextInputIdx < rebuildState.totalInputs)
    {
        buildInputRowWidgets (rebuildState.nextInputIdx);
        ++rebuildState.nextInputIdx;
        ++done;
    }
    done = 0;
    while (done < kBatch && rebuildState.nextOutputIdx < rebuildState.totalOutputs)
    {
        buildOutputColumnWidgets (rebuildState.nextOutputIdx);
        ++rebuildState.nextOutputIdx;
        ++done;
    }

    if (onRebuildProgress)
        onRebuildProgress (rebuildState.nextInputIdx + rebuildState.nextOutputIdx,
                           rebuildState.totalInputs + rebuildState.totalOutputs);

    const bool moreInputs  = rebuildState.nextInputIdx  < rebuildState.totalInputs;
    const bool moreOutputs = rebuildState.nextOutputIdx < rebuildState.totalOutputs;
    if (moreInputs || moreOutputs)
    {
        const auto gen = rebuildState.generation;
        juce::MessageManager::callAsync ([this, gen]
        {
            if (gen != rebuildState.generation) return;
            continueRebuild();
        });
    }
    else
    {
        finishRebuild();
    }
}

void MatrixView::finishRebuild()
{
    auto& matrix = engine.getRoutingMatrix();
    grid = std::make_unique<CrosspointGrid> (matrix);
    grid->setDimensions (rebuildState.totalInputs, rebuildState.totalOutputs, cellSize);
    gridViewport.setViewedComponent (grid.get(), false);

    // Device boundary lines (same start-of-group flag we recorded when
    // building inputLabels / outputLabels).
    std::vector<int> inBounds;
    for (int i = 1; i < (int) inputLabels.size(); ++i)
        if (inputLabels[(size_t) i].startsNewGroup) inBounds.push_back (i);
    std::vector<int> outBounds;
    for (int j = 1; j < (int) outputLabels.size(); ++j)
        if (outputLabels[(size_t) j].startsNewGroup) outBounds.push_back (j);
    grid->setDeviceBoundaries (inBounds, outBounds);

    // Restore visibility of the rails/grid we hid during the chunked build.
    // Doing this AFTER all the addAndMakeVisible() calls inside the chunks
    // means JUCE only has to paint the final tree once, instead of after
    // every newly-added widget.  Massively cuts message-thread paint work
    // during the rebuild and stops the audio thread from getting starved.
    leftRailViewport.setVisible (true);
    topRailViewport .setVisible (true);
    gridViewport    .setVisible (true);
    cornerCell      .setVisible (true);

    resized();
    repaint();

    rebuildState.active = false;
    if (onRebuildFinished) onRebuildFinished();
}

void MatrixView::buildInputRowWidgets (int n)
{
    if (n < 0 || n >= (int) inputLabels.size()) return;
    auto& matrix = engine.getRoutingMatrix();
    {
        auto* lbl = new SelectableLabel();
        lbl->setText (inputLabels[(size_t) n].deviceName + "  ch."
                          + juce::String (inputLabels[(size_t) n].channelIndex),
                      juce::dontSendNotification);
        lbl->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
        lbl->setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        lbl->setJustificationType (juce::Justification::centredLeft);
        lbl->setTooltip ("Click to select.  Shift-click to extend, Cmd-click to toggle.  "
                         "While multiple inputs are selected, FX popup actions apply to all of them.");
        lbl->onMouseDown = [this, n] (const juce::ModifierKeys& mods)
        {
            handleChannelHeaderClick (true, n, mods);
        };
        leftRailContent.addAndMakeVisible (*lbl);
        inputNames.add (lbl);

        auto* sl = makeTrimSlider();
        sl->setValue (linToDb (matrix.getInputTrim (n)), juce::dontSendNotification);
        sl->onValueChange = [this, &matrix, sl, n]
        {
            const float dbVal  = (float) sl->getValue();
            const float linVal = dbToLin (dbVal);
            matrix.setInputTrim (n, linVal);
            // Multi-select linking: same trim on every other selected input.
            if (isChannelSelected (true, n))
            {
                auto sel = getSelectedChannels (true);
                if (sel.size() > 1)
                {
                    for (int other : sel)
                    {
                        if (other == n) continue;
                        if (other >= 0 && other < inputTrims.size())
                        {
                            inputTrims[other]->setValue (dbVal, juce::dontSendNotification);
                            matrix.setInputTrim (other, linVal);
                        }
                    }
                }
            }
        };
        leftRailContent.addAndMakeVisible (*sl);
        inputTrims.add (sl);

        auto* m = new LevelMeter (LevelMeter::Orientation::Horizontal);
        leftRailContent.addAndMakeVisible (*m);
        inputMeters.add (m);

        auto* mute = new juce::TextButton ("M");
        mute->setName ("mute");
        mute->setClickingTogglesState (true);
        mute->setToggleState (matrix.getInputMute (n), juce::dontSendNotification);
        mute->onClick = [this, &matrix, mute, n]
        {
            const bool on = mute->getToggleState();
            matrix.setInputMute (n, on);
            // Link to other selected inputs.
            if (isChannelSelected (true, n))
            {
                auto sel = getSelectedChannels (true);
                if (sel.size() > 1)
                {
                    for (int other : sel)
                    {
                        if (other == n) continue;
                        if (other >= 0 && other < inputMuteBtns.size())
                        {
                            inputMuteBtns[other]->setToggleState (on, juce::dontSendNotification);
                            matrix.setInputMute (other, on);
                        }
                    }
                }
            }
            if (onUserMuteChanged) onUserMuteChanged();
        };
        leftRailContent.addAndMakeVisible (*mute);
        inputMuteBtns.add (mute);

        auto* solo = new juce::TextButton ("S");
        solo->setName ("solo");
        solo->setClickingTogglesState (true);
        solo->setToggleState (matrix.getInputSolo (n), juce::dontSendNotification);
        solo->onClick = [this, &matrix, solo, n]
        {
            const bool on = solo->getToggleState();
            matrix.setInputSolo (n, on);
            if (isChannelSelected (true, n))
            {
                auto sel = getSelectedChannels (true);
                if (sel.size() > 1)
                {
                    for (int other : sel)
                    {
                        if (other == n) continue;
                        if (other >= 0 && other < inputSoloBtns.size())
                        {
                            inputSoloBtns[other]->setToggleState (on, juce::dontSendNotification);
                            matrix.setInputSolo (other, on);
                        }
                    }
                }
            }
        };
        leftRailContent.addAndMakeVisible (*solo);
        inputSoloBtns.add (solo);

        auto* infx = new juce::TextButton ("FX");
        infx->setName ("fx");
        infx->onClick = [this, n] { openFxMenuFor (/*isInput=*/true, n); };
        leftRailContent.addAndMakeVisible (*infx);
        inputFxBtns.add (infx);
    }
    updateFxButtonAppearance (true, n);

    // Set THIS row's widget bounds directly -- avoids the O(N) global
    // layout pass that used to fire after every chunk.  Matches the
    // geometry in layoutLeftRail() exactly.
    const int yy = n * cellSize;
    inputNames   .getLast()->setBounds (8,   yy + 2,  96,  cellSize - 4);
    inputTrims   .getLast()->setBounds (106, yy + 2,  32,  32);
    inputMeters  .getLast()->setBounds (142, yy + 12, 84,  12);
    inputMuteBtns.getLast()->setBounds (230, yy + 7,  18,  22);
    inputSoloBtns.getLast()->setBounds (250, yy + 7,  18,  22);
    inputFxBtns  .getLast()->setBounds (272, yy + 7,  30,  22);
}

void MatrixView::buildOutputColumnWidgets (int m)
{
    if (m < 0 || m >= (int) outputLabels.size()) return;
    auto& matrix = engine.getRoutingMatrix();
    {
        auto* sl = makeTrimSlider();
        sl->setValue (linToDb (matrix.getOutputTrim (m)), juce::dontSendNotification);
        sl->onValueChange = [this, &matrix, sl, m]
        {
            const float dbVal  = (float) sl->getValue();
            const float linVal = dbToLin (dbVal);
            matrix.setOutputTrim (m, linVal);
            if (isChannelSelected (false, m))
            {
                auto sel = getSelectedChannels (false);
                if (sel.size() > 1)
                {
                    for (int other : sel)
                    {
                        if (other == m) continue;
                        if (other >= 0 && other < outputTrims.size())
                        {
                            outputTrims[other]->setValue (dbVal, juce::dontSendNotification);
                            matrix.setOutputTrim (other, linVal);
                        }
                    }
                }
            }
        };
        topRailContent.addAndMakeVisible (*sl);
        outputTrims.add (sl);

        auto* met = new LevelMeter (LevelMeter::Orientation::Vertical);
        topRailContent.addAndMakeVisible (*met);
        outputMeters.add (met);

        auto* mute = new juce::TextButton ("M");
        mute->setName ("mute");
        mute->setClickingTogglesState (true);
        mute->setToggleState (matrix.getOutputMute (m), juce::dontSendNotification);
        mute->onClick = [this, &matrix, mute, m]
        {
            const bool on = mute->getToggleState();
            matrix.setOutputMute (m, on);
            if (isChannelSelected (false, m))
            {
                auto sel = getSelectedChannels (false);
                if (sel.size() > 1)
                {
                    for (int other : sel)
                    {
                        if (other == m) continue;
                        if (other >= 0 && other < outputMuteBtns.size())
                        {
                            outputMuteBtns[other]->setToggleState (on, juce::dontSendNotification);
                            matrix.setOutputMute (other, on);
                        }
                    }
                }
            }
            if (onUserMuteChanged) onUserMuteChanged();
        };
        topRailContent.addAndMakeVisible (*mute);
        outputMuteBtns.add (mute);

        auto* fx = new juce::TextButton ("FX");
        fx->setName ("fx");
        fx->onClick = [this, m] { openFxMenuFor (/*isInput=*/false, m); };
        topRailContent.addAndMakeVisible (*fx);
        outputFxBtns.add (fx);
    }
    updateFxButtonAppearance (false, m);

    // Set THIS column's widget bounds directly.  Matches layoutTopRail().
    const int xx = m * cellSize;
    outputTrims   .getLast()->setBounds (xx + 2,  60,  32, 32);
    outputMeters  .getLast()->setBounds (xx + 11, 96,  14, 42);
    outputMuteBtns.getLast()->setBounds (xx + 9,  140, 18, 16);
    outputFxBtns  .getLast()->setBounds (xx + 5,  160, 26, 18);
}

void MatrixView::layoutLeftRail()
{
    for (int n = 0; n < (int) inputNames.size(); ++n)
    {
        const int yy = n * cellSize;
        inputNames   [n]->setBounds (8,   yy + 2,  96,  cellSize - 4);
        inputTrims   [n]->setBounds (106, yy + 2,  32,  32);
        inputMeters  [n]->setBounds (142, yy + 12, 84,  12);
        inputMuteBtns[n]->setBounds (230, yy + 7,  18,  22);
        inputSoloBtns[n]->setBounds (250, yy + 7,  18,  22);
        if (n < (int) inputFxBtns.size())
            inputFxBtns[n]->setBounds (272, yy + 7, 30, 22);
    }
}

void MatrixView::layoutTopRail()
{
    for (int m = 0; m < (int) outputTrims.size(); ++m)
    {
        const int xx = m * cellSize;
        outputTrims   [m]->setBounds (xx + 2,  60,  32, 32);
        outputMeters  [m]->setBounds (xx + 11, 96,  14, 42);
        outputMuteBtns[m]->setBounds (xx + 9,  140, 18, 16);
        outputFxBtns  [m]->setBounds (xx + 5,  160, 26, 18);
    }
}

void MatrixView::resized()
{
    cornerCell.setBounds (0, 0, labelColWidth, labelRowHeight);
    topRailViewport.setBounds (labelColWidth, 0, getWidth() - labelColWidth, labelRowHeight);
    leftRailViewport.setBounds (0, labelRowHeight, labelColWidth, getHeight() - labelRowHeight);
    gridViewport.setBounds (labelColWidth, labelRowHeight, getWidth() - labelColWidth, getHeight() - labelRowHeight);

    layoutLeftRail();
    layoutTopRail();
}

void MatrixView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (12, 12, 14)); // Dark background
}

void MatrixView::paintLeftRail (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (12, 12, 14));

    // Only paint separators that fall inside the visible clip rectangle.
    const auto clip = g.getClipBounds();
    const int nStart = juce::jmax (0, clip.getY() / cellSize);
    const int nEnd   = juce::jmin ((int) inputLabels.size(),
                                   (clip.getBottom() + cellSize - 1) / cellSize);

    // Hover overlay when an INPUT group is hovered in the bottom panel.
    if (! highlightedInputs.empty())
    {
        g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.18f));
        for (int n : highlightedInputs)
            if (n >= nStart && n < nEnd)
                g.fillRect (0, n * cellSize, leftRailContent.getWidth(), cellSize);
    }

    // Multi-select highlight (stronger than the group hover so the user
    // sees their selection at a glance) -- amber band.
    if (! selectedInputs.empty())
    {
        g.setColour (juce::Colour::fromRGB (255, 180, 60).withAlpha (0.28f));
        for (int n : selectedInputs)
            if (n >= nStart && n < nEnd)
                g.fillRect (0, n * cellSize, leftRailContent.getWidth(), cellSize);
    }

    g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.25f));
    for (int n = nStart; n < nEnd; ++n)
    {
        if (n > 0 && inputLabels[(size_t) n].startsNewGroup)
        {
            const float y = (float) n * (float) cellSize;
            g.drawLine (0.0f, y, (float) leftRailContent.getWidth(), y, 1.0f);
        }
    }
}

void MatrixView::paintTopRail (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (12, 12, 14));

    // Visible column range from clip rectangle.  The rail used to redraw
    // every column on every scroll frame, which was the dominant cost.
    const auto clip = g.getClipBounds();
    const int mStart = juce::jmax (0, clip.getX() / cellSize);
    // Extend right edge a bit so rotated labels that begin off-screen still draw.
    const int mEnd   = juce::jmin ((int) outputLabels.size(),
                                   (clip.getRight() + 240) / cellSize);

    // Hover overlay
    if (! highlightedOutputs.empty())
    {
        g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.08f));
        for (int m : highlightedOutputs)
            if (m >= mStart && m < mEnd)
                g.fillRect (m * cellSize, 0, cellSize, labelRowHeight);
    }

    // Multi-select highlight on outputs -- same amber band as inputs.
    if (! selectedOutputs.empty())
    {
        g.setColour (juce::Colour::fromRGB (255, 180, 60).withAlpha (0.28f));
        for (int m : selectedOutputs)
            if (m >= mStart && m < mEnd)
                g.fillRect (m * cellSize, 0, cellSize, labelRowHeight);
    }

    // Device separation lines
    g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.25f));
    for (int m = mStart; m < mEnd; ++m)
    {
        if (m > 0 && outputLabels[(size_t) m].startsNewGroup)
        {
            const float x = (float) m * (float) cellSize;
            g.drawLine (x, 0.0f, x, (float) labelRowHeight, 1.0f);
        }
    }

    // Rotated column names
    g.setColour (juce::Colour::fromRGB (160, 160, 165));
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
    for (int m = mStart; m < mEnd; ++m)
    {
        const int xx = m * cellSize;
        const auto& lbl = outputLabels[(size_t) m];
        const juce::String text = lbl.deviceName + " ch." + juce::String (lbl.channelIndex);

        juce::Graphics::ScopedSaveState state (g);
        const float cx = (float) xx + cellSize * 0.5f;
        const float cy = 52.0f;
        g.addTransform (juce::AffineTransform::rotation (-0.6f, cx, cy));
        g.drawText (text,
                    (int) cx, (int) cy - 8,
                    220, 16,
                    juce::Justification::centredLeft, true);
    }
}

void MatrixView::paintCornerCell (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (16, 16, 20));
    
    // Diagonal divide line
    g.setColour (juce::Colour::fromRGB (40, 40, 48));
    g.drawLine (0.0f, 0.0f, (float) labelColWidth, (float) labelRowHeight, 1.0f);
    
    // Corner Text
    g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.0f, juce::Font::bold));
    g.setColour (juce::Colour::fromRGB (0, 255, 210));
    g.drawText ("OUTPUTS", labelColWidth - 100, 16, 92, 16, juce::Justification::topRight, true);
    
    g.setColour (juce::Colour::fromRGB (180, 180, 185));
    g.drawText ("INPUTS", 12, labelRowHeight - 28, 92, 16, juce::Justification::bottomLeft, true);
}

void MatrixView::scrollBarMoved (juce::ScrollBar* scrollBar, double newRangeStart)
{
    if (scrollBar == &gridViewport.getVerticalScrollBar())
    {
        leftRailViewport.setViewPosition (0, (int) newRangeStart);
    }
    else if (scrollBar == &gridViewport.getHorizontalScrollBar())
    {
        topRailViewport.setViewPosition ((int) newRangeStart, 0);
    }
}

void MatrixView::timerCallback()
{
    const float decay = engine.getSettings().meterDecayFactor;
    const int nIn  = (int) inputMeters .size();
    const int nOut = (int) outputMeters.size();
    for (int n = 0; n < nIn;  ++n)
    {
        inputMeters[n]->pushPeak  (engine.getInputPeak  (n));
        inputMeters[n]->tickDecay (decay);
    }
    for (int m = 0; m < nOut; ++m)
    {
        outputMeters[m]->pushPeak  (engine.getOutputPeak (m));
        outputMeters[m]->tickDecay (decay);
    }
}

static PluginHost* getHost (AudioEngine& e, bool isInput, int ch)
{
    return isInput ? e.getInputPluginHost (ch) : e.getPluginHost (ch);
}

void MatrixView::updateFxButtonAppearance (bool isInput, int ch)
{
    auto& btns = isInput ? inputFxBtns : outputFxBtns;
    if (ch < 0 || ch >= btns.size()) return;
    auto* host = getHost (engine, isInput, ch);
    auto* btn  = btns[ch];

    // Color states (user-specified):
    //  - no plugins        : default dark grey (unchanged look)
    //  - any working plugin: cyan
    //  - plugins loaded but every one bypassed: dim red
    juce::Colour col (50, 50, 56);
    if (host && host->anyLoaded())
        col = host->anyActive() ? juce::Colour::fromRGB (0,  170, 200)
                                : juce::Colour::fromRGB (90, 28,  28);
    btn->setColour (juce::TextButton::buttonColourId,   col);
    btn->setColour (juce::TextButton::buttonOnColourId, col);
    btn->setButtonText ("FX");
}

void MatrixView::openFxMenuFor (bool isInput, int ch)
{
    auto* host = getHost (engine, isInput, ch);
    if (host == nullptr) return;

    auto& btns = isInput ? inputFxBtns : outputFxBtns;
    if (ch < 0 || ch >= btns.size()) return;

    // Build the broadcast target list.  If the clicked channel is part of
    // a multi-channel selection on its side, every operation in the popup
    // applies to all of them.  Otherwise just the clicked channel.
    std::vector<int> targets;
    auto sel = getSelectedChannels (isInput);
    if (sel.size() > 1
        && std::find (sel.begin(), sel.end(), ch) != sel.end())
        targets = std::move (sel);
    else
        targets = { ch };

    auto popup = std::make_unique<FxChainPopupContent> (*this, isInput, ch, std::move (targets));
    auto bounds = btns[ch]->getScreenBounds();
    juce::CallOutBox::launchAsynchronously (std::move (popup), bounds, nullptr);
}

// ============================================================================
// Channel-header multi-select
// ============================================================================
void MatrixView::handleChannelHeaderClick (bool isInput, int ch, const juce::ModifierKeys& mods)
{
    auto& sel  = isInput ? selectedInputs    : selectedOutputs;
    auto& last = isInput ? lastClickedInput  : lastClickedOutput;
    const int total = isInput ? (int) inputLabels.size() : (int) outputLabels.size();
    if (ch < 0 || ch >= total) return;

    if (mods.isShiftDown() && last >= 0 && last < total)
    {
        // Range: replace selection with [min(last,ch) .. max(last,ch)].
        sel.clear();
        const int lo = juce::jmin (last, ch);
        const int hi = juce::jmax (last, ch);
        for (int i = lo; i <= hi; ++i) sel.push_back (i);
    }
    else if (mods.isCommandDown())
    {
        // Toggle membership; don't clear the rest.
        auto it = std::find (sel.begin(), sel.end(), ch);
        if (it != sel.end()) sel.erase (it);
        else                 sel.push_back (ch);
        last = ch;
    }
    else
    {
        // Plain click: select just this channel.
        sel = { ch };
        last = ch;
    }

    if (isInput) leftRailContent.repaint();
    else         topRailContent .repaint();
}

std::vector<int> MatrixView::getSelectedChannels (bool isInput) const
{
    return isInput ? selectedInputs : selectedOutputs;
}

void MatrixView::clearChannelSelection (bool isInput)
{
    if (isInput) { selectedInputs.clear();  lastClickedInput  = -1; leftRailContent.repaint(); }
    else         { selectedOutputs.clear(); lastClickedOutput = -1; topRailContent .repaint(); }
}

bool MatrixView::isChannelSelected (bool isInput, int ch) const
{
    const auto& sel = isInput ? selectedInputs : selectedOutputs;
    return std::find (sel.begin(), sel.end(), ch) != sel.end();
}

void MatrixView::TopRailContent::mouseDown (const juce::MouseEvent& e)
{
    // Only the rotated-label band (above the trim/meter row) triggers
    // selection; the widget rows below pass clicks through to their own
    // children (which never delegate to us in the first place).
    constexpr int labelBandHeight = 55;
    if (e.y < labelBandHeight)
    {
        const int col = e.x / owner.cellSize;
        if (col >= 0 && col < (int) owner.outputLabels.size())
        {
            owner.handleChannelHeaderClick (false, col, e.mods);
            return;
        }
    }
    // Blank click (below labels, or past the last column) -> clear outputs.
    owner.clearChannelSelection (false);
}

// ============================================================================
// FxChainPopupContent (small 3-row B / name / X plugin chain panel)
// ============================================================================
MatrixView::FxChainPopupContent::FxChainPopupContent (MatrixView& o, bool inp, int primaryCh,
                                                      std::vector<int> targetChannels)
    : owner (o), isInput (inp), ch (primaryCh), targets (std::move (targetChannels))
{
    header.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold));
    header.setColour (juce::Label::textColourId, juce::Colour::fromRGB (0, 200, 220));
    {
        juce::String hdrText = juce::String (isInput ? "INPUT" : "OUTPUT")
                             + " ch." + juce::String (ch + 1) + "  FX chain";
        if (targets.size() > 1)
            hdrText << "   (broadcast to " << juce::String ((int) targets.size()) << " channels)";
        header.setText (hdrText, juce::dontSendNotification);
        if (targets.size() > 1)
            header.setColour (juce::Label::textColourId, juce::Colour::fromRGB (255, 180, 60));
    }
    addAndMakeVisible (header);

    for (int s = 0; s < (int) rows.size(); ++s)
    {
        auto& row = rows[(size_t) s];
        row.bypass.setName ("bypass");
        row.bypass.setClickingTogglesState (true);
        row.bypass.setTooltip (targets.size() > 1
                               ? "Bypass this slot on ALL selected channels"
                               : "Bypass this slot");
        row.bypass.onClick = [this, s] { onSlotBypassClicked (s); };

        row.name.setName ("slot");
        row.name.setTooltip (targets.size() > 1
                             ? "Click: load (broadcast to all selected) / open editor on primary.  Drag: reorder (broadcast)."
                             : "Click: load / open editor.  Drag: reorder.");
        row.name.setButtonText ("+ insert");
        row.name.slotIdx = s;
        row.name.onClick = [this, s] { onSlotNameClicked (s); };
        row.name.onSwap  = [this] (int from, int to)
        {
            // Broadcast the swap to every target channel so an N-way drag
            // reorder keeps the chains in lock-step.
            for (int targetCh : targets)
                if (auto* h = getHost (owner.engine, isInput, targetCh))
                    h->swapSlots (from, to);
            for (int targetCh : targets)
                owner.updateFxButtonAppearance (isInput, targetCh);
            refreshAll();
        };

        row.remove.setName ("remove");
        row.remove.setTooltip (targets.size() > 1
                               ? "Remove plugin from this slot on ALL selected channels"
                               : "Remove plugin");
        row.remove.onClick = [this, s] { onSlotRemoveClicked (s); };

        addAndMakeVisible (row.bypass);
        addAndMakeVisible (row.name);
        addAndMakeVisible (row.remove);
    }

    refreshAll();
    setSize (240, 16 + 3 * 24 + 6);
    startTimer (150);   // poll so async plugin loads reflect in the popup
}

MatrixView::FxChainPopupContent::~FxChainPopupContent() { stopTimer(); }

void MatrixView::FxChainPopupContent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (20, 20, 24));
    g.setColour (juce::Colour::fromRGB (40, 40, 48));
    g.drawRect (getLocalBounds(), 1);
}

void MatrixView::FxChainPopupContent::resized()
{
    auto r = getLocalBounds().reduced (4);
    header.setBounds (r.removeFromTop (14));
    r.removeFromTop (2);
    constexpr int rowH = 22;
    for (auto& row : rows)
    {
        auto rr = r.removeFromTop (rowH);
        row.bypass.setBounds (rr.removeFromLeft (20));
        rr.removeFromLeft (2);
        row.remove.setBounds (rr.removeFromRight (20));
        rr.removeFromRight (2);
        row.name  .setBounds (rr);
        r.removeFromTop (2);
    }
}

void MatrixView::FxChainPopupContent::timerCallback() { refreshAll(); }

void MatrixView::FxChainPopupContent::refreshAll()
{
    auto* host = getHost (owner.engine, isInput, ch);
    for (int s = 0; s < (int) rows.size(); ++s)
    {
        auto& row = rows[(size_t) s];
        auto* p = host ? host->getPluginAt (s) : nullptr;
        const bool loaded = (p != nullptr);
        if (loaded)
        {
            const float cpu = host->getCpuLoadAt (s) * 100.0f;
            const juce::String txt = p->getName() + "   " + juce::String (cpu, 1) + "%";
            if (row.name.getButtonText() != txt) row.name.setButtonText (txt);
            row.bypass.setEnabled (true);
            row.remove.setEnabled (true);
            row.bypass.setToggleState (host->isBypassedAt (s), juce::dontSendNotification);
            row.name.setToggleState (host->isBypassedAt (s), juce::dontSendNotification);
        }
        else
        {
            if (row.name.getButtonText() != "+ insert") row.name.setButtonText ("+ insert");
            row.bypass.setToggleState (false, juce::dontSendNotification);
            row.bypass.setEnabled (false);
            row.remove.setEnabled (false);
            row.name.setToggleState (false, juce::dontSendNotification);
        }
    }
}

void MatrixView::FxChainPopupContent::onSlotNameClicked (int slotIdx)
{
    auto* primary = getHost (owner.engine, isInput, ch);
    if (primary == nullptr) return;
    if (primary->getPluginAt (slotIdx) != nullptr)
    {
        // Open editor only on the primary -- broadcast-opening N windows
        // would be obnoxious and most plugins don't expect parallel editor
        // instances on N processor instances anyway.
        owner.showEditorFor (isInput, ch, slotIdx);
    }
    else
    {
        // Load -- broadcast to every selected channel.  loadPluginIntoMulti
        // shows the file chooser once and instantiates the chosen plugin
        // into the same slot on each channel.
        owner.loadPluginIntoMulti (isInput, targets, slotIdx);
    }
}

void MatrixView::FxChainPopupContent::onSlotBypassClicked (int slotIdx)
{
    const bool on = rows[(size_t) slotIdx].bypass.getToggleState();
    for (int targetCh : targets)
    {
        if (auto* h = getHost (owner.engine, isInput, targetCh))
            h->setBypassedAt (slotIdx, on);
        owner.updateFxButtonAppearance (isInput, targetCh);
    }
    refreshAll();
}

void MatrixView::FxChainPopupContent::onSlotRemoveClicked (int slotIdx)
{
    for (int targetCh : targets)
    {
        owner.closeEditorFor (isInput, targetCh, slotIdx);
        if (auto* h = getHost (owner.engine, targetCh == ch ? isInput : isInput, targetCh))
            h->clearSlot (slotIdx);
        owner.updateFxButtonAppearance (isInput, targetCh);
    }
    refreshAll();
}

void MatrixView::closeAllPluginEditors()
{
    for (auto& row : outputEditorWindows) for (auto& w : row) w.reset();
    for (auto& row : inputEditorWindows)  for (auto& w : row) w.reset();
}

void MatrixView::refreshMuteButtonStates()
{
    auto& m = engine.getRoutingMatrix();
    for (int n = 0; n < inputMuteBtns.size()  && n < m.getNumInputs();  ++n)
        inputMuteBtns[n] ->setToggleState (m.getInputMute (n),  juce::dontSendNotification);
    for (int o = 0; o < outputMuteBtns.size() && o < m.getNumOutputs(); ++o)
        outputMuteBtns[o]->setToggleState (m.getOutputMute (o), juce::dontSendNotification);
}

void MatrixView::loadPluginInto (bool isInput, int ch, int slotIdx)
{
    juce::File startDir ("/Library/Audio/Plug-Ins/Components");
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/Audio/Plug-Ins/Components");

    pluginFileChooser = std::make_unique<juce::FileChooser> (
        "Choose an AU plugin (.component)",
        startDir,
        "*.component;*.audiounit");

    pluginFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode
      | juce::FileBrowserComponent::canSelectFiles
      | juce::FileBrowserComponent::canSelectDirectories,
        [this, isInput, ch, slotIdx] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;

            auto& fmt = *engine.getPluginFormatManager().getFormat (0);
            juce::OwnedArray<juce::PluginDescription> descs;
            fmt.findAllTypesForFile (descs, file.getFullPathName());

            if (descs.isEmpty())
            {
                juce::NativeMessageBox::showAsync (
                    juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                        .withTitle ("No AU plugin found")
                        .withMessage (file.getFileName() + " is not a loadable Audio Unit."),
                    nullptr);
                return;
            }

            auto chooseDesc = [this, isInput, ch, slotIdx] (juce::PluginDescription desc)
            {
                engine.getPluginFormatManager().createPluginInstanceAsync (
                    desc, engine.getEngineSampleRate(), engine.getEngineBlockSize(),
                    [this, isInput, ch, slotIdx] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                   const juce::String& error)
                    {
                        if (instance == nullptr)
                        {
                            juce::NativeMessageBox::showAsync (
                                juce::MessageBoxOptions()
                                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                                    .withTitle ("Plugin load failed")
                                    .withMessage (error),
                                nullptr);
                            return;
                        }
                        auto* host = getHost (engine, isInput, ch);
                        if (host == nullptr) return;
                        closeEditorFor (isInput, ch, slotIdx);
                        host->setPluginAt (slotIdx, std::move (instance));
                        updateFxButtonAppearance (isInput, ch);
                    });
            };

            if (descs.size() == 1)
            {
                chooseDesc (*descs[0]);
            }
            else
            {
                juce::PopupMenu pick;
                std::vector<juce::PluginDescription> copies;
                copies.reserve ((size_t) descs.size());
                for (auto* d : descs) copies.push_back (*d);
                for (size_t i = 0; i < copies.size(); ++i)
                {
                    auto d = copies[i];
                    pick.addItem (d.name + "  -  " + d.manufacturerName,
                                  [d, chooseDesc] { chooseDesc (d); });
                }
                auto& btns = isInput ? inputFxBtns : outputFxBtns;
                pick.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (btns[ch]));
            }
        });
}

void MatrixView::loadPluginIntoMulti (bool isInput, std::vector<int> channels, int slotIdx)
{
    if (channels.empty()) return;

    juce::File startDir ("/Library/Audio/Plug-Ins/Components");
    if (! startDir.isDirectory())
        startDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                       .getChildFile ("Library/Audio/Plug-Ins/Components");

    pluginFileChooser = std::make_unique<juce::FileChooser> (
        "Choose an AU plugin (.component)  -  loads into " + juce::String ((int) channels.size())
            + " channel(s)",
        startDir,
        "*.component;*.audiounit");

    pluginFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode
      | juce::FileBrowserComponent::canSelectFiles
      | juce::FileBrowserComponent::canSelectDirectories,
        [this, isInput, channels, slotIdx] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;

            auto& fmt = *engine.getPluginFormatManager().getFormat (0);
            juce::OwnedArray<juce::PluginDescription> descs;
            fmt.findAllTypesForFile (descs, file.getFullPathName());

            if (descs.isEmpty())
            {
                juce::NativeMessageBox::showAsync (
                    juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                        .withTitle ("No AU plugin found")
                        .withMessage (file.getFileName() + " is not a loadable Audio Unit."),
                    nullptr);
                return;
            }

            // For each target channel we fire a separate createPluginInstanceAsync
            // -- AU instances are per-host, can't be shared.  All callbacks land
            // on the message thread; whichever finishes first installs first.
            auto chooseDesc = [this, isInput, channels, slotIdx] (juce::PluginDescription desc)
            {
                juce::Logger::writeToLog ("loadPluginIntoMulti: broadcasting '" + desc.name
                                          + "' to " + juce::String ((int) channels.size())
                                          + " " + juce::String (isInput ? "input" : "output")
                                          + " channels, slot " + juce::String (slotIdx + 1));
                for (int targetCh : channels)
                {
                    engine.getPluginFormatManager().createPluginInstanceAsync (
                        desc, engine.getEngineSampleRate(), engine.getEngineBlockSize(),
                        [this, isInput, targetCh, slotIdx]
                        (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err)
                        {
                            if (instance == nullptr)
                            {
                                juce::Logger::writeToLog ("  ch." + juce::String (targetCh + 1)
                                                          + " load failed: " + err);
                                return;
                            }
                            auto* host = getHost (engine, isInput, targetCh);
                            if (host == nullptr) return;
                            // Replace whatever was there -- the whole point of
                            // broadcast is "make all selected the same".
                            closeEditorFor (isInput, targetCh, slotIdx);
                            host->setPluginAt (slotIdx, std::move (instance));
                            updateFxButtonAppearance (isInput, targetCh);
                        });
                }
            };

            if (descs.size() == 1)
            {
                chooseDesc (*descs[0]);
            }
            else
            {
                juce::PopupMenu pick;
                std::vector<juce::PluginDescription> copies;
                copies.reserve ((size_t) descs.size());
                for (auto* d : descs) copies.push_back (*d);
                for (size_t i = 0; i < copies.size(); ++i)
                {
                    auto d = copies[i];
                    pick.addItem (d.name + "  -  " + d.manufacturerName,
                                  [d, chooseDesc] { chooseDesc (d); });
                }
                auto& btns = isInput ? inputFxBtns : outputFxBtns;
                const int primary = channels.front();
                if (primary >= 0 && primary < btns.size())
                    pick.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (btns[primary]));
                else
                    pick.showMenuAsync (juce::PopupMenu::Options());
            }
        });
}

void MatrixView::showEditorFor (bool isInput, int ch, int slotIdx)
{
    auto& wins = isInput ? inputEditorWindows : outputEditorWindows;
    if (ch < 0 || ch >= (int) wins.size()) return;
    if (slotIdx < 0 || slotIdx >= PluginHost::kNumSlots) return;

    auto* host = getHost (engine, isInput, ch);
    if (host == nullptr) return;
    auto* plugin = host->getPluginAt (slotIdx);
    if (plugin == nullptr) return;

    if (wins[(size_t) ch][(size_t) slotIdx])
    {
        wins[(size_t) ch][(size_t) slotIdx]->toFront (true);
        return;
    }

    // Build a context label like "INPUT BlackHole 2ch ch.1 / slot 2".
    juce::String ctx;
    {
        const auto& labels = isInput ? inputLabels : outputLabels;
        if (ch >= 0 && ch < (int) labels.size())
        {
            ctx << (isInput ? "INPUT " : "OUTPUT ");
            ctx << labels[(size_t) ch].deviceName;
            ctx << " ch." << juce::String (labels[(size_t) ch].channelIndex);
            ctx << "  /  slot " << juce::String (slotIdx + 1);
        }
    }
    // Multi-select linking: if this channel is part of a >1 selection,
    // collect every other selected channel's same-slot plugin instance
    // and hand them to the editor window.  PluginEditorWindow installs
    // an AudioProcessorListener that mirrors every parameter change made
    // in this editor into the siblings.  Only siblings that actually have
    // a plugin loaded at the same slot are linked (mismatch == skipped).
    std::vector<juce::AudioPluginInstance*> siblings;
    {
        auto sel = getSelectedChannels (isInput);
        if (sel.size() > 1
            && std::find (sel.begin(), sel.end(), ch) != sel.end())
        {
            for (int other : sel)
            {
                if (other == ch) continue;
                auto* otherHost = getHost (engine, isInput, other);
                if (auto* sib = otherHost ? otherHost->getPluginAt (slotIdx) : nullptr)
                    siblings.push_back (sib);
            }
        }
    }

    juce::Logger::writeToLog ("opening per-channel plugin editor [" + ctx + "] = "
                              + plugin->getName()
                              + (siblings.empty() ? juce::String{}
                                 : "  (linked to " + juce::String ((int) siblings.size()) + " sibling(s))"));
    wins[(size_t) ch][(size_t) slotIdx].reset (new PluginEditorWindow (*plugin,
        [this, isInput, ch, slotIdx]
        {
            juce::MessageManager::callAsync ([this, isInput, ch, slotIdx]
            {
                closeEditorFor (isInput, ch, slotIdx);
            });
        },
        ctx,
        std::move (siblings)));
    juce::Logger::writeToLog ("opened per-channel plugin editor [" + ctx + "] = " + plugin->getName());
}

void MatrixView::closeEditorFor (bool isInput, int ch, int slotIdx)
{
    auto& wins = isInput ? inputEditorWindows : outputEditorWindows;
    if (ch < 0 || ch >= (int) wins.size()) return;
    if (slotIdx < 0 || slotIdx >= PluginHost::kNumSlots) return;
    wins[(size_t) ch][(size_t) slotIdx].reset();
}

} // namespace dcr

#include "UI/MatrixView.h"

#include "Engine/AudioEngine.h"
#include "DSP/PluginHost.h"
#include "UI/PluginEditorWindow.h"
#include "Routing/RoutingMatrix.h"

#include <cmath>

namespace dcr {

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

void MatrixView::TopRailContent::paint (juce::Graphics& g)
{
    owner.paintTopRail (g);
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

    topRailViewport.setScrollBarsShown (false, false);
    addAndMakeVisible (topRailViewport);
    topRailViewport.setViewedComponent (&topRailContent, false);

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
    return s;
}

void MatrixView::rebuildFromEngine()
{
    // Drop existing children.
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

    inputLabels.clear();
    outputLabels.clear();
    for (auto& d : engine.getDeviceInfo())
    {
        for (int c = 0; c < d.numInputChannels; ++c)
            inputLabels.push_back ({ d.name, c + 1, c == 0 });
        for (int c = 0; c < d.numOutputChannels; ++c)
            outputLabels.push_back ({ d.name, c + 1, c == 0 });
    }

    const int nIn  = (int) inputLabels .size();
    const int nOut = (int) outputLabels.size();
    auto& matrix = engine.getRoutingMatrix();

    // Set size of rail content panels
    leftRailContent.setSize (labelColWidth, nIn * cellSize);
    topRailContent.setSize (nOut * cellSize, labelRowHeight);

    // Input row widgets.
    for (int n = 0; n < nIn; ++n)
    {
        auto* lbl = new juce::Label ({},
            inputLabels[(size_t) n].deviceName + "  ch." + juce::String (inputLabels[(size_t) n].channelIndex));
        lbl->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
        lbl->setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        lbl->setJustificationType (juce::Justification::centredLeft);
        leftRailContent.addAndMakeVisible (*lbl);
        inputNames.add (lbl);

        auto* sl = makeTrimSlider();
        sl->setValue (linToDb (matrix.getInputTrim (n)), juce::dontSendNotification);
        sl->onValueChange = [&matrix, sl, n] { matrix.setInputTrim (n, dbToLin ((float) sl->getValue())); };
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
            matrix.setInputMute (n, mute->getToggleState());
            if (onUserMuteChanged) onUserMuteChanged();
        };
        leftRailContent.addAndMakeVisible (*mute);
        inputMuteBtns.add (mute);

        auto* solo = new juce::TextButton ("S");
        solo->setName ("solo");
        solo->setClickingTogglesState (true);
        solo->setToggleState (matrix.getInputSolo (n), juce::dontSendNotification);
        solo->onClick = [&matrix, solo, n] { matrix.setInputSolo (n, solo->getToggleState()); };
        leftRailContent.addAndMakeVisible (*solo);
        inputSoloBtns.add (solo);

        auto* infx = new juce::TextButton ("FX");
        infx->setName ("fx");
        infx->onClick = [this, n] { openFxMenuFor (/*isInput=*/true, n); };
        leftRailContent.addAndMakeVisible (*infx);
        inputFxBtns.add (infx);
    }
    inputEditorWindows.resize ((size_t) nIn);
    for (int n = 0; n < nIn; ++n) updateFxButtonAppearance (true, n);

    // Output column widgets.
    for (int m = 0; m < nOut; ++m)
    {
        auto* sl = makeTrimSlider();
        sl->setValue (linToDb (matrix.getOutputTrim (m)), juce::dontSendNotification);
        sl->onValueChange = [&matrix, sl, m] { matrix.setOutputTrim (m, dbToLin ((float) sl->getValue())); };
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
            matrix.setOutputMute (m, mute->getToggleState());
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
    outputEditorWindows.resize ((size_t) nOut);
    for (int m = 0; m < nOut; ++m) updateFxButtonAppearance (false, m);

    // Single crosspoint grid inside gridViewport
    grid = std::make_unique<CrosspointGrid> (matrix);
    grid->setDimensions (nIn, nOut, cellSize);
    gridViewport.setViewedComponent (grid.get(), false);

    // Calculate device boundaries
    std::vector<int> inBounds;
    for (int i = 1; i < nIn; ++i)
        if (inputLabels[(size_t) i].startsNewGroup)
            inBounds.push_back (i);

    std::vector<int> outBounds;
    for (int j = 1; j < nOut; ++j)
        if (outputLabels[(size_t) j].startsNewGroup)
            outBounds.push_back (j);

    grid->setDeviceBoundaries (inBounds, outBounds);

    resized();
    repaint();
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

    auto popup = std::make_unique<FxChainPopupContent> (*this, isInput, ch);
    auto bounds = btns[ch]->getScreenBounds();
    juce::CallOutBox::launchAsynchronously (std::move (popup), bounds, nullptr);
}

// ============================================================================
// FxChainPopupContent (small 3-row B / name / X plugin chain panel)
// ============================================================================
MatrixView::FxChainPopupContent::FxChainPopupContent (MatrixView& o, bool inp, int c)
    : owner (o), isInput (inp), ch (c)
{
    header.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::bold));
    header.setColour (juce::Label::textColourId, juce::Colour::fromRGB (0, 200, 220));
    header.setText (juce::String (isInput ? "INPUT" : "OUTPUT")
                    + " ch." + juce::String (ch + 1) + "  FX chain",
                    juce::dontSendNotification);
    addAndMakeVisible (header);

    for (int s = 0; s < (int) rows.size(); ++s)
    {
        auto& row = rows[(size_t) s];
        row.bypass.setName ("bypass");
        row.bypass.setClickingTogglesState (true);
        row.bypass.setTooltip ("Bypass this slot");
        row.bypass.onClick = [this, s] { onSlotBypassClicked (s); };

        row.name.setName ("slot");
        row.name.setTooltip ("Click: load / open editor.  Drag: reorder.");
        row.name.setButtonText ("+ insert");
        row.name.slotIdx = s;
        row.name.onClick = [this, s] { onSlotNameClicked (s); };
        row.name.onSwap  = [this] (int from, int to)
        {
            if (auto* h = getHost (owner.engine, isInput, ch))
            {
                h->swapSlots (from, to);
                owner.updateFxButtonAppearance (isInput, ch);
                refreshAll();
            }
        };

        row.remove.setName ("remove");
        row.remove.setTooltip ("Remove plugin");
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
    auto* host = getHost (owner.engine, isInput, ch);
    if (host == nullptr) return;
    if (host->getPluginAt (slotIdx) != nullptr)
        owner.showEditorFor (isInput, ch, slotIdx);
    else
        owner.loadPluginInto (isInput, ch, slotIdx);
}

void MatrixView::FxChainPopupContent::onSlotBypassClicked (int slotIdx)
{
    if (auto* host = getHost (owner.engine, isInput, ch))
    {
        host->setBypassedAt (slotIdx, rows[(size_t) slotIdx].bypass.getToggleState());
        owner.updateFxButtonAppearance (isInput, ch);
        refreshAll();
    }
}

void MatrixView::FxChainPopupContent::onSlotRemoveClicked (int slotIdx)
{
    owner.closeEditorFor (isInput, ch, slotIdx);
    if (auto* host = getHost (owner.engine, isInput, ch))
        host->clearSlot (slotIdx);
    owner.updateFxButtonAppearance (isInput, ch);
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
    wins[(size_t) ch][(size_t) slotIdx].reset (new PluginEditorWindow (*plugin,
        [this, isInput, ch, slotIdx]
        {
            juce::MessageManager::callAsync ([this, isInput, ch, slotIdx]
            {
                closeEditorFor (isInput, ch, slotIdx);
            });
        },
        ctx));
}

void MatrixView::closeEditorFor (bool isInput, int ch, int slotIdx)
{
    auto& wins = isInput ? inputEditorWindows : outputEditorWindows;
    if (ch < 0 || ch >= (int) wins.size()) return;
    if (slotIdx < 0 || slotIdx >= PluginHost::kNumSlots) return;
    wins[(size_t) ch][(size_t) slotIdx].reset();
}

} // namespace dcr

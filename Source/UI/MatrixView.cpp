#include "UI/MatrixView.h"

#include "DSP/Builtin/InternalPluginFormat.h"
#include "DSP/PluginHost.h"
#include "Engine/AudioEngine.h"
#include "Routing/RoutingMatrix.h"
#include "UI/PluginEditorWindow.h"

#include <algorithm>
#include <cmath>

namespace dcr
{

    namespace
    {
        // Channel-name label that surfaces a mouseDown callback.  Used in the
        // matrix rails to drive multi-track selection (shift / cmd modifiers).
        // We intentionally swallow the click so juce::Label's double-click
        // doesn't pop the rename editor.
        class SelectableLabel : public juce::Label
        {
        public:
            // Callback signature now passes the local click X coordinate too --
            // the input-rail row label uses a leading "[-]" prefix that should
            // collapse the device when clicked, but selection behavior should
            // kick in everywhere else on the row.  Click position is the only
            // way to differentiate without adding a separate button widget per
            // row (~600 extra components on a busy session).
            std::function<void (const juce::ModifierKeys&, int x)> onMouseDown;
            void mouseDown (const juce::MouseEvent& e) override
            {
                if (onMouseDown)
                    onMouseDown (e.mods, e.x);
            }
        };
    }

    namespace
    {
        juce::uint32 nowMs() { return juce::Time::getMillisecondCounter(); }
        void logRebuildStep (const char* what, juce::uint32 startMs)
        {
            juce::Logger::writeToLog (juce::String ("MatrixView::rebuild: ") + what
                                      + " @ "
                                      + juce::String (nowMs() - startMs)
                                      + " ms");
        }

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

        // Expand/Collapse-all buttons in the corner legend.  Added AFTER
        // cornerCell so they paint on top of its background.
        auto setupCornerBtn = [this] (juce::TextButton& b, const char* tip, bool isInput, bool collapse) {
            b.setTooltip (tip);
            b.setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (58, 58, 66));
            b.onClick = [this, isInput, collapse] { collapseAllDevices (isInput, collapse); };
            addAndMakeVisible (b);
        };
        setupCornerBtn (outExpandAllBtn, "Expand all output devices", false, false);
        setupCornerBtn (outCollapseAllBtn, "Collapse all output devices", false, true);
        setupCornerBtn (inExpandAllBtn, "Expand all input devices", true, false);
        setupCornerBtn (inCollapseAllBtn, "Collapse all input devices", true, true);

        // Empty-state guidance.  Added last so it paints on top of the (empty)
        // grid viewport.  Non-interactive so clicks pass through.
        emptyHint.setJustificationType (juce::Justification::centred);
        emptyHint.setInterceptsMouseClicks (false, false);
        emptyHint.setColour (juce::Label::textColourId, juce::Colour::fromRGB (130, 130, 142));
        emptyHint.setFont (juce::FontOptions (16.0f));
        emptyHint.setText ("No devices selected\n\nOpen  Devices...  to choose your inputs and outputs",
            juce::dontSendNotification);
        addChildComponent (emptyHint); // hidden until updateEmptyHintVisibility() shows it

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
        if (grid)
            grid->setHighlightedColumns (highlightedOutputs);
        topRailContent.repaint();
        repaint();
    }

    void MatrixView::setHighlightedInputs (std::vector<int> ins)
    {
        highlightedInputs = std::move (ins);
        if (grid)
            grid->setHighlightedRows (highlightedInputs);
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
        s->setColour (juce::Slider::rotarySliderFillColourId, juce::Colour::fromRGB (0, 255, 210));
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
        rebuildState.startMs = juce::Time::getMillisecondCounter();

        // Channel counts may change.  Drop selection so it can't refer to a
        // channel that no longer exists -- a stale index would point at a
        // different device after device add/remove.
        selectedInputs.clear();
        selectedOutputs.clear();
        lastClickedInput = -1;
        lastClickedOutput = -1;

        // Consume the one-shot force flag.  A collapse/expand changes the label
        // STRUCTURE without changing the engine's physical channel list, so the
        // fast path below (which only compares against the engine) can't tell --
        // the caller sets this to force a real rebuild.
        const bool forceFull = forceStructuralRebuild;
        forceStructuralRebuild = false;

        // FAST PATH: settings-only restart leaves the channel layout intact.
        // No reason to throw away 500+ widgets just to recreate identical ones --
        // just refresh their values from the engine and return.  Single-digit
        // milliseconds vs multiple seconds.
        if (!forceFull && !inputLabels.empty() && samePhysicalLayout())
        {
            softRefreshFromEngine();
            juce::Logger::writeToLog ("MatrixView::rebuild: fast-path soft refresh ("
                                      + juce::String (juce::Time::getMillisecondCounter()
                                                      - rebuildState.startMs)
                                      + " ms)");
            updateEmptyHintVisibility();
            if (onRebuildFinished)
                onRebuildFinished();
            return;
        }

        clearAllChannelWidgets();
        buildLabelsFromEngine();

        const int nIn = (int) inputLabels.size();
        const int nOut = (int) outputLabels.size();

        // Set size of rail content panels up-front so the viewport's scroll
        // ranges are right from the start; widgets fill into the canvas as
        // they get built asynchronously below.
        leftRailContent.setSize (labelColWidth, nIn * cellSize);
        topRailContent.setSize (nOut * cellSize, labelRowHeight);

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
        topRailViewport.setVisible (false);
        gridViewport.setVisible (false);
        cornerCell.setVisible (false);

        // Async chunked build -- spreads ~600 component creations over many
        // message-thread ticks so the UI stays responsive on big device
        // configs (48 in + 66 out used to freeze the window for ~3 seconds).
        inputEditorWindows.resize ((size_t) nIn);
        outputEditorWindows.resize ((size_t) nOut);

        rebuildState.generation = ++rebuildGenerationCounter;
        rebuildState.active = true;
        rebuildState.nextInputIdx = 0;
        rebuildState.nextOutputIdx = 0;
        rebuildState.totalInputs = nIn;
        rebuildState.totalOutputs = nOut;
        if (onRebuildProgress)
            onRebuildProgress (0, nIn + nOut);

        // Kick off the chunked build on the next tick so the current event
        // (mouse click etc.) finishes first.
        const auto gen = rebuildState.generation;
        juce::MessageManager::callAsync ([this, gen] {
            if (gen != rebuildState.generation)
                return; // a newer rebuild has started
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
                if (inIdx >= (int) inputLabels.size())
                    return false;
                if (inputLabels[(size_t) inIdx].deviceName != d.name)
                    return false;
                if (inputLabels[(size_t) inIdx].channelIndex != c + 1)
                    return false;
            }
            for (int c = 0; c < d.numOutputChannels; ++c, ++outIdx)
            {
                if (outIdx >= (int) outputLabels.size())
                    return false;
                if (outputLabels[(size_t) outIdx].deviceName != d.name)
                    return false;
                if (outputLabels[(size_t) outIdx].channelIndex != c + 1)
                    return false;
            }
        }
        return inIdx == (int) inputLabels.size()
               && outIdx == (int) outputLabels.size();
    }

    void MatrixView::softRefreshFromEngine()
    {
        auto& matrix = engine.getRoutingMatrix();
        const int nIn = (int) inputTrims.size();
        const int nOut = (int) outputTrims.size();
        for (int n = 0; n < nIn; ++n)
        {
            if (n >= (int) inputLabels.size())
                break;
            const auto& lbl = inputLabels[(size_t) n];
            if (lbl.isCollapsedRow)
                continue;
            const int engCh = lbl.firstChannel;
            if (inputTrims[n] != nullptr)
                inputTrims[n]->setValue (linToDb (matrix.getInputTrim (engCh)), juce::dontSendNotification);
            if (inputMuteBtns[n] != nullptr)
                inputMuteBtns[n]->setToggleState (matrix.getInputMute (engCh), juce::dontSendNotification);
            if (inputSoloBtns[n] != nullptr)
                inputSoloBtns[n]->setToggleState (matrix.getInputSolo (engCh), juce::dontSendNotification);
            updateFxButtonAppearance (true, engCh);
        }
        for (int m = 0; m < nOut; ++m)
        {
            if (m >= (int) outputLabels.size())
                break;
            const auto& lbl = outputLabels[(size_t) m];
            if (lbl.isCollapsedRow)
                continue;
            const int engOut = lbl.firstChannel;
            if (outputTrims[m] != nullptr)
                outputTrims[m]->setValue (linToDb (matrix.getOutputTrim (engOut)), juce::dontSendNotification);
            if (outputMuteBtns[m] != nullptr)
                outputMuteBtns[m]->setToggleState (matrix.getOutputMute (engOut), juce::dontSendNotification);
            updateFxButtonAppearance (false, engOut);
        }
        if (grid != nullptr)
            grid->repaint();
    }

    void MatrixView::refreshTrimWidgetsFromEngine()
    {
        auto& matrix = engine.getRoutingMatrix();
        const int nIn = (int) inputTrims.size();
        const int nOut = (int) outputTrims.size();

        auto syncSlider = [] (juce::Slider* s, float db) {
            if (s == nullptr)
                return;
            if (s->isMouseButtonDown())
                return; // don't fight a live drag
            if (std::abs ((float) s->getValue() - db) <= 0.01f)
                return;
            s->setValue (db, juce::dontSendNotification);
        };

        for (int n = 0; n < nIn; ++n)
        {
            if (n >= (int) inputLabels.size())
                break;
            const auto& lbl = inputLabels[(size_t) n];
            if (lbl.isCollapsedRow)
                continue;
            const int engCh = lbl.firstChannel;
            syncSlider (inputTrims[n], linToDb (matrix.getInputTrim (engCh)));
            if (inputMuteBtns[n] != nullptr)
                inputMuteBtns[n]->setToggleState (matrix.getInputMute (engCh), juce::dontSendNotification);
            if (inputSoloBtns[n] != nullptr)
                inputSoloBtns[n]->setToggleState (matrix.getInputSolo (engCh), juce::dontSendNotification);
        }
        for (int m = 0; m < nOut; ++m)
        {
            if (m >= (int) outputLabels.size())
                break;
            const auto& lbl = outputLabels[(size_t) m];
            if (lbl.isCollapsedRow)
                continue;
            const int engOut = lbl.firstChannel;
            syncSlider (outputTrims[m], linToDb (matrix.getOutputTrim (engOut)));
            if (outputMuteBtns[m] != nullptr)
                outputMuteBtns[m]->setToggleState (matrix.getOutputMute (engOut), juce::dontSendNotification);
        }
    }

    void MatrixView::clearAllChannelWidgets()
    {
        grid.reset();
        inputNames.clear();
        inputTrims.clear();
        inputMeters.clear();
        inputMuteBtns.clear();
        inputSoloBtns.clear();
        inputFxBtns.clear();
        inputCollapseBtns.clear();
        outputTrims.clear();
        outputMeters.clear();
        outputMuteBtns.clear();
        outputFxBtns.clear();
        outputCollapseBtns.clear();
        // Close any open plugin editors -- engine restart invalidates instances.
        for (auto& row : outputEditorWindows)
            for (auto& w : row)
                if (w)
                    w->setVisible (false);
        for (auto& row : inputEditorWindows)
            for (auto& w : row)
                if (w)
                    w->setVisible (false);
        outputEditorWindows.clear();
        inputEditorWindows.clear();
    }

    void MatrixView::buildLabelsFromEngine()
    {
        inputLabels.clear();
        outputLabels.clear();

        // engineInputIdx / engineOutputIdx track the global channel index that
        // accumulates as we walk every device.  These are the indices the
        // engine uses for routing matrix / mute / trim / plugin host lookups.
        int engineInputIdx = 0;
        int engineOutputIdx = 0;

        for (auto& d : engine.getDeviceInfo())
        {
            const bool inCollapsed = collapsedInputDevices.count (d.name) > 0
                                     && d.numInputChannels > 0;
            const bool outCollapsed = collapsedOutputDevices.count (d.name) > 0
                                      && d.numOutputChannels > 0;

            if (inCollapsed)
            {
                ChannelLabel lbl;
                lbl.deviceName = d.name;
                lbl.channelIndex = -1;
                lbl.startsNewGroup = true;
                lbl.isCollapsedRow = true;
                lbl.firstChannel = engineInputIdx;
                lbl.channelCount = d.numInputChannels;
                inputLabels.push_back (std::move (lbl));
                engineInputIdx += d.numInputChannels;
            }
            else
            {
                for (int c = 0; c < d.numInputChannels; ++c)
                {
                    ChannelLabel lbl;
                    lbl.deviceName = d.name;
                    lbl.channelIndex = c + 1;
                    lbl.startsNewGroup = (c == 0);
                    lbl.firstChannel = engineInputIdx + c;
                    lbl.channelCount = 1;
                    inputLabels.push_back (std::move (lbl));
                }
                engineInputIdx += d.numInputChannels;
            }

            if (outCollapsed)
            {
                ChannelLabel lbl;
                lbl.deviceName = d.name;
                lbl.channelIndex = -1;
                lbl.startsNewGroup = true;
                lbl.isCollapsedRow = true;
                lbl.firstChannel = engineOutputIdx;
                lbl.channelCount = d.numOutputChannels;
                outputLabels.push_back (std::move (lbl));
                engineOutputIdx += d.numOutputChannels;
            }
            else
            {
                for (int c = 0; c < d.numOutputChannels; ++c)
                {
                    ChannelLabel lbl;
                    lbl.deviceName = d.name;
                    lbl.channelIndex = c + 1;
                    lbl.startsNewGroup = (c == 0);
                    lbl.firstChannel = engineOutputIdx + c;
                    lbl.channelCount = 1;
                    outputLabels.push_back (std::move (lbl));
                }
                engineOutputIdx += d.numOutputChannels;
            }
        }
    }

    // ----- Device collapse / expand public API -----------------------------------
    void MatrixView::setDeviceCollapsed (bool isInput,
        const juce::String& deviceName,
        bool collapsed)
    {
        auto& set = isInput ? collapsedInputDevices : collapsedOutputDevices;
        const bool already = set.count (deviceName) > 0;
        if (already == collapsed)
            return;

        if (collapsed)
            set.insert (deviceName);
        else
            set.erase (deviceName);

        // Force a structural rebuild -- the row/column list changed shape but the
        // engine's physical channel list did NOT, so the fast path can't detect
        // the change on its own.
        forceStructuralRebuild = true;
        rebuildFromEngine();
        notifyCollapseChanged();
    }

    bool MatrixView::isDeviceCollapsed (bool isInput,
        const juce::String& deviceName) const
    {
        const auto& set = isInput ? collapsedInputDevices : collapsedOutputDevices;
        return set.count (deviceName) > 0;
    }

    void MatrixView::collapseAllDevices (bool isInput, bool collapsed)
    {
        auto& set = isInput ? collapsedInputDevices : collapsedOutputDevices;
        const size_t prev = set.size();
        set.clear();
        if (collapsed)
        {
            for (auto& d : engine.getDeviceInfo())
            {
                const int n = isInput ? d.numInputChannels : d.numOutputChannels;
                if (n > 0)
                    set.insert (d.name);
            }
        }
        if (set.size() == prev && !collapsed)
            return; // nothing to do (was already empty)
        forceStructuralRebuild = true;
        rebuildFromEngine();
        notifyCollapseChanged();
    }

    std::vector<juce::String> MatrixView::getCollapsedDeviceNames (bool isInput) const
    {
        const auto& set = isInput ? collapsedInputDevices : collapsedOutputDevices;
        return { set.begin(), set.end() };
    }

    void MatrixView::setCollapsedDeviceNames (bool isInput,
        std::vector<juce::String> names)
    {
        auto& set = isInput ? collapsedInputDevices : collapsedOutputDevices;
        set.clear();
        for (auto& n : names)
            set.insert (std::move (n));
        forceStructuralRebuild = true;
        rebuildFromEngine();
        // No notifyCollapseChanged() here -- this is called from snapshot
        // restore so we don't want to re-save what we just loaded.
    }

    void MatrixView::notifyCollapseChanged()
    {
        if (onCollapseStateChanged)
            onCollapseStateChanged();
    }

    void MatrixView::continueRebuild()
    {
        if (!rebuildState.active)
            return;

        const auto chunkStart = nowMs();

        // Bigger chunks now that each row sets its own bounds inline -- the
        // earlier 16-per-side cap existed because we were re-laying-out ALL
        // existing widgets every tick (O(N) work per tick).  Per-row layout
        // means the only per-tick cost is the new widget creation, so we
        // can comfortably do 64 per side.
        constexpr int kBatch = 64;

        int inputsBuilt = 0;
        while (inputsBuilt < kBatch && rebuildState.nextInputIdx < rebuildState.totalInputs)
        {
            buildInputRowWidgets (rebuildState.nextInputIdx);
            ++rebuildState.nextInputIdx;
            ++inputsBuilt;
        }
        int outputsBuilt = 0;
        while (outputsBuilt < kBatch && rebuildState.nextOutputIdx < rebuildState.totalOutputs)
        {
            buildOutputColumnWidgets (rebuildState.nextOutputIdx);
            ++rebuildState.nextOutputIdx;
            ++outputsBuilt;
        }

        juce::Logger::writeToLog ("MatrixView::rebuild: chunk #"
                                  + juce::String (rebuildState.chunkCount + 1)
                                  + " built " + juce::String (inputsBuilt) + " in + "
                                  + juce::String (outputsBuilt) + " out in "
                                  + juce::String (nowMs() - chunkStart) + " ms");

        ++rebuildState.chunkCount;

        if (onRebuildProgress)
            onRebuildProgress (rebuildState.nextInputIdx + rebuildState.nextOutputIdx,
                rebuildState.totalInputs + rebuildState.totalOutputs);

        const bool moreInputs = rebuildState.nextInputIdx < rebuildState.totalInputs;
        const bool moreOutputs = rebuildState.nextOutputIdx < rebuildState.totalOutputs;
        if (moreInputs || moreOutputs)
        {
            const auto gen = rebuildState.generation;
            juce::MessageManager::callAsync ([this, gen] {
                if (gen != rebuildState.generation)
                    return;
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
        logRebuildStep ("chunks done", rebuildState.startMs);

        auto& matrix = engine.getRoutingMatrix();
        grid = std::make_unique<CrosspointGrid> (matrix);

        // Build per-visible-row / per-visible-column spans so collapsed device
        // rows/columns can render as aggregate cells.  count==1 spans are
        // ordinary 1:1 channels.
        std::vector<CrosspointGrid::CellSpan> inSpans, outSpans;
        inSpans.reserve (inputLabels.size());
        outSpans.reserve (outputLabels.size());
        for (auto& l : inputLabels)
            inSpans.push_back ({ l.firstChannel, l.channelCount });
        for (auto& l : outputLabels)
            outSpans.push_back ({ l.firstChannel, l.channelCount });

        grid->setDimensions (rebuildState.totalInputs, rebuildState.totalOutputs, cellSize, std::move (inSpans), std::move (outSpans));
        gridViewport.setViewedComponent (grid.get(), false);

        // Click on an "aggregate" cell (where at least one side is collapsed)
        // -> expand both involved devices so the user can route precisely.
        grid->onAggregateCellClicked = [this] (int outVisIdx, int inVisIdx) {
            if (outVisIdx >= 0 && outVisIdx < (int) outputLabels.size())
            {
                const auto& ol = outputLabels[(size_t) outVisIdx];
                if (ol.isCollapsedRow)
                    setDeviceCollapsed (false, ol.deviceName, false);
            }
            if (inVisIdx >= 0 && inVisIdx < (int) inputLabels.size())
            {
                const auto& il = inputLabels[(size_t) inVisIdx];
                if (il.isCollapsedRow)
                    setDeviceCollapsed (true, il.deviceName, false);
            }
        };

        // Device boundary lines (same start-of-group flag we recorded when
        // building inputLabels / outputLabels).
        std::vector<int> inBounds;
        for (int i = 1; i < (int) inputLabels.size(); ++i)
            if (inputLabels[(size_t) i].startsNewGroup)
                inBounds.push_back (i);
        std::vector<int> outBounds;
        for (int j = 1; j < (int) outputLabels.size(); ++j)
            if (outputLabels[(size_t) j].startsNewGroup)
                outBounds.push_back (j);
        grid->setDeviceBoundaries (inBounds, outBounds);

        // Restore visibility of the rails/grid we hid during the chunked build.
        // Doing this AFTER all the addAndMakeVisible() calls inside the chunks
        // means JUCE only has to paint the final tree once, instead of after
        // every newly-added widget.  Massively cuts message-thread paint work
        // during the rebuild and stops the audio thread from getting starved.
        leftRailViewport.setVisible (true);
        topRailViewport.setVisible (true);
        gridViewport.setVisible (true);
        cornerCell.setVisible (true);

        resized();
        repaint();

        rebuildState.active = false;
        updateEmptyHintVisibility();
        juce::Logger::writeToLog ("MatrixView::rebuild: DONE ("
                                  + juce::String (rebuildState.chunkCount) + " chunks, "
                                  + juce::String (juce::Time::getMillisecondCounter()
                                                  - rebuildState.startMs)
                                  + " ms total)");
        if (onRebuildFinished)
            onRebuildFinished();
    }

    void MatrixView::buildInputRowWidgets (int n)
    {
        if (n < 0 || n >= (int) inputLabels.size())
            return;
        auto& matrix = engine.getRoutingMatrix();

        // ----- Collapsed device row -----------------------------------------------
        // Real "+" TextButton on the left followed by a device-name label.
        // Per-channel widget slots are nullptr for these rows so iterators
        // must null-guard.
        if (inputLabels[(size_t) n].isCollapsedRow)
        {
            const juce::String devName = inputLabels[(size_t) n].deviceName;
            const int chCount = inputLabels[(size_t) n].channelCount;

            auto* expandBtn = new juce::TextButton ("+");
            expandBtn->setTooltip ("Expand " + devName + "'s input channels");
            expandBtn->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (28, 70, 80));
            expandBtn->setColour (juce::TextButton::textColourOffId, juce::Colour::fromRGB (0, 220, 240));
            expandBtn->onClick = [this, devName] { setDeviceCollapsed (true, devName, false); };
            leftRailContent.addAndMakeVisible (*expandBtn);

            auto* lbl = new SelectableLabel();
            lbl->setText (devName + "  (" + juce::String (chCount) + " ch)",
                juce::dontSendNotification);
            lbl->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                10.0f,
                juce::Font::bold));
            lbl->setColour (juce::Label::textColourId, juce::Colour::fromRGB (0, 200, 220));
            lbl->setJustificationType (juce::Justification::centredLeft);
            lbl->setTooltip ("Click [+] to expand " + devName + "'s input channels");
            // Label itself doesn't toggle -- only the button does, so the user
            // can still drag-select across the row in the future if needed.
            lbl->onMouseDown = [] (const juce::ModifierKeys&, int) {};
            leftRailContent.addAndMakeVisible (*lbl);

            inputNames.add (lbl);
            inputCollapseBtns.add (expandBtn);
            inputTrims.add (nullptr);
            inputMeters.add (nullptr);
            inputMuteBtns.add (nullptr);
            inputSoloBtns.add (nullptr);
            inputFxBtns.add (nullptr);

            const int yy = n * cellSize;
            expandBtn->setBounds (4, yy + 4, 18, cellSize - 8);
            lbl->setBounds (26, yy + 2, leftRailContent.getWidth() - 30, cellSize - 4);
            return;
        }

        // engCh = the engine-side channel this label represents.  When no
        // device is collapsed firstChannel == n, but with collapsed devices
        // present the visual index and engine index diverge.
        const int engCh = inputLabels[(size_t) n].firstChannel;

        {
            const bool isFirstChOfDevice = inputLabels[(size_t) n].startsNewGroup;
            const juce::String devName = inputLabels[(size_t) n].deviceName;

            // First channel of each device gets a real "−" button to collapse
            // the device.  Subsequent channel rows have no collapse button
            // (nullptr) so per-row layout knows to slide the label leftwards.
            if (isFirstChOfDevice)
            {
                auto* collapseBtn = new juce::TextButton (juce::String::charToString ((juce::juce_wchar) 0x2212)); // "−"
                collapseBtn->setTooltip ("Collapse " + devName + "'s input channels");
                collapseBtn->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (40, 40, 48));
                collapseBtn->setColour (juce::TextButton::textColourOffId, juce::Colour::fromRGB (200, 200, 205));
                collapseBtn->onClick = [this, devName] { setDeviceCollapsed (true, devName, true); };
                leftRailContent.addAndMakeVisible (*collapseBtn);
                inputCollapseBtns.add (collapseBtn);
            }
            else
            {
                inputCollapseBtns.add (nullptr);
            }

            auto* lbl = new SelectableLabel();
            lbl->setText (devName + "  ch."
                              + juce::String (inputLabels[(size_t) n].channelIndex),
                juce::dontSendNotification);
            lbl->setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
            lbl->setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
            lbl->setJustificationType (juce::Justification::centredLeft);
            lbl->setTooltip ("Click to select.  Shift-click to extend, Cmd-click to toggle.");
            lbl->onMouseDown = [this, n] (const juce::ModifierKeys& mods, int) {
                handleChannelHeaderClick (true, n, mods);
            };
            leftRailContent.addAndMakeVisible (*lbl);
            inputNames.add (lbl);

            auto* sl = makeTrimSlider();
            sl->setValue (linToDb (matrix.getInputTrim (engCh)), juce::dontSendNotification);
            sl->onValueChange = [this, &matrix, sl, n, engCh] {
                const float dbVal = (float) sl->getValue();
                const float linVal = dbToLin (dbVal);
                matrix.setInputTrim (engCh, linVal);
                // Multi-select linking: same trim on every other selected input.
                // Skip collapsed-device peer rows (inputTrims[other] == nullptr).
                if (isChannelSelected (true, n))
                {
                    auto sel = getSelectedChannels (true);
                    if (sel.size() > 1)
                    {
                        for (int other : sel)
                        {
                            if (other == n)
                                continue;
                            if (other < 0 || other >= inputTrims.size())
                                continue;
                            if (inputTrims[other] == nullptr)
                                continue;
                            const int otherEng = inputLabels[(size_t) other].firstChannel;
                            inputTrims[other]->setValue (dbVal, juce::dontSendNotification);
                            matrix.setInputTrim (otherEng, linVal);
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
            mute->setToggleState (matrix.getInputMute (engCh), juce::dontSendNotification);
            mute->onClick = [this, &matrix, mute, n, engCh] {
                const bool on = mute->getToggleState();
                matrix.setInputMute (engCh, on);
                if (isChannelSelected (true, n))
                {
                    auto sel = getSelectedChannels (true);
                    if (sel.size() > 1)
                    {
                        for (int other : sel)
                        {
                            if (other == n)
                                continue;
                            if (other < 0 || other >= inputMuteBtns.size())
                                continue;
                            if (inputMuteBtns[other] == nullptr)
                                continue;
                            const int otherEng = inputLabels[(size_t) other].firstChannel;
                            inputMuteBtns[other]->setToggleState (on, juce::dontSendNotification);
                            matrix.setInputMute (otherEng, on);
                        }
                    }
                }
                if (onUserMuteChanged)
                    onUserMuteChanged();
            };
            leftRailContent.addAndMakeVisible (*mute);
            inputMuteBtns.add (mute);

            auto* solo = new juce::TextButton ("S");
            solo->setName ("solo");
            solo->setClickingTogglesState (true);
            solo->setToggleState (matrix.getInputSolo (engCh), juce::dontSendNotification);
            solo->onClick = [this, &matrix, solo, n, engCh] {
                const bool on = solo->getToggleState();
                matrix.setInputSolo (engCh, on);
                if (isChannelSelected (true, n))
                {
                    auto sel = getSelectedChannels (true);
                    if (sel.size() > 1)
                    {
                        for (int other : sel)
                        {
                            if (other == n)
                                continue;
                            if (other < 0 || other >= inputSoloBtns.size())
                                continue;
                            if (inputSoloBtns[other] == nullptr)
                                continue;
                            const int otherEng = inputLabels[(size_t) other].firstChannel;
                            inputSoloBtns[other]->setToggleState (on, juce::dontSendNotification);
                            matrix.setInputSolo (otherEng, on);
                        }
                    }
                }
            };
            leftRailContent.addAndMakeVisible (*solo);
            inputSoloBtns.add (solo);

            auto* infx = new juce::TextButton ("FX");
            infx->setName ("fx");
            // openFxMenuFor takes a CHANNEL index (engine-side), not a label
            // index -- the FX popup edits the PluginHost for that engine
            // channel.
            infx->onClick = [this, engCh] { openFxMenuFor (/*isInput=*/true, engCh); };
            leftRailContent.addAndMakeVisible (*infx);
            inputFxBtns.add (infx);
        }
        updateFxButtonAppearance (true, engCh);

        // Set THIS row's widget bounds directly -- avoids the O(N) global
        // layout pass that used to fire after every chunk.  Matches the
        // geometry in layoutLeftRail() exactly.
        const int yy = n * cellSize;
        if (inputCollapseBtns.getLast() != nullptr)
            inputCollapseBtns.getLast()->setBounds (4, yy + 4, 18, cellSize - 8);
        inputNames.getLast()->setBounds (26, yy + 2, 78, cellSize - 4);
        inputTrims.getLast()->setBounds (106, yy + 2, 32, 32);
        inputMeters.getLast()->setBounds (142, yy + 12, 84, 12);
        inputMuteBtns.getLast()->setBounds (230, yy + 7, 18, 22);
        inputSoloBtns.getLast()->setBounds (250, yy + 7, 18, 22);
        inputFxBtns.getLast()->setBounds (272, yy + 7, 30, 22);
    }

    void MatrixView::buildOutputColumnWidgets (int m)
    {
        if (m < 0 || m >= (int) outputLabels.size())
            return;
        auto& matrix = engine.getRoutingMatrix();

        // Collapsed column -- only a "+" expand button (the rotated DeviceName
        // text is painted by paintTopRail).  Per-channel widget slots are
        // nullptr so iterators must null-guard.
        if (outputLabels[(size_t) m].isCollapsedRow)
        {
            const juce::String devName = outputLabels[(size_t) m].deviceName;

            auto* expandBtn = new juce::TextButton ("+");
            expandBtn->setTooltip ("Expand " + devName + "'s output channels");
            expandBtn->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (28, 70, 80));
            expandBtn->setColour (juce::TextButton::textColourOffId, juce::Colour::fromRGB (0, 220, 240));
            expandBtn->onClick = [this, devName] { setDeviceCollapsed (false, devName, false); };
            topRailContent.addAndMakeVisible (*expandBtn);

            outputCollapseBtns.add (expandBtn);
            outputTrims.add (nullptr);
            outputMeters.add (nullptr);
            outputMuteBtns.add (nullptr);
            outputFxBtns.add (nullptr);

            // Place the button at the BOTTOM of the column header (just above
            // the trim row), so it sits below the rotated DeviceName text.
            const int xx = m * cellSize;
            expandBtn->setBounds (xx + (cellSize - 18) / 2, topRailWidgetsY, 18, 18);
            return;
        }

        const int engOut = outputLabels[(size_t) m].firstChannel;

        // First column of an expanded device gets a "−" button at the bottom
        // of the header band (above the trim).  Other columns: nullptr.
        if (outputLabels[(size_t) m].startsNewGroup)
        {
            const juce::String devName = outputLabels[(size_t) m].deviceName;
            auto* collapseBtn = new juce::TextButton (juce::String::charToString ((juce::juce_wchar) 0x2212));
            collapseBtn->setTooltip ("Collapse " + devName + "'s output channels");
            collapseBtn->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (40, 40, 48));
            collapseBtn->setColour (juce::TextButton::textColourOffId, juce::Colour::fromRGB (200, 200, 205));
            collapseBtn->onClick = [this, devName] { setDeviceCollapsed (false, devName, true); };
            topRailContent.addAndMakeVisible (*collapseBtn);
            outputCollapseBtns.add (collapseBtn);
            const int xx = m * cellSize;
            collapseBtn->setBounds (xx + (cellSize - 18) / 2, topRailWidgetsY, 18, 18);
        }
        else
        {
            outputCollapseBtns.add (nullptr);
        }

        {
            auto* sl = makeTrimSlider();
            sl->setValue (linToDb (matrix.getOutputTrim (engOut)), juce::dontSendNotification);
            sl->onValueChange = [this, &matrix, sl, m, engOut] {
                const float dbVal = (float) sl->getValue();
                const float linVal = dbToLin (dbVal);
                matrix.setOutputTrim (engOut, linVal);
                if (isChannelSelected (false, m))
                {
                    auto sel = getSelectedChannels (false);
                    if (sel.size() > 1)
                    {
                        for (int other : sel)
                        {
                            if (other == m)
                                continue;
                            if (other < 0 || other >= outputTrims.size())
                                continue;
                            if (outputTrims[other] == nullptr)
                                continue;
                            const int otherEng = outputLabels[(size_t) other].firstChannel;
                            outputTrims[other]->setValue (dbVal, juce::dontSendNotification);
                            matrix.setOutputTrim (otherEng, linVal);
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
            mute->setToggleState (matrix.getOutputMute (engOut), juce::dontSendNotification);
            mute->onClick = [this, &matrix, mute, m, engOut] {
                const bool on = mute->getToggleState();
                matrix.setOutputMute (engOut, on);
                if (isChannelSelected (false, m))
                {
                    auto sel = getSelectedChannels (false);
                    if (sel.size() > 1)
                    {
                        for (int other : sel)
                        {
                            if (other == m)
                                continue;
                            if (other < 0 || other >= outputMuteBtns.size())
                                continue;
                            if (outputMuteBtns[other] == nullptr)
                                continue;
                            const int otherEng = outputLabels[(size_t) other].firstChannel;
                            outputMuteBtns[other]->setToggleState (on, juce::dontSendNotification);
                            matrix.setOutputMute (otherEng, on);
                        }
                    }
                }
                if (onUserMuteChanged)
                    onUserMuteChanged();
            };
            topRailContent.addAndMakeVisible (*mute);
            outputMuteBtns.add (mute);

            auto* fx = new juce::TextButton ("FX");
            fx->setName ("fx");
            fx->onClick = [this, engOut] { openFxMenuFor (/*isInput=*/false, engOut); };
            topRailContent.addAndMakeVisible (*fx);
            outputFxBtns.add (fx);
        }
        updateFxButtonAppearance (false, engOut);

        // Set THIS column's widget bounds directly.  Matches layoutTopRail().
        const int xx = m * cellSize;
        outputTrims.getLast()->setBounds (xx + 2, topRailWidgetsY + 24, 32, 32);
        outputMeters.getLast()->setBounds (xx + 11, topRailWidgetsY + 60, 14, 42);
        outputMuteBtns.getLast()->setBounds (xx + 9, topRailWidgetsY + 104, 18, 16);
        outputFxBtns.getLast()->setBounds (xx + 5, topRailWidgetsY + 124, 26, 18);
    }

    void MatrixView::layoutLeftRail()
    {
        for (int n = 0; n < (int) inputNames.size(); ++n)
        {
            const int yy = n * cellSize;
            if (n < inputCollapseBtns.size() && inputCollapseBtns[n] != nullptr)
                inputCollapseBtns[n]->setBounds (4, yy + 4, 18, cellSize - 8);

            // Collapsed-row name labels take the rest of the row width.
            // Per-channel widget slots are nullptr for these rows.
            if (n < (int) inputLabels.size() && inputLabels[(size_t) n].isCollapsedRow)
            {
                if (inputNames[n] != nullptr)
                    inputNames[n]->setBounds (26, yy + 2, leftRailContent.getWidth() - 30, cellSize - 4);
                continue;
            }
            if (inputNames[n] != nullptr)
                inputNames[n]->setBounds (26, yy + 2, 78, cellSize - 4);
            if (n < inputTrims.size() && inputTrims[n] != nullptr)
                inputTrims[n]->setBounds (106, yy + 2, 32, 32);
            if (n < inputMeters.size() && inputMeters[n] != nullptr)
                inputMeters[n]->setBounds (142, yy + 12, 84, 12);
            if (n < inputMuteBtns.size() && inputMuteBtns[n] != nullptr)
                inputMuteBtns[n]->setBounds (230, yy + 7, 18, 22);
            if (n < inputSoloBtns.size() && inputSoloBtns[n] != nullptr)
                inputSoloBtns[n]->setBounds (250, yy + 7, 18, 22);
            if (n < (int) inputFxBtns.size() && inputFxBtns[n] != nullptr)
                inputFxBtns[n]->setBounds (272, yy + 7, 30, 22);
        }
    }

    void MatrixView::layoutTopRail()
    {
        for (int m = 0; m < (int) outputTrims.size(); ++m)
        {
            const int xx = m * cellSize;
            // Collapse/expand button (first column of every device, plus the
            // header for any collapsed column).
            if (m < outputCollapseBtns.size() && outputCollapseBtns[m] != nullptr)
                outputCollapseBtns[m]->setBounds (xx + (cellSize - 18) / 2, topRailWidgetsY, 18, 18);
            if (outputTrims[m] == nullptr)
                continue; // collapsed column has no per-channel widgets
            outputTrims[m]->setBounds (xx + 2, topRailWidgetsY + 24, 32, 32);
            outputMeters[m]->setBounds (xx + 11, topRailWidgetsY + 60, 14, 42);
            outputMuteBtns[m]->setBounds (xx + 9, topRailWidgetsY + 104, 18, 16);
            outputFxBtns[m]->setBounds (xx + 5, topRailWidgetsY + 124, 26, 18);
        }
    }

    void MatrixView::resized()
    {
        cornerCell.setBounds (0, 0, labelColWidth, labelRowHeight);

        // Corner expand/collapse-all buttons.  Outputs sit under the OUTPUTS
        // legend (top-right); inputs sit above the INPUTS legend (bottom-left).
        constexpr int bw = 66, bh = 20, gap = 4;
        outExpandAllBtn.setBounds (labelColWidth - 8 - bw * 2 - gap, 36, bw, bh);
        outCollapseAllBtn.setBounds (labelColWidth - 8 - bw, 36, bw, bh);
        inExpandAllBtn.setBounds (12, labelRowHeight - 52, bw, bh);
        inCollapseAllBtn.setBounds (12 + bw + gap, labelRowHeight - 52, bw, bh);

        topRailViewport.setBounds (labelColWidth, 0, getWidth() - labelColWidth, labelRowHeight);
        leftRailViewport.setBounds (0, labelRowHeight, labelColWidth, getHeight() - labelRowHeight);
        gridViewport.setBounds (labelColWidth, labelRowHeight, getWidth() - labelColWidth, getHeight() - labelRowHeight);

        // Empty-state hint sits centred over the grid region.
        emptyHint.setBounds (labelColWidth, labelRowHeight, juce::jmax (1, getWidth() - labelColWidth), juce::jmax (1, getHeight() - labelRowHeight));

        layoutLeftRail();
        layoutTopRail();
    }

    void MatrixView::updateEmptyHintVisibility()
    {
        const bool empty = inputLabels.empty() && outputLabels.empty();
        emptyHint.setVisible (empty);
        if (empty)
            emptyHint.toFront (false); // keep above the empty grid viewport
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
        const int nEnd = juce::jmin ((int) inputLabels.size(),
            (clip.getBottom() + cellSize - 1) / cellSize);

        // Hover overlay when an INPUT group is hovered in the bottom panel.
        if (!highlightedInputs.empty())
        {
            g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.18f));
            for (int n : highlightedInputs)
                if (n >= nStart && n < nEnd)
                    g.fillRect (0, n * cellSize, leftRailContent.getWidth(), cellSize);
        }

        // Multi-select highlight (stronger than the group hover so the user
        // sees their selection at a glance) -- amber band.
        if (!selectedInputs.empty())
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
        const int mEnd = juce::jmin ((int) outputLabels.size(),
            (clip.getRight() + 240) / cellSize);

        // Hover overlay
        if (!highlightedOutputs.empty())
        {
            g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.08f));
            for (int m : highlightedOutputs)
                if (m >= mStart && m < mEnd)
                    g.fillRect (m * cellSize, 0, cellSize, labelRowHeight);
        }

        // Multi-select highlight on outputs -- same amber band as inputs.
        if (!selectedOutputs.empty())
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

        // Per-column vertical labels rotated 90 deg CCW on screen.  JUCE's
        // positive rotation goes CW on screen (because the y-axis points
        // down), so we pass -halfPi to actually rotate CCW.  Two parallel
        // strips per column so the channel marker doesn't get truncated
        // when the device name is long:
        //   * Left strip (device name)   -- centered at pivotX - 8
        //   * Right strip (ch.N or "(N ch)" for collapsed) -- centered at pivotX + 8
        // Each strip is ~14 px wide on screen; with cellSize = 32 we have
        // plenty of margin so adjacent columns never overlap.
        const float textLenPx = (float) (topRailWidgetsY - 8); // vertical room above widgets
        const float pivotYRot = (float) (topRailWidgetsY - 4);

        for (int m = mStart; m < mEnd; ++m)
        {
            const auto& lbl = outputLabels[(size_t) m];

            juce::String nameText, chText;
            juce::Colour col;
            if (lbl.isCollapsedRow)
            {
                nameText = lbl.deviceName;
                chText = "(" + juce::String (lbl.channelCount) + " ch)";
                col = juce::Colour::fromRGB (0, 200, 220);
            }
            else
            {
                nameText = lbl.deviceName;
                chText = "ch." + juce::String (lbl.channelIndex);
                col = lbl.startsNewGroup
                          ? juce::Colour::fromRGB (215, 215, 220)
                          : juce::Colour::fromRGB (165, 165, 170);
            }

            const float colCx = (float) (m * cellSize) + cellSize * 0.5f;

            // Helper: draw one rotated vertical strip with text climbing
            // upward from pivot (colCx + xOffset, pivotYRot).
            auto drawVertStrip = [&] (const juce::String& text,
                                     float xOffset,
                                     float fontSize,
                                     juce::Colour textCol) {
                juce::Graphics::ScopedSaveState state (g);
                const float pivotX = colCx + xOffset;
                g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi,
                    pivotX,
                    pivotYRot));
                g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), fontSize, 0));
                g.setColour (textCol);
                g.drawText (text,
                    (int) pivotX,
                    (int) pivotYRot - 7,
                    (int) textLenPx,
                    14,
                    juce::Justification::centredLeft,
                    true);
            };

            // Left strip = device name (slightly larger).  Right strip =
            // channel marker in a muted tint so the eye reads name first.
            drawVertStrip (nameText, -8.0f, 12.0f, col);
            drawVertStrip (chText, +7.0f, 10.5f, col.darker (0.2f));
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
        // Sampled perf log -- if the message thread is starved, the meter timer
        // (configured for 30 Hz / 33 ms) will fire far less often than expected.
        // We log every Nth tick with the wall-clock gap since the previous log
        // so we can see the actual cadence.
        {
            static thread_local int counter = 0;
            static thread_local juce::uint32 lastMs = 0;
            if ((counter++ % 60) == 0)
            {
                const auto now = juce::Time::getMillisecondCounter();
                if (lastMs != 0)
                    juce::Logger::writeToLog ("MatrixView::timer #" + juce::String (counter)
                                              + " 60 ticks in " + juce::String (now - lastMs)
                                              + " ms (expected ~"
                                              + juce::String (60 * 1000
                                                              / juce::jmax (1, engine.getSettings().meterTimerHz))
                                              + ")");
                lastMs = now;
            }
        }

        // Resync trim/mute/solo sliders to the engine when something changed them
        // that the user didn't drag directly (VCA group fader riding its members,
        // multi-select links, snapshot restore).  Gated on the matrix dirty
        // generation so a static matrix adds nothing to the meter tick.
        const uint64_t gen = engine.getRoutingMatrix().getDirtyGeneration();
        if (gen != lastTrimRefreshGen)
        {
            lastTrimRefreshGen = gen;
            refreshTrimWidgetsFromEngine();
        }

        const float decay = engine.getSettings().meterDecayFactor;
        const int nIn = (int) inputMeters.size();
        const int nOut = (int) outputMeters.size();
        for (int n = 0; n < nIn; ++n)
        {
            if (inputMeters[n] == nullptr)
                continue; // collapsed row
            const int engCh = n < (int) inputLabels.size()
                                  ? inputLabels[(size_t) n].firstChannel
                                  : n;
            inputMeters[n]->pushPeak (engine.getInputPeak (engCh));
            inputMeters[n]->tickDecay (decay);
        }
        for (int m = 0; m < nOut; ++m)
        {
            if (outputMeters[m] == nullptr)
                continue; // collapsed column
            const int engOut = m < (int) outputLabels.size()
                                   ? outputLabels[(size_t) m].firstChannel
                                   : m;
            outputMeters[m]->pushPeak (engine.getOutputPeak (engOut));
            outputMeters[m]->tickDecay (decay);
        }
    }

    static PluginHost* getHost (AudioEngine& e, bool isInput, int ch)
    {
        return isInput ? e.getInputPluginHost (ch) : e.getPluginHost (ch);
    }

    // Walk the label list to find which visible row/column corresponds to a
    // given engine channel.  Returns -1 if the channel lives inside a
    // collapsed device row (no per-channel widget exists).
    static int findLabelIdxForEngineCh (const std::vector<MatrixView::ChannelLabel>& labels,
        int engCh)
    {
        for (int i = 0; i < (int) labels.size(); ++i)
        {
            const auto& l = labels[(size_t) i];
            if (l.isCollapsedRow)
                continue;
            if (l.firstChannel == engCh)
                return i;
        }
        return -1;
    }

    void MatrixView::updateFxButtonAppearance (bool isInput, int engCh)
    {
        const auto& labels = isInput ? inputLabels : outputLabels;
        auto& btns = isInput ? inputFxBtns : outputFxBtns;
        const int labelIdx = findLabelIdxForEngineCh (labels, engCh);
        if (labelIdx < 0 || labelIdx >= btns.size())
            return;
        auto* btn = btns[labelIdx];
        if (btn == nullptr)
            return; // collapsed-row safety net

        auto* host = getHost (engine, isInput, engCh);

        // Color states (user-specified):
        //  - no plugins        : default dark grey (unchanged look)
        //  - any working plugin: cyan
        //  - plugins loaded but every one bypassed: dim red
        juce::Colour col (50, 50, 56);
        if (host && host->anyLoaded())
            col = host->anyActive() ? juce::Colour::fromRGB (0, 170, 200)
                                    : juce::Colour::fromRGB (90, 28, 28);
        btn->setColour (juce::TextButton::buttonColourId, col);
        btn->setColour (juce::TextButton::buttonOnColourId, col);
        btn->setButtonText ("FX");
    }

    void MatrixView::openFxMenuFor (bool isInput, int engCh)
    {
        auto* host = getHost (engine, isInput, engCh);
        if (host == nullptr)
            return;

        const auto& labels = isInput ? inputLabels : outputLabels;
        auto& btns = isInput ? inputFxBtns : outputFxBtns;
        const int labelIdx = findLabelIdxForEngineCh (labels, engCh);
        if (labelIdx < 0 || labelIdx >= btns.size() || btns[labelIdx] == nullptr)
            return;

        // Build the broadcast target list IN ENGINE CHANNEL space.  Selection
        // is stored as label indices; convert each selected label to its
        // engine channel and drop collapsed-row entries.
        std::vector<int> targets;
        auto selLabels = getSelectedChannels (isInput);
        if (selLabels.size() > 1
            && std::find (selLabels.begin(), selLabels.end(), labelIdx) != selLabels.end())
        {
            for (int li : selLabels)
            {
                if (li < 0 || li >= (int) labels.size())
                    continue;
                if (labels[(size_t) li].isCollapsedRow)
                    continue;
                targets.push_back (labels[(size_t) li].firstChannel);
            }
        }
        else
        {
            targets = { engCh };
        }

        auto popup = std::make_unique<FxChainPopupContent> (*this, isInput, engCh, std::move (targets));
        auto bounds = btns[labelIdx]->getScreenBounds();
        juce::CallOutBox::launchAsynchronously (std::move (popup), bounds, nullptr);
    }

    // ============================================================================
    // Channel-header multi-select
    // ============================================================================
    void MatrixView::handleChannelHeaderClick (bool isInput, int ch, const juce::ModifierKeys& mods)
    {
        auto& sel = isInput ? selectedInputs : selectedOutputs;
        auto& last = isInput ? lastClickedInput : lastClickedOutput;
        const int total = isInput ? (int) inputLabels.size() : (int) outputLabels.size();
        if (ch < 0 || ch >= total)
            return;

        if (mods.isShiftDown() && last >= 0 && last < total)
        {
            // Range: replace selection with [min(last,ch) .. max(last,ch)].
            sel.clear();
            const int lo = juce::jmin (last, ch);
            const int hi = juce::jmax (last, ch);
            for (int i = lo; i <= hi; ++i)
                sel.push_back (i);
        }
        else if (mods.isCommandDown())
        {
            // Toggle membership; don't clear the rest.
            auto it = std::find (sel.begin(), sel.end(), ch);
            if (it != sel.end())
                sel.erase (it);
            else
                sel.push_back (ch);
            last = ch;
        }
        else
        {
            // Plain click: select just this channel.
            sel = { ch };
            last = ch;
        }

        if (isInput)
            leftRailContent.repaint();
        else
            topRailContent.repaint();
    }

    std::vector<int> MatrixView::getSelectedChannels (bool isInput) const
    {
        return isInput ? selectedInputs : selectedOutputs;
    }

    void MatrixView::clearChannelSelection (bool isInput)
    {
        if (isInput)
        {
            selectedInputs.clear();
            lastClickedInput = -1;
            leftRailContent.repaint();
        }
        else
        {
            selectedOutputs.clear();
            lastClickedOutput = -1;
            topRailContent.repaint();
        }
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
        const int labelBandHeight = topRailWidgetsY;
        if (e.y < labelBandHeight)
        {
            const int col = e.x / owner.cellSize;
            if (col >= 0 && col < (int) owner.outputLabels.size())
            {
                // Collapsed-device column: clicking anywhere in the rotated
                // name area also expands (the "+" button below handles its
                // own clicks).  Expanded columns: the "-" button on the
                // first-of-device column handles collapse; rotated-text
                // clicks here drive normal selection.
                const auto& lbl = owner.outputLabels[(size_t) col];
                if (lbl.isCollapsedRow)
                {
                    owner.setDeviceCollapsed (false, lbl.deviceName, false);
                    return;
                }
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
    MatrixView::FxChainPopupContent::FxChainPopupContent (MatrixView& o, bool inp, int primaryCh, std::vector<int> targetChannels)
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
            row.name.onSwap = [this] (int from, int to) {
                // Close any open editors for the two slots being swapped FIRST --
                // swapSlots moves the plugin instances between slots, and an editor
                // left open would reference a plugin that now lives in the other
                // slot, crashing on teardown / reopen.
                for (int targetCh : targets)
                {
                    owner.closeEditorFor (isInput, targetCh, from);
                    owner.closeEditorFor (isInput, targetCh, to);
                }
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
        startTimer (150); // poll so async plugin loads reflect in the popup
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
            row.name.setBounds (rr);
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
                if (row.name.getButtonText() != txt)
                    row.name.setButtonText (txt);
                row.bypass.setEnabled (true);
                row.remove.setEnabled (true);
                row.bypass.setToggleState (host->isBypassedAt (s), juce::dontSendNotification);
                row.name.setToggleState (host->isBypassedAt (s), juce::dontSendNotification);
            }
            else
            {
                if (row.name.getButtonText() != "+ insert")
                    row.name.setButtonText ("+ insert");
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
        if (primary == nullptr)
            return;
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
        for (auto& row : outputEditorWindows)
            for (auto& w : row)
                w.reset();
        for (auto& row : inputEditorWindows)
            for (auto& w : row)
                w.reset();
    }

    void MatrixView::refreshMuteButtonStates()
    {
        auto& m = engine.getRoutingMatrix();
        for (int n = 0; n < inputMuteBtns.size() && n < (int) inputLabels.size(); ++n)
        {
            if (inputMuteBtns[n] == nullptr)
                continue; // collapsed row
            const int engCh = inputLabels[(size_t) n].firstChannel;
            if (engCh >= m.getNumInputs())
                continue;
            inputMuteBtns[n]->setToggleState (m.getInputMute (engCh), juce::dontSendNotification);
        }
        for (int o = 0; o < outputMuteBtns.size() && o < (int) outputLabels.size(); ++o)
        {
            if (outputMuteBtns[o] == nullptr)
                continue; // collapsed column
            const int engOut = outputLabels[(size_t) o].firstChannel;
            if (engOut >= m.getNumOutputs())
                continue;
            outputMuteBtns[o]->setToggleState (m.getOutputMute (engOut), juce::dontSendNotification);
        }
    }

    void MatrixView::loadPluginInto (bool isInput, int ch, int slotIdx)
    {
        juce::File startDir ("/Library/Audio/Plug-Ins/Components");
        if (!startDir.isDirectory())
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
            [this, isInput, ch, slotIdx] (const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File {})
                    return;

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

                auto chooseDesc = [this, isInput, ch, slotIdx] (juce::PluginDescription desc) {
                    engine.getPluginFormatManager().createPluginInstanceAsync (
                        desc, engine.getEngineSampleRate(), engine.getEngineBlockSize(), [this, isInput, ch, slotIdx] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& error) {
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
                            if (host == nullptr)
                                return;
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
                    for (auto* d : descs)
                        copies.push_back (*d);
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
        if (channels.empty())
            return;

        // First choose a SOURCE: a built-in DSP module (instant) or an AU from
        // disk.  Built-ins live in their own submenu so they're one click away
        // and never require a file browse.
        juce::PopupMenu menu;

        juce::PopupMenu builtinMenu;
        for (const auto& d : dcr::builtin::InternalPluginFormat::getBuiltinDescriptions())
        {
            auto desc = d;
            builtinMenu.addItem (desc.name, [this, isInput, channels, slotIdx, desc] {
                instantiateAndBroadcast (isInput, channels, slotIdx, desc);
            });
        }
        menu.addSubMenu ("Built-in", builtinMenu);
        menu.addSeparator();
        menu.addItem ("Load Audio Unit from file...", [this, isInput, channels, slotIdx] {
            browseForAuAndBroadcast (isInput, channels, slotIdx);
        });

        // Anchor the menu on the clicked channel's FX button when we can find it.
        auto& btns = isInput ? inputFxBtns : outputFxBtns;
        const auto& labels = isInput ? inputLabels : outputLabels;
        int anchorLabelIdx = -1;
        for (int i = 0; i < (int) labels.size(); ++i)
            if (!labels[(size_t) i].isCollapsedRow
                && labels[(size_t) i].firstChannel == channels.front())
            {
                anchorLabelIdx = i;
                break;
            }

        if (anchorLabelIdx >= 0 && anchorLabelIdx < btns.size() && btns[anchorLabelIdx] != nullptr)
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (btns[anchorLabelIdx]));
        else
            menu.showMenuAsync (juce::PopupMenu::Options());
    }

    void MatrixView::instantiateAndBroadcast (bool isInput, std::vector<int> channels, int slotIdx, juce::PluginDescription desc)
    {
        juce::Logger::writeToLog ("FX load: broadcasting '" + desc.name + "' to "
                                  + juce::String ((int) channels.size()) + " "
                                  + juce::String (isInput ? "input" : "output")
                                  + " channel(s), slot " + juce::String (slotIdx + 1));

        // One instance per channel -- plugin instances (AU or built-in) are
        // per-host and can't be shared.  Built-ins instantiate synchronously;
        // AUs land async.  Either way the callback runs on the message thread.
        for (int targetCh : channels)
        {
            engine.getPluginFormatManager().createPluginInstanceAsync (
                desc, engine.getEngineSampleRate(), engine.getEngineBlockSize(), [this, isInput, targetCh, slotIdx] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err) {
                    if (instance == nullptr)
                    {
                        juce::Logger::writeToLog ("  ch." + juce::String (targetCh + 1)
                                                  + " load failed: " + err);
                        return;
                    }
                    auto* host = getHost (engine, isInput, targetCh);
                    if (host == nullptr)
                        return;
                    closeEditorFor (isInput, targetCh, slotIdx);
                    host->setPluginAt (slotIdx, std::move (instance));
                    updateFxButtonAppearance (isInput, targetCh);
                });
        }
    }

    void MatrixView::browseForAuAndBroadcast (bool isInput, std::vector<int> channels, int slotIdx)
    {
        juce::File startDir ("/Library/Audio/Plug-Ins/Components");
        if (!startDir.isDirectory())
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
            [this, isInput, channels, slotIdx] (const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File {})
                    return;

                // The AU format is index 0 (registered before the Internal format).
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

                if (descs.size() == 1)
                {
                    instantiateAndBroadcast (isInput, channels, slotIdx, *descs[0]);
                }
                else
                {
                    juce::PopupMenu pick;
                    std::vector<juce::PluginDescription> copies;
                    copies.reserve ((size_t) descs.size());
                    for (auto* d : descs)
                        copies.push_back (*d);
                    for (auto d : copies)
                        pick.addItem (d.name + "  -  " + d.manufacturerName,
                            [this, isInput, channels, slotIdx, d] { instantiateAndBroadcast (isInput, channels, slotIdx, d); });
                    pick.showMenuAsync (juce::PopupMenu::Options());
                }
            });
    }

    void MatrixView::showEditorFor (bool isInput, int ch, int slotIdx)
    {
        auto& wins = isInput ? inputEditorWindows : outputEditorWindows;
        if (ch < 0 || ch >= (int) wins.size())
            return;
        if (slotIdx < 0 || slotIdx >= PluginHost::kNumSlots)
            return;

        auto* host = getHost (engine, isInput, ch);
        if (host == nullptr)
            return;
        auto* plugin = host->getPluginAt (slotIdx);
        if (plugin == nullptr)
            return;

        if (wins[(size_t) ch][(size_t) slotIdx])
        {
            wins[(size_t) ch][(size_t) slotIdx]->toFront (true);
            return;
        }

        // Build a context label like "INPUT <device> ch.1 / slot 2".
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
                    if (other == ch)
                        continue;
                    auto* otherHost = getHost (engine, isInput, other);
                    if (auto* sib = otherHost ? otherHost->getPluginAt (slotIdx) : nullptr)
                        siblings.push_back (sib);
                }
            }
        }

        juce::Logger::writeToLog ("opening per-channel plugin editor [" + ctx + "] = "
                                  + plugin->getName()
                                  + (siblings.empty() ? juce::String {}
                                                      : "  (linked to " + juce::String ((int) siblings.size()) + " sibling(s))"));
        wins[(size_t) ch][(size_t) slotIdx].reset (new PluginEditorWindow (*plugin, [this, isInput, ch, slotIdx] { juce::MessageManager::callAsync ([this, isInput, ch, slotIdx] {
                                                                                                                       closeEditorFor (isInput, ch, slotIdx);
                                                                                                                   }); }, ctx, std::move (siblings)));
        juce::Logger::writeToLog ("opened per-channel plugin editor [" + ctx + "] = " + plugin->getName());
    }

    void MatrixView::closeEditorFor (bool isInput, int ch, int slotIdx)
    {
        auto& wins = isInput ? inputEditorWindows : outputEditorWindows;
        if (ch < 0 || ch >= (int) wins.size())
            return;
        if (slotIdx < 0 || slotIdx >= PluginHost::kNumSlots)
            return;
        wins[(size_t) ch][(size_t) slotIdx].reset();
    }

} // namespace dcr

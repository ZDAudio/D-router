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

MatrixView::MatrixView (AudioEngine& e) : engine (e)
{
    resumeUpdates();
}

MatrixView::~MatrixView() = default;

void MatrixView::resumeUpdates()
{
    const int hz = juce::jmax (1, engine.getSettings().meterTimerHz);
    startTimerHz (hz);
}

void MatrixView::setHighlightedOutputs (std::vector<int> outs)
{
    highlightedOutputs = std::move (outs);
    if (grid) grid->setHighlightedColumns (highlightedOutputs);
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
    s->setColour (juce::Slider::rotarySliderFillColourId,    juce::Colour::fromRGB (110, 170, 255));
    s->setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour::fromRGB (60, 60, 70));
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
    outputTrims    .clear();
    outputMeters   .clear();
    outputMuteBtns .clear();
    outputFxBtns   .clear();
    // Close any open plugin editors -- engine restart invalidates instances.
    for (auto& w : editorWindows) if (w) w->setVisible (false);
    editorWindows.clear();

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

    // Input row widgets.
    for (int n = 0; n < nIn; ++n)
    {
        auto* lbl = new juce::Label ({},
            inputLabels[(size_t) n].deviceName + "  ch." + juce::String (inputLabels[(size_t) n].channelIndex));
        lbl->setFont (juce::FontOptions (11.0f));
        lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        lbl->setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (*lbl);
        inputNames.add (lbl);

        auto* sl = makeTrimSlider();
        sl->setValue (linToDb (matrix.getInputTrim (n)), juce::dontSendNotification);
        sl->onValueChange = [&matrix, sl, n] { matrix.setInputTrim (n, dbToLin ((float) sl->getValue())); };
        addAndMakeVisible (*sl);
        inputTrims.add (sl);

        auto* m = new LevelMeter (LevelMeter::Orientation::Horizontal);
        addAndMakeVisible (*m);
        inputMeters.add (m);

        auto* mute = new juce::TextButton ("M");
        mute->setClickingTogglesState (true);
        mute->setToggleState (matrix.getInputMute (n), juce::dontSendNotification);
        mute->setColour (juce::TextButton::buttonColourId,   juce::Colour::fromRGB (50, 50, 56));
        mute->setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromRGB (210, 60, 50));
        mute->onClick = [&matrix, mute, n] { matrix.setInputMute (n, mute->getToggleState()); };
        addAndMakeVisible (*mute);
        inputMuteBtns.add (mute);

        auto* solo = new juce::TextButton ("S");
        solo->setClickingTogglesState (true);
        solo->setToggleState (matrix.getInputSolo (n), juce::dontSendNotification);
        solo->setColour (juce::TextButton::buttonColourId,   juce::Colour::fromRGB (50, 50, 56));
        solo->setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromRGB (220, 180, 30));
        solo->onClick = [&matrix, solo, n] { matrix.setInputSolo (n, solo->getToggleState()); };
        addAndMakeVisible (*solo);
        inputSoloBtns.add (solo);
    }

    // Output column widgets.
    for (int m = 0; m < nOut; ++m)
    {
        auto* sl = makeTrimSlider();
        sl->setValue (linToDb (matrix.getOutputTrim (m)), juce::dontSendNotification);
        sl->onValueChange = [&matrix, sl, m] { matrix.setOutputTrim (m, dbToLin ((float) sl->getValue())); };
        addAndMakeVisible (*sl);
        outputTrims.add (sl);

        auto* met = new LevelMeter (LevelMeter::Orientation::Vertical);
        addAndMakeVisible (*met);
        outputMeters.add (met);

        auto* mute = new juce::TextButton ("M");
        mute->setClickingTogglesState (true);
        mute->setToggleState (matrix.getOutputMute (m), juce::dontSendNotification);
        mute->setColour (juce::TextButton::buttonColourId,   juce::Colour::fromRGB (50, 50, 56));
        mute->setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromRGB (210, 60, 50));
        mute->onClick = [&matrix, mute, m] { matrix.setOutputMute (m, mute->getToggleState()); };
        addAndMakeVisible (*mute);
        outputMuteBtns.add (mute);

        auto* fx = new juce::TextButton ("FX");
        fx->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (50, 50, 56));
        fx->onClick = [this, m] { openFxMenuFor (m); };
        addAndMakeVisible (*fx);
        outputFxBtns.add (fx);
    }
    editorWindows.resize ((size_t) nOut);
    for (int m = 0; m < nOut; ++m) updateFxButtonAppearance (m);

    // Single crosspoint grid (replaces 262k-Component approach).
    grid = std::make_unique<CrosspointGrid> (matrix);
    grid->setDimensions (nIn, nOut, cellSize);
    addAndMakeVisible (*grid);

    const int width  = labelColWidth + nOut * cellSize + 12;
    const int height = labelRowHeight + nIn  * cellSize + 12;
    setSize (width, height);
    resized();
    repaint();
}

void MatrixView::resized()
{
    const int x0 = labelColWidth;
    const int y0 = labelRowHeight;

    // Input rows (left rail).
    for (int n = 0; n < (int) inputNames.size(); ++n)
    {
        const int yy = y0 + n * cellSize;
        inputNames   [n]->setBounds (8,   yy + 2,  100, cellSize - 4);
        inputTrims   [n]->setBounds (108, yy + 2,  32,  32);
        inputMeters  [n]->setBounds (144, yy + 12, 92,  12);
        inputMuteBtns[n]->setBounds (240, yy + 7,  18,  22);
        inputSoloBtns[n]->setBounds (260, yy + 7,  18,  22);
    }

    // Output columns (top rail).
    for (int m = 0; m < (int) outputTrims.size(); ++m)
    {
        const int xx = x0 + m * cellSize;
        outputTrims   [m]->setBounds (xx + 2,  60,  32, 32);
        outputMeters  [m]->setBounds (xx + 11, 96,  14, 42);
        outputMuteBtns[m]->setBounds (xx + 9,  140, 18, 16);
        outputFxBtns  [m]->setBounds (xx + 5,  160, 26, 18);
    }

    // The single crosspoint grid.
    if (grid)
        grid->setBounds (x0, y0,
                         grid->getNumOuts() * cellSize,
                         grid->getNumIns()  * cellSize);
}

void MatrixView::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (24, 24, 28));

    const int x0 = labelColWidth;
    const int y0 = labelRowHeight;

    // Group separators - inputs
    g.setColour (juce::Colour::fromRGB (50, 50, 60));
    for (size_t n = 0; n < inputLabels.size(); ++n)
        if (inputLabels[n].startsNewGroup)
            g.drawLine (0.0f, (float) (y0 + (int) n * cellSize),
                        (float) getWidth(), (float) (y0 + (int) n * cellSize), 1.0f);
    for (size_t m = 0; m < outputLabels.size(); ++m)
        if (outputLabels[m].startsNewGroup)
            g.drawLine ((float) (x0 + (int) m * cellSize), 0.0f,
                        (float) (x0 + (int) m * cellSize), (float) getHeight(), 1.0f);

    // Output column names (rotated -45 deg) in top area.
    g.setColour (juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (11.0f));
    for (size_t m = 0; m < outputLabels.size(); ++m)
    {
        const int xx = x0 + (int) m * cellSize;
        const auto& lbl = outputLabels[m];
        juce::String text = lbl.deviceName + " ch." + juce::String (lbl.channelIndex);

        juce::Graphics::ScopedSaveState state (g);
        const float cx = (float) xx + cellSize * 0.5f;
        const float cy = 52.0f;
        g.addTransform (juce::AffineTransform::rotation (-0.6f, cx, cy));
        g.drawText (text,
                    (int) cx, (int) cy - 8,
                    140, 16,
                    juce::Justification::centredLeft, true);
    }

    // Corner cell
    g.setColour (juce::Colour::fromRGB (35, 35, 42));
    g.fillRect (0, 0, labelColWidth, labelRowHeight);
    g.setColour (juce::Colours::lightgrey);
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText ("IN  \\  OUT", 8, 8, labelColWidth - 16, 16,
                juce::Justification::topLeft, true);

    // Highlight overlay on output column headers (top rail) when a group is
    // hovered in the bottom panel.
    if (! highlightedOutputs.empty())
    {
        g.setColour (juce::Colour::fromRGBA (255, 220, 80, 64));
        for (int m : highlightedOutputs)
        {
            if (m < 0 || m >= (int) outputLabels.size()) continue;
            const int xx = x0 + m * cellSize;
            g.fillRect (xx, 0, cellSize, y0);
        }
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

void MatrixView::updateFxButtonAppearance (int m)
{
    if (m < 0 || m >= (int) outputFxBtns.size()) return;
    auto* host = engine.getPluginHost (m);
    auto* btn = outputFxBtns[m];
    if (host == nullptr || host->getPlugin() == nullptr)
    {
        btn->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (50, 50, 56));
        btn->setButtonText ("FX");
    }
    else if (host->isBypassed())
    {
        btn->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (180, 140, 30));
        btn->setButtonText ("FX");
    }
    else
    {
        btn->setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (60, 140, 80));
        btn->setButtonText ("FX");
    }
}

void MatrixView::openFxMenuFor (int m)
{
    auto* host = engine.getPluginHost (m);
    if (host == nullptr) return;

    juce::PopupMenu menu;
    const bool loaded = host->getPlugin() != nullptr;

    menu.addItem ("Load plugin...", true, false, [this, m] { loadPluginInto (m); });
    menu.addSeparator();
    menu.addItem ("Show editor",
                  loaded,
                  false,
                  [this, m] { showEditorFor (m); });
    menu.addItem ("Bypass",
                  loaded,
                  loaded && host->isBypassed(),
                  [this, m]
                  {
                      auto* h = engine.getPluginHost (m);
                      if (h) h->setBypassed (! h->isBypassed());
                      updateFxButtonAppearance (m);
                  });
    menu.addSeparator();
    menu.addItem ("Remove",
                  loaded,
                  false,
                  [this, m]
                  {
                      closeEditorFor (m);
                      if (auto* h = engine.getPluginHost (m)) h->clearPlugin();
                      updateFxButtonAppearance (m);
                  });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (outputFxBtns[m]));
}

void MatrixView::loadPluginInto (int m)
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
        [this, m] (const juce::FileChooser& fc)
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

            // Pick first (.component usually contains one AU); if multiple,
            // show a popup to choose.
            auto chooseDesc = [this, m] (juce::PluginDescription desc)
            {
                engine.getPluginFormatManager().createPluginInstanceAsync (
                    desc, engine.getEngineSampleRate(), engine.getEngineBlockSize(),
                    [this, m] (std::unique_ptr<juce::AudioPluginInstance> instance,
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
                        auto* host = engine.getPluginHost (m);
                        if (host == nullptr) return;
                        closeEditorFor (m);
                        host->setPlugin (std::move (instance));
                        updateFxButtonAppearance (m);
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
                    pick.addItem (d.name + "  -  " + d.manufacturerName, [d, chooseDesc] { chooseDesc (d); });
                }
                pick.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (outputFxBtns[m]));
            }
        });
}

void MatrixView::showEditorFor (int m)
{
    if (m < 0 || m >= (int) editorWindows.size()) return;
    auto* host = engine.getPluginHost (m);
    if (host == nullptr) return;
    auto* plugin = host->getPlugin();
    if (plugin == nullptr) return;

    if (editorWindows[(size_t) m])
    {
        editorWindows[(size_t) m]->toFront (true);
        return;
    }

    editorWindows[(size_t) m].reset (new PluginEditorWindow (*plugin,
        [this, m]
        {
            // Defer the window deletion until after the close button handler returns.
            juce::MessageManager::callAsync ([this, m] { closeEditorFor (m); });
        }));
}

void MatrixView::closeEditorFor (int m)
{
    if (m < 0 || m >= (int) editorWindows.size()) return;
    editorWindows[(size_t) m].reset();
}

} // namespace dcr

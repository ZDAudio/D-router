#include "UI/OutputGroupPanel.h"

#include "DSP/Builtin/InternalPluginFormat.h"
#include "DSP/MultiChannelPluginHost.h"
#include "Engine/AudioEngine.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"
#include "Routing/InputGroupManager.h"
#include "UI/PluginEditorWindow.h"

namespace dcr {

namespace
{
    constexpr int cardW = 250;
    constexpr int slotH = 22;
}

// ============================================================================
// Direction-aware accessor dispatch
// ============================================================================
int OutputGroupPanel::mgrNumGroups() const
{
    return direction == Direction::Inputs
        ? engine.getInputGroupManager().getNumGroups()
        : engine.getGroupManager().getNumGroups();
}
OutputGroup* OutputGroupPanel::mgrGetGroup (int idx)
{
    return direction == Direction::Inputs
        ? engine.getInputGroupManager().getGroup (idx)
        : engine.getGroupManager().getGroup (idx);
}
const OutputGroup* OutputGroupPanel::mgrGetGroup (int idx) const
{
    return direction == Direction::Inputs
        ? engine.getInputGroupManager().getGroup (idx)
        : engine.getGroupManager().getGroup (idx);
}
int OutputGroupPanel::mgrCreateGroup (juce::String name, juce::AudioChannelSet cs)
{
    return direction == Direction::Inputs
        ? engine.getInputGroupManager().createGroup (std::move (name), cs)
        : engine.getGroupManager().createGroup (std::move (name), cs);
}
void OutputGroupPanel::mgrRemoveGroup (int idx)
{
    if (direction == Direction::Inputs) engine.getInputGroupManager().removeGroup (idx);
    else                                engine.getGroupManager().removeGroup (idx);
}
void OutputGroupPanel::mgrAssignChannel (int gIdx, int slot, int globalCh)
{
    if (direction == Direction::Inputs) engine.getInputGroupManager().assignChannel (gIdx, slot, globalCh);
    else                                engine.getGroupManager().assignChannel (gIdx, slot, globalCh);
}
void OutputGroupPanel::mgrMoveFader (int gIdx, float db)
{
    if (direction == Direction::Inputs)
        engine.getInputGroupManager().moveGroupFader (gIdx, db, engine.getRoutingMatrix());
    else
        engine.getGroupManager().moveGroupFader (gIdx, db, engine.getRoutingMatrix());
}
void OutputGroupPanel::mgrSetMute (int gIdx, bool m)
{
    if (direction == Direction::Inputs)
        engine.getInputGroupManager().setGroupMute (gIdx, m, engine.getRoutingMatrix());
    else
        engine.getGroupManager().setGroupMute (gIdx, m, engine.getRoutingMatrix());
}
float OutputGroupPanel::srcPeak (int globalCh) const
{
    return direction == Direction::Inputs
        ? engine.getInputPeak (globalCh)
        : engine.getOutputPeak (globalCh);
}
juce::String OutputGroupPanel::resolveChannelName (int globalCh) const
{
    int idx = 0;
    for (auto& d : engine.getDeviceInfo())
    {
        const int n = (direction == Direction::Inputs) ? d.numInputChannels : d.numOutputChannels;
        for (int ci = 0; ci < n; ++ci, ++idx)
            if (idx == globalCh) return d.name + " " + juce::String (ci + 1);
    }
    return "?";
}

// ============================================================================
OutputGroupPanel::OutputGroupPanel (AudioEngine& e, Direction dir)
    : engine (e), direction (dir)
{
    panelTitle.setText (dir == Direction::Inputs ? "INPUT GROUPS" : "OUTPUT GROUPS",
                        juce::dontSendNotification);
    panelTitle.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold));
    panelTitle.setColour (juce::Label::textColourId,
                          juce::Colour (0xFF000000u | engine.getSettings().accentColorRGB));
    addAndMakeVisible (panelTitle);

    popOutBtn.setName ("win");
    popOutBtn.setTooltip ("Pop panel out into its own window");
    popOutBtn.onClick = [this] { if (onPopOutRequested) onPopOutRequested(); };
    addAndMakeVisible (popOutBtn);

    addAndMakeVisible (cardsViewport);
    cardsViewport.setViewedComponent (&cardsHolder, false);
    cardsViewport.setScrollBarsShown (false, true);   // horizontal only

    hoverHintLabel.setText ("Hover-to-highlight requires this panel to be popped out so the matrix is visible alongside it.",
                            juce::dontSendNotification);
    hoverHintLabel.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.0f, 0));
    hoverHintLabel.setJustificationType (juce::Justification::centred);
    hoverHintLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (120, 120, 130));
    addAndMakeVisible (hoverHintLabel);

    startTimerHz (engine.getSettings().meterTimerHz);
}

OutputGroupPanel::~OutputGroupPanel() = default;

void OutputGroupPanel::setDetached (bool d)
{
    detached = d;
    popOutBtn.setButtonText (d ? "<-" : "->");
    popOutBtn.setTooltip (d ? "Dock panel back into main window"
                            : "Pop panel out into its own window");
    hoverHintLabel.setVisible (! d);  // hint only meaningful when embedded
    resized();
    // Clear any leftover matrix highlight from before the toggle.
    if (onGroupHover) onGroupHover ({});
    lastHoveredCardIdx = -1;
}

void OutputGroupPanel::visibilityChanged()
{
    if (isVisible())
        startTimerHz (juce::jmax (1, engine.getSettings().meterTimerHz));
    else
        stopTimer();
}

void OutputGroupPanel::resumeUpdates()
{
    if (isVisible())
        startTimerHz (juce::jmax (1, engine.getSettings().meterTimerHz));
}

void OutputGroupPanel::closeAllPluginEditors()
{
    for (auto* card : cards)
        for (auto& w : card->editorWindows)
            w.reset();
}

// ============================================================================
// Card
// ============================================================================
void OutputGroupPanel::Card::buildFor (OutputGroupPanel& p, int gIdx)
{
    panel    = &p;
    groupIdx = gIdx;
    auto* g = p.mgrGetGroup (gIdx);
    if (g == nullptr) return;

    name.setText (g->name, juce::dontSendNotification);
    name.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 11.5f, juce::Font::bold));
    name.setColour (juce::Label::textColourId, juce::Colour::fromRGB (0, 255, 210));
    name.setJustificationType (juce::Justification::centredLeft);

    juce::String memStr;
    for (size_t i = 0; i < g->memberChannels.size(); ++i)
    {
        if (i > 0) memStr << ", ";
        const int ch = g->memberChannels[i];
        memStr << (ch < 0 ? "-" : p.resolveChannelName (ch));
    }
    members.setText (memStr, juce::dontSendNotification);
    members.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 9.0f, 0));
    members.setColour (juce::Label::textColourId, juce::Colour::fromRGB (140, 140, 145));

    // Range goes one notch below -60 (visible as "-inf") so the user can drag
    // the fader to a true silence position; OutputGroupManager's dB->linear
    // already clamps values <= -60 to gain 0, so anywhere in (-inf .. -60]
    // mutes the group's member channels.
    fader.setRange (-65.0, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (0.0);
    fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 14);   // editable numeric
    fader.setDoubleClickReturnValue (true, 0.0);                          // double-click resets to 0 dB
    fader.textFromValueFunction = [] (double v)
    {
        if (v <= -60.0) return juce::String ("-inf");
        if (std::abs (v) < 0.05) return juce::String ("0.0 dB");
        return juce::String (v, 1) + " dB";
    };
    fader.valueFromTextFunction = [] (const juce::String& s) -> double
    {
        auto t = s.trim().toLowerCase();
        if (t.contains ("inf") || t == "mute") return -65.0;
        return t.removeCharacters (" dB").getDoubleValue();
    };
    fader.setValue (g->faderDb.load(), juce::dontSendNotification);
    fader.updateText();   // force initial numeric to render instead of "..."
    fader.onValueChange = [this, gIdx]
    {
        panel->mgrMoveFader (gIdx, (float) fader.getValue());
    };

    mute.setName ("mute");
    mute.setClickingTogglesState (true);
    mute.setToggleState (g->muted.load(), juce::dontSendNotification);
    mute.onClick = [this, gIdx]
    {
        panel->mgrSetMute (gIdx, mute.getToggleState());
    };

    addAndMakeVisible (name);
    addAndMakeVisible (fader);
    addAndMakeVisible (mute);
    addAndMakeVisible (meter);
    addAndMakeVisible (members);

    for (int s = 0; s < (int) slots.size(); ++s)
    {
        auto row = std::make_unique<SlotRow>();
        row->slotIdx = s;

        row->bypass.setName ("bypass");
        row->bypass.setClickingTogglesState (true);
        row->bypass.setTooltip ("Bypass this slot");
        row->bypass.onClick = [this, s]
        {
            if (auto* h = getSlotHost (s))
            {
                h->setBypassed (slots[(size_t) s]->bypass.getToggleState());
                refreshSlotAppearance (s);
            }
        };

        row->name.setName ("slot");
        row->name.setTooltip ("Click: load / open editor.  Drag: reorder.");
        row->name.setButtonText ("+ insert");
        row->name.slotIdx = s;
        row->name.onClick = [this, s] { onSlotNameClicked (s); };
        row->name.onSwap  = [this] (int from, int to)
        {
            auto* a = getSlotHost (from);
            auto* b = getSlotHost (to);
            if (a != nullptr && b != nullptr)
            {
                a->swapStateWith (*b);
                refreshSlotAppearance (from);
                refreshSlotAppearance (to);
            }
        };

        row->remove.setName ("remove");
        row->remove.setTooltip ("Remove plugin");
        row->remove.onClick = [this, s] { onSlotRemoveClicked (s); };

        addAndMakeVisible (row->bypass);
        addAndMakeVisible (row->name);
        addAndMakeVisible (row->remove);
        slots[(size_t) s] = std::move (row);
        refreshSlotAppearance (s);
    }
}

MultiChannelPluginHost* OutputGroupPanel::Card::getSlotHost (int slotIdx) const
{
    auto* g = panel->mgrGetGroup (groupIdx);
    if (g == nullptr) return nullptr;
    if (slotIdx < 0 || slotIdx >= (int) g->pluginSlots.size()) return nullptr;
    return g->pluginSlots[(size_t) slotIdx].get();
}

void OutputGroupPanel::Card::refreshSlotAppearance (int slotIdx)
{
    auto* host = getSlotHost (slotIdx);
    auto& row  = *slots[(size_t) slotIdx];
    const bool loaded = host && host->getPlugin();

    if (loaded)
    {
        const float cpu = host->getCpuLoadAvg() * 100.0f;
        const auto newText = host->getPlugin()->getName()
                              + "   " + juce::String (cpu, 1) + "%";
        if (row.name.getButtonText() != newText)
            row.name.setButtonText (newText);
        row.bypass.setToggleState (host->isBypassed(), juce::dontSendNotification);
        row.bypass.setEnabled (true);
        row.remove.setEnabled (true);
        row.name.setToggleState (host->isBypassed(), juce::dontSendNotification);
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

void OutputGroupPanel::Card::onSlotNameClicked (int slotIdx)
{
    auto* host = getSlotHost (slotIdx);
    if (host && host->getPlugin()) openEditorFor (slotIdx);
    else                            panel->requestLoadPlugin (groupIdx, slotIdx);
}

void OutputGroupPanel::Card::onSlotRemoveClicked (int slotIdx)
{
    closeEditorFor (slotIdx);
    if (auto* host = getSlotHost (slotIdx)) host->clearPlugin();
    refreshSlotAppearance (slotIdx);
}

void OutputGroupPanel::Card::openEditorFor (int slotIdx)
{
    auto* host = getSlotHost (slotIdx);
    if (host == nullptr || host->getPlugin() == nullptr) return;
    if (editorWindows[(size_t) slotIdx])
    {
        editorWindows[(size_t) slotIdx]->toFront (true);
        return;
    }
    // Title prefix identifies which group / slot owns this editor.
    juce::String ctx;
    if (auto* g = panel->mgrGetGroup (groupIdx))
    {
        ctx << (panel->getDirection() == Direction::Inputs ? "INPUT GROUP " : "OUTPUT GROUP ");
        ctx << g->name;
        ctx << "  /  slot " << juce::String (slotIdx + 1);
    }
    // Log BEFORE the editor ctor -- if the plugin's editor takes us down
    // hard, the log file still contains the last line we wrote and tells
    // the user exactly which plugin to blame.
    const auto pname = host->getPlugin()->getName();
    juce::Logger::writeToLog ("opening group plugin editor [" + ctx + "] = " + pname);
    editorWindows[(size_t) slotIdx].reset (new PluginEditorWindow (
        *host->getPlugin(),
        [this, slotIdx] { juce::MessageManager::callAsync ([this, slotIdx] { closeEditorFor (slotIdx); }); },
        ctx));
    juce::Logger::writeToLog ("opened group plugin editor [" + ctx + "] = " + pname);
}

void OutputGroupPanel::Card::closeEditorFor (int slotIdx)
{
    editorWindows[(size_t) slotIdx].reset();
}

void OutputGroupPanel::Card::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (2.0f);

    g.setColour (juce::Colour::fromRGB (20, 20, 24));
    g.fillRect (bounds);

    g.setColour (juce::Colour::fromRGB (40, 40, 48));
    g.drawRect (bounds, 1.0f);
}

void OutputGroupPanel::Card::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (18);
    name.setBounds (top);
    r.removeFromTop (4);

    // Wider left column so the fader's text box can display "0.0 dB"
    // instead of getting truncated to "...".
    auto leftCol = r.removeFromLeft (110);
    fader.setBounds (leftCol.removeFromLeft (50).withTrimmedTop (4).withTrimmedBottom (4));
    leftCol.removeFromLeft (4);
    meter.setBounds (leftCol.removeFromLeft (14).withTrimmedTop (4).withTrimmedBottom (4));
    leftCol.removeFromLeft (4);
    mute.setBounds (leftCol.removeFromTop (20));
    r.removeFromLeft (4);

    // Right column: 5 slot rows.  Guard against being called before buildFor()
    // has populated the slots.
    int y = r.getY();
    for (int s = 0; s < (int) slots.size(); ++s)
    {
        if (! slots[(size_t) s]) { y += slotH; continue; }
        auto& row = *slots[(size_t) s];
        const int x = r.getX();
        const int w = r.getWidth();
        row.bypass.setBounds (x,            y, 18, slotH - 2);
        row.name  .setBounds (x + 20,       y, w - 40, slotH - 2);
        row.remove.setBounds (x + w - 18,   y, 18, slotH - 2);
        y += slotH;
    }
    members.setBounds (r.getX(), y + 2, r.getWidth(), 14);
}

// ============================================================================
// Panel
// ============================================================================
void OutputGroupPanel::rebuild()
{
    cards.clear();
    const int n = mgrNumGroups();
    for (int i = 0; i < n; ++i)
    {
        auto* c = new Card();
        c->buildFor (*this, i);                // populate slots before any setBounds
        cardsHolder.addAndMakeVisible (*c);    // cards live inside the scrolled holder
        cards.add (c);
    }
    resized();
    repaint();
}

void OutputGroupPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (10, 10, 12)); // Deep black-gray
    g.setColour (juce::Colour::fromRGB (36, 36, 42)); // Clean tech boundary border
    g.drawRect (getLocalBounds(), 1);

    if (cards.isEmpty())
    {
        g.setColour (juce::Colour::fromRGB (130, 130, 140));
        g.setFont (juce::FontOptions (12.0f));
        g.drawText ("No output groups - click \"Groups...\" to create one.",
                    getLocalBounds(), juce::Justification::centred);
    }
}

void OutputGroupPanel::resized()
{
    auto r = getLocalBounds().reduced (4, 4);

    // Header row: title left, single pop-out button right.
    auto header = r.removeFromTop (20);
    popOutBtn.setBounds (header.removeFromRight (32));
    panelTitle.setBounds (header);
    r.removeFromTop (4);

    if (hoverHintLabel.isVisible())
    {
        hoverHintLabel.setBounds (r.removeFromBottom (16));
    }
    cardsViewport.setBounds (r);

    // cardsHolder shrinks scrollbar height off the available area so the
    // bottom of each card stays visible when h-scrollbar appears.
    const int hScrollBarH = cardsViewport.isHorizontalScrollBarShown() ? 14 : 0;
    const int holderH = juce::jmax (60, r.getHeight() - hScrollBarH);
    cardsHolder.setSize (juce::jmax (cards.size() * cardW, r.getWidth()), holderH);

    for (int i = 0; i < cards.size(); ++i)
        cards[i]->setBounds (i * cardW, 0, cardW, holderH);
}

void OutputGroupPanel::timerCallback()
{
    int hoverIdx = -1;

    for (int i = 0; i < cards.size(); ++i)
    {
        auto* g = mgrGetGroup (cards[i]->groupIdx);
        if (g == nullptr) continue;

        float pk = 0.0f;
        for (int ch : g->memberChannels)
            if (ch >= 0) pk = juce::jmax (pk, srcPeak (ch));
        cards[i]->meter.pushPeak (pk);
        cards[i]->meter.tickDecay (engine.getSettings().meterDecayFactor);

        const float curDb = g->faderDb.load();
        if (! cards[i]->fader.isMouseButtonDown()
            && std::abs ((float) cards[i]->fader.getValue() - curDb) > 0.001f)
            cards[i]->fader.setValue (curDb, juce::dontSendNotification);
        cards[i]->mute.setToggleState (g->muted.load(), juce::dontSendNotification);

        if (hoverIdx < 0 && cards[i]->isMouseOver (true)) hoverIdx = i;
    }

    // Per-slot CPU% text gets repainted on every setButtonText - far too
    // heavy at meter rate (30 Hz x 5 slots x N groups).  Throttle to ~6 Hz.
    if (++slotRefreshCounter >= 5)
    {
        slotRefreshCounter = 0;
        for (int i = 0; i < cards.size(); ++i)
            for (int s = 0; s < (int) cards[i]->slots.size(); ++s)
                cards[i]->refreshSlotAppearance (s);
    }

    if (hoverIdx != lastHoveredCardIdx)
    {
        lastHoveredCardIdx = hoverIdx;
        // Only fire highlight when popped out: otherwise the matrix isn't
        // visible (different tab) and the highlight goes to a hidden view.
        if (onGroupHover && detached)
        {
            if (hoverIdx >= 0)
            {
                if (auto* g = mgrGetGroup (cards[hoverIdx]->groupIdx))
                    onGroupHover (g->memberChannels);
                else
                    onGroupHover ({});
            }
            else
            {
                onGroupHover ({});
            }
        }
    }
}

void OutputGroupPanel::requestLoadPlugin (int cardIdx, int slotIdx)
{
    // Choose a source: built-in DSP (instant) or an AU from disk.
    juce::PopupMenu menu;

    juce::PopupMenu builtinMenu;
    for (const auto& d : dcr::builtin::InternalPluginFormat::getBuiltinDescriptions())
    {
        auto desc = d;
        builtinMenu.addItem (desc.name, [this, cardIdx, slotIdx, desc]
        {
            installPluginIntoGroup (cardIdx, slotIdx, desc);
        });
    }
    menu.addSubMenu ("Built-in", builtinMenu);
    menu.addSeparator();
    menu.addItem ("Load Audio Unit from file...", [this, cardIdx, slotIdx]
    {
        browseForAuIntoGroup (cardIdx, slotIdx);
    });
    menu.showMenuAsync (juce::PopupMenu::Options());
}

void OutputGroupPanel::installPluginIntoGroup (int cardIdx, int slotIdx, juce::PluginDescription desc)
{
    engine.getPluginFormatManager().createPluginInstanceAsync (
        desc, engine.getEngineSampleRate(), engine.getEngineBlockSize(),
        [this, cardIdx, slotIdx] (std::unique_ptr<juce::AudioPluginInstance> instance,
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
            auto* g = mgrGetGroup (cardIdx);
            if (g == nullptr) return;
            if (slotIdx < 0 || slotIdx >= (int) g->pluginSlots.size()) return;
            auto& host = g->pluginSlots[(size_t) slotIdx];
            if (! host) return;
            host->setPlugin (std::move (instance), g->channelSet);

            for (auto* c : cards)
                if (c->groupIdx == cardIdx) { c->refreshSlotAppearance (slotIdx); break; }
        });
}

void OutputGroupPanel::browseForAuIntoGroup (int cardIdx, int slotIdx)
{
    juce::File start ("/Library/Audio/Plug-Ins/Components");
    if (! start.isDirectory())
        start = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                    .getChildFile ("Library/Audio/Plug-Ins/Components");

    pluginFileChooser = std::make_unique<juce::FileChooser> (
        "Choose an AU plugin", start, "*.component;*.audiounit");

    pluginFileChooser->launchAsync (
        juce::FileBrowserComponent::openMode
      | juce::FileBrowserComponent::canSelectFiles
      | juce::FileBrowserComponent::canSelectDirectories,
        [this, cardIdx, slotIdx] (const juce::FileChooser& fc)
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
            installPluginIntoGroup (cardIdx, slotIdx, *descs[0]);
        });
}

} // namespace dcr

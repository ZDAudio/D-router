#include "UI/OutputGroupPanel.h"

#include "DSP/MultiChannelPluginHost.h"
#include "Engine/AudioEngine.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"
#include "UI/PluginEditorWindow.h"

namespace dcr {

namespace
{
    constexpr int cardW = 250;
    constexpr int slotH = 22;
}

OutputGroupPanel::OutputGroupPanel (AudioEngine& e) : engine (e)
{
    addAndMakeVisible (cardsViewport);
    cardsViewport.setViewedComponent (&cardsHolder, false);
    cardsViewport.setScrollBarsShown (false, true);   // horizontal only
    startTimerHz (30);
}

OutputGroupPanel::~OutputGroupPanel() = default;

// ============================================================================
// Card
// ============================================================================
void OutputGroupPanel::Card::buildFor (OutputGroupPanel& p, int gIdx)
{
    panel    = &p;
    groupIdx = gIdx;
    auto* g = p.engine.getGroupManager().getGroup (gIdx);
    if (g == nullptr) return;

    name.setText (g->name, juce::dontSendNotification);
    name.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    name.setColour (juce::Label::textColourId, juce::Colours::white);
    name.setJustificationType (juce::Justification::centredLeft);

    juce::String memStr;
    auto& info = p.engine.getDeviceInfo();
    auto resolveName = [&info] (int globalCh) -> juce::String
    {
        int idx = 0;
        for (auto& d : info)
            for (int ci = 0; ci < d.numOutputChannels; ++ci, ++idx)
                if (idx == globalCh) return d.name + " " + juce::String (ci + 1);
        return "?";
    };
    for (size_t i = 0; i < g->memberChannels.size(); ++i)
    {
        if (i > 0) memStr << ", ";
        const int ch = g->memberChannels[i];
        memStr << (ch < 0 ? "-" : resolveName (ch));
    }
    members.setText (memStr, juce::dontSendNotification);
    members.setFont (juce::FontOptions (10.0f));
    members.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 170));

    fader.setRange (-60.0, 12.0, 0.1);
    fader.setSkewFactorFromMidPoint (0.0);
    fader.setValue (g->faderDb.load(), juce::dontSendNotification);
    fader.setColour (juce::Slider::trackColourId, juce::Colour::fromRGB (110, 170, 255));
    fader.setPopupDisplayEnabled (true, true, nullptr);
    fader.setTextValueSuffix (" dB");
    fader.onValueChange = [this, gIdx]
    {
        panel->engine.getGroupManager().moveGroupFader (gIdx, (float) fader.getValue(),
                                                        panel->engine.getRoutingMatrix());
    };

    mute.setClickingTogglesState (true);
    mute.setToggleState (g->muted.load(), juce::dontSendNotification);
    mute.setColour (juce::TextButton::buttonColourId,   juce::Colour::fromRGB (50, 50, 56));
    mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromRGB (210, 60, 50));
    mute.onClick = [this, gIdx]
    {
        panel->engine.getGroupManager().setGroupMute (gIdx, mute.getToggleState(),
                                                      panel->engine.getRoutingMatrix());
    };

    popOut.setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (50, 50, 56));
    popOut.onClick = [this] { if (panel->onPopOutRequested) panel->onPopOutRequested(); };

    addAndMakeVisible (name);
    addAndMakeVisible (fader);
    addAndMakeVisible (mute);
    addAndMakeVisible (meter);
    addAndMakeVisible (members);
    addAndMakeVisible (popOut);

    for (int s = 0; s < (int) slots.size(); ++s)
    {
        auto row = std::make_unique<SlotRow>();
        row->slotIdx = s;

        row->bypass.setClickingTogglesState (true);
        row->bypass.setColour (juce::TextButton::buttonColourId,   juce::Colour::fromRGB (50, 50, 56));
        row->bypass.setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromRGB (180, 140, 30));
        row->bypass.setTooltip ("Bypass this slot");
        row->bypass.onClick = [this, s]
        {
            if (auto* h = getSlotHost (s))
            {
                h->setBypassed (slots[(size_t) s]->bypass.getToggleState());
                refreshSlotAppearance (s);
            }
        };

        row->name.setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (40, 40, 46));
        row->name.setTooltip ("Click: load / open editor");
        row->name.onClick = [this, s] { onSlotNameClicked (s); };

        row->remove.setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (50, 50, 56));
        row->remove.setTooltip ("Remove plugin");
        row->remove.onClick = [this, s] { onSlotRemoveClicked (s); };

        addAndMakeVisible (row->bypass);
        addAndMakeVisible (row->name);
        addAndMakeVisible (row->remove);
        slots[(size_t) s] = std::move (row);
        refreshSlotAppearance (s);
    }
    updatePopOut (panel->detached);
}

MultiChannelPluginHost* OutputGroupPanel::Card::getSlotHost (int slotIdx) const
{
    auto* g = panel->engine.getGroupManager().getGroup (groupIdx);
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
        row.name.setButtonText (host->getPlugin()->getName()
                                + "   " + juce::String (cpu, 1) + "%");
        row.bypass.setToggleState (host->isBypassed(), juce::dontSendNotification);
        row.bypass.setEnabled (true);
        row.remove.setEnabled (true);
        row.name.setColour (juce::TextButton::buttonColourId,
                            host->isBypassed() ? juce::Colour::fromRGB (80, 60, 30)
                                                : juce::Colour::fromRGB (40, 70, 60));
    }
    else
    {
        row.name.setButtonText ("+ insert");
        row.bypass.setToggleState (false, juce::dontSendNotification);
        row.bypass.setEnabled (false);
        row.remove.setEnabled (false);
        row.name.setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (40, 40, 46));
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
    editorWindows[(size_t) slotIdx].reset (new PluginEditorWindow (
        *host->getPlugin(),
        [this, slotIdx] { juce::MessageManager::callAsync ([this, slotIdx] { closeEditorFor (slotIdx); }); }));
}

void OutputGroupPanel::Card::closeEditorFor (int slotIdx)
{
    editorWindows[(size_t) slotIdx].reset();
}

void OutputGroupPanel::Card::updatePopOut (bool detached)
{
    popOut.setButtonText (detached ? "<-" : "->");
    popOut.setTooltip (detached ? "Dock panel back into main window"
                                 : "Pop panel out into its own window");
}

void OutputGroupPanel::Card::resized()
{
    auto r = getLocalBounds().reduced (6);
    auto top = r.removeFromTop (18);
    popOut.setBounds (top.removeFromRight (28));
    name.setBounds (top);
    r.removeFromTop (4);

    auto leftCol = r.removeFromLeft (90);
    fader.setBounds (leftCol.removeFromLeft (24).withTrimmedTop (4).withTrimmedBottom (4));
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
    auto& mgr = engine.getGroupManager();
    const int n = mgr.getNumGroups();
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
    g.fillAll (juce::Colour::fromRGB (22, 22, 26));
    g.setColour (juce::Colour::fromRGB (50, 50, 60));
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
    cardsViewport.setBounds (getLocalBounds());

    // cardsHolder shrinks scrollbar height off the available area so the
    // bottom of each card stays visible when h-scrollbar appears.
    const int hScrollBarH = cardsViewport.isHorizontalScrollBarShown() ? 14 : 0;
    const int holderH = juce::jmax (60, getHeight() - hScrollBarH);
    cardsHolder.setSize (juce::jmax (cards.size() * cardW, getWidth()), holderH);

    for (int i = 0; i < cards.size(); ++i)
        cards[i]->setBounds (i * cardW, 0, cardW, holderH);
}

void OutputGroupPanel::timerCallback()
{
    auto& mgr = engine.getGroupManager();
    int hoverIdx = -1;

    for (int i = 0; i < cards.size(); ++i)
    {
        auto* g = mgr.getGroup (cards[i]->groupIdx);
        if (g == nullptr) continue;

        float pk = 0.0f;
        for (int ch : g->memberChannels)
            if (ch >= 0) pk = juce::jmax (pk, engine.getOutputPeak (ch));
        cards[i]->meter.pushPeak (pk);
        cards[i]->meter.tickDecay (engine.getSettings().meterDecayFactor);

        const float curDb = g->faderDb.load();
        if (! cards[i]->fader.isMouseButtonDown()
            && std::abs ((float) cards[i]->fader.getValue() - curDb) > 0.001f)
            cards[i]->fader.setValue (curDb, juce::dontSendNotification);
        cards[i]->mute.setToggleState (g->muted.load(), juce::dontSendNotification);

        if (hoverIdx < 0 && cards[i]->isMouseOver (true)) hoverIdx = i;

        // Refresh per-slot CPU% text (cheap; happens at meter rate).
        for (int s = 0; s < (int) cards[i]->slots.size(); ++s)
            cards[i]->refreshSlotAppearance (s);
    }

    if (hoverIdx != lastHoveredCardIdx)
    {
        lastHoveredCardIdx = hoverIdx;
        if (onGroupHover)
        {
            if (hoverIdx >= 0)
            {
                if (auto* g = mgr.getGroup (cards[hoverIdx]->groupIdx))
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
    pendingLoadCardIdx = cardIdx;
    pendingLoadSlotIdx = slotIdx;

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
        [this] (const juce::FileChooser& fc)
        {
            const int gIdx = pendingLoadCardIdx;
            const int sIdx = pendingLoadSlotIdx;
            pendingLoadCardIdx = pendingLoadSlotIdx = -1;

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
            auto desc = *descs[0];
            engine.getPluginFormatManager().createPluginInstanceAsync (
                desc, engine.getEngineSampleRate(), engine.getEngineBlockSize(),
                [this, gIdx, sIdx] (std::unique_ptr<juce::AudioPluginInstance> instance,
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
                    auto* g = engine.getGroupManager().getGroup (gIdx);
                    if (g == nullptr) return;
                    if (sIdx < 0 || sIdx >= (int) g->pluginSlots.size()) return;
                    auto& host = g->pluginSlots[(size_t) sIdx];
                    if (! host) return;
                    host->setPlugin (std::move (instance), g->channelSet);

                    // Refresh card UI.
                    for (auto* c : cards)
                        if (c->groupIdx == gIdx) { c->refreshSlotAppearance (sIdx); break; }
                });
        });
}

} // namespace dcr

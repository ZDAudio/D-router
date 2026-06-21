#include "UI/GroupManagerDialog.h"

#include "Routing/InputGroupManager.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"

namespace dcr
{

    namespace
    {
        struct LayoutOption
        {
            juce::String name;
            juce::AudioChannelSet channelSet;
        };

        std::vector<LayoutOption> getLayoutOptions()
        {
            return {
                { "Mono", juce::AudioChannelSet::mono() },
                { "Stereo", juce::AudioChannelSet::stereo() },
                { "Quad", juce::AudioChannelSet::quadraphonic() },
                { "5.1", juce::AudioChannelSet::create5point1() },
                { "7.1", juce::AudioChannelSet::create7point1() },
                { "7.1.2", juce::AudioChannelSet::create7point1point2() },
                { "7.1.4", juce::AudioChannelSet::create7point1point4() },
            };
        }

        // Build (deviceName, ch.X) labels in same order as AudioEngine's global
        // channel index, on whichever direction is requested.
        struct ChannelLabel
        {
            int globalIdx;
            juce::String text;
        };

        std::vector<ChannelLabel> buildChannelLabels (const AudioEngine& engine,
            GroupManagerDialog::Direction dir)
        {
            std::vector<ChannelLabel> out;
            int idx = 0;
            for (auto& d : engine.getDeviceInfo())
            {
                const int n = (dir == GroupManagerDialog::Direction::Inputs)
                                  ? d.numInputChannels
                                  : d.numOutputChannels;
                for (int c = 0; c < n; ++c, ++idx)
                    out.push_back ({ idx, d.name + "  ch." + juce::String (c + 1) });
            }
            return out;
        }
    }

    // =========================================================================
    // Direction dispatch
    // =========================================================================
    int GroupManagerDialog::dGetNumGroups() const
    {
        return direction == Direction::Inputs
                   ? engine.getInputGroupManager().getNumGroups()
                   : engine.getGroupManager().getNumGroups();
    }
    OutputGroup* GroupManagerDialog::dGetGroup (int idx)
    {
        return direction == Direction::Inputs
                   ? engine.getInputGroupManager().getGroup (idx)
                   : engine.getGroupManager().getGroup (idx);
    }
    int GroupManagerDialog::dCreateGroup (juce::String name, juce::AudioChannelSet cs)
    {
        return direction == Direction::Inputs
                   ? engine.getInputGroupManager().createGroup (std::move (name), cs)
                   : engine.getGroupManager().createGroup (std::move (name), cs);
    }
    void GroupManagerDialog::dRemoveGroup (int idx)
    {
        if (direction == Direction::Inputs)
            engine.getInputGroupManager().removeGroup (idx);
        else
            engine.getGroupManager().removeGroup (idx);
    }
    void GroupManagerDialog::dAssignChannel (int gIdx, int slot, int globalCh)
    {
        if (direction == Direction::Inputs)
            engine.getInputGroupManager().assignChannel (gIdx, slot, globalCh);
        else
            engine.getGroupManager().assignChannel (gIdx, slot, globalCh);
    }

    GroupManagerDialog::GroupManagerDialog (AudioEngine& e, Direction dir)
        : engine (e), direction (dir)
    {
        setSize (760, 540);

        title.setText ("GROUPS", juce::dontSendNotification);
        title.setFont (juce::FontOptions (16.0f, juce::Font::bold));
        addAndMakeVisible (title);

        createBtn.setTooltip ("Create new group in the currently selected side");
        deleteBtn.setTooltip ("Delete the selected group");

        inSideBtn.setClickingTogglesState (true);
        outSideBtn.setClickingTogglesState (true);
        inSideBtn.setRadioGroupId (911);
        outSideBtn.setRadioGroupId (911);
        inSideBtn.setToggleState (direction == Direction::Inputs, juce::dontSendNotification);
        outSideBtn.setToggleState (direction == Direction::Outputs, juce::dontSendNotification);
        inSideBtn.onClick = [this] { if (inSideBtn .getToggleState()) setDirection (Direction::Inputs); };
        outSideBtn.onClick = [this] { if (outSideBtn.getToggleState()) setDirection (Direction::Outputs); };
        addAndMakeVisible (inSideBtn);
        addAndMakeVisible (outSideBtn);

        groupList.setModel (&listModel);
        groupList.setRowHeight (28);
        groupList.setColour (juce::ListBox::backgroundColourId, juce::Colour::fromRGB (22, 22, 26));
        addAndMakeVisible (groupList);

        createBtn.onClick = [this] { onCreateClicked(); };
        deleteBtn.onClick = [this] { onDeleteClicked(); };
        doneBtn.onClick = [this] {
            if (onClose)
                onClose();
            if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                dw->exitModalState (1);
        };
        addAndMakeVisible (createBtn);
        addAndMakeVisible (deleteBtn);
        addAndMakeVisible (doneBtn);

        nameLbl.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        layoutLbl.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        slotsHdr.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
        slotsHdr.setFont (juce::FontOptions (12.0f, juce::Font::bold));

        editor.addAndMakeVisible (nameLbl);
        editor.addAndMakeVisible (nameEd);
        editor.addAndMakeVisible (layoutLbl);
        editor.addAndMakeVisible (layoutCombo);
        editor.addAndMakeVisible (slotsHdr);
        editor.addAndMakeVisible (slotsViewport);
        slotsViewport.setViewedComponent (&slotsHolder, false);
        slotsViewport.setScrollBarsShown (true, false);

        nameEd.onTextChange = [this] {
            if (auto* g = dGetGroup (selectedGroup))
            {
                g->name = nameEd.getText();
                groupList.repaint();
            }
        };

        int id = 1;
        for (const auto& opt : getLayoutOptions())
            layoutCombo.addItem (opt.name, id++);
        layoutCombo.onChange = [this] {
            auto* g = dGetGroup (selectedGroup);
            if (g == nullptr)
                return;
            auto opts = getLayoutOptions();
            const int sel = layoutCombo.getSelectedId() - 1;
            if (sel < 0 || sel >= (int) opts.size())
                return;
            // Re-laying-out reconfigures each slot's plugin in place
            // (releaseResources + setBusesLayout + prepareToPlay).  Close any panel
            // editor open on those plugins first -- same lifetime guard as delete
            // (see onStructuralChange).
            if (onStructuralChange)
                onStructuralChange();
            g->channelSet = opts[(size_t) sel].channelSet;
            g->memberChannels.assign ((size_t) g->channelSet.size(), -1);
            for (auto& slot : g->pluginSlots)
                if (slot)
                    slot->prepare (engine.getEngineSampleRate(),
                        engine.getEngineBlockSize(),
                        g->channelSet.size());
            rebuildEditor();
        };

        addAndMakeVisible (editor);

        rebuildGroupList();
    }

    void GroupManagerDialog::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour::fromRGB (32, 32, 36));
    }

    void GroupManagerDialog::setDirection (Direction d)
    {
        if (direction == d)
            return;
        direction = d;
        // Auto-select first group on the new side (if any) so the editor on the
        // right shows its members immediately instead of going blank.
        selectedGroup = (dGetNumGroups() > 0) ? 0 : -1;
        inSideBtn.setToggleState (d == Direction::Inputs, juce::dontSendNotification);
        outSideBtn.setToggleState (d == Direction::Outputs, juce::dontSendNotification);
        rebuildGroupList();
        rebuildEditor();
    }

    void GroupManagerDialog::resized()
    {
        auto r = getLocalBounds().reduced (12);
        auto headerRow = r.removeFromTop (28);

        // Left: GROUPS title + Create (+) / Delete (-) right next to it.
        title.setBounds (headerRow.removeFromLeft (90));
        headerRow.removeFromLeft (4);
        createBtn.setBounds (headerRow.removeFromLeft (28));
        headerRow.removeFromLeft (2);
        deleteBtn.setBounds (headerRow.removeFromLeft (28));

        // Right: side toggle.
        outSideBtn.setBounds (headerRow.removeFromRight (90));
        headerRow.removeFromRight (4);
        inSideBtn.setBounds (headerRow.removeFromRight (90));
        r.removeFromTop (6);

        auto bottom = r.removeFromBottom (32);
        doneBtn.setBounds (bottom.removeFromRight (80));
        r.removeFromBottom (8);

        auto left = r.removeFromLeft (260);
        r.removeFromLeft (12);
        groupList.setBounds (left);
        editor.setBounds (r);

        // Editor internal layout
        auto er = editor.getLocalBounds();
        auto row = er.removeFromTop (28);
        nameLbl.setBounds (row.removeFromLeft (60));
        nameEd.setBounds (row);
        er.removeFromTop (4);
        row = er.removeFromTop (28);
        layoutLbl.setBounds (row.removeFromLeft (60));
        layoutCombo.setBounds (row);
        er.removeFromTop (10);
        slotsHdr.setBounds (er.removeFromTop (20));
        slotsViewport.setBounds (er);

        if (auto* g = dGetGroup (selectedGroup))
        {
            const int n = (int) g->memberChannels.size();
            const int rowH = 28;
            slotsHolder.setSize (slotsViewport.getWidth() - 16, juce::jmax (rowH, n * rowH + 4));
            for (int i = 0; i < n; ++i)
            {
                slotLabels[i]->setBounds (0, i * rowH + 4, 60, rowH - 8);
                slotCombos[i]->setBounds (66, i * rowH + 4, slotsHolder.getWidth() - 70, rowH - 8);
            }
        }
    }

    void GroupManagerDialog::rebuildGroupList()
    {
        groupList.updateContent();
        if (selectedGroup >= dGetNumGroups())
            selectedGroup = dGetNumGroups() - 1;
        if (selectedGroup >= 0)
            groupList.selectRow (selectedGroup);
        rebuildEditor();
    }

    void GroupManagerDialog::rebuildEditor()
    {
        auto* g = dGetGroup (selectedGroup);
        const bool enabled = g != nullptr;
        nameEd.setEnabled (enabled);
        layoutCombo.setEnabled (enabled);

        slotLabels.clear();
        slotCombos.clear();

        if (!enabled)
        {
            nameEd.setText ({}, juce::dontSendNotification);
            layoutCombo.setSelectedId (0, juce::dontSendNotification);
            slotsHolder.setSize (slotsViewport.getWidth() - 16, 0);
            return;
        }

        nameEd.setText (g->name, juce::dontSendNotification);
        auto opts = getLayoutOptions();
        for (int i = 0; i < (int) opts.size(); ++i)
            if (opts[(size_t) i].channelSet == g->channelSet)
            {
                layoutCombo.setSelectedId (i + 1, juce::dontSendNotification);
                break;
            }

        auto allChannels = buildChannelLabels (engine, direction);

        const auto chTypes = g->channelSet.getChannelTypes();
        const int n = juce::jmin ((int) chTypes.size(), (int) g->memberChannels.size());
        for (int i = 0; i < n; ++i)
        {
            auto* lbl = new juce::Label ({}, juce::AudioChannelSet::getChannelTypeName (chTypes[i]));
            lbl->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            lbl->setFont (juce::FontOptions (11.0f));
            slotsHolder.addAndMakeVisible (*lbl);
            slotLabels.add (lbl);

            auto* cb = new juce::ComboBox();
            cb->addItem ("(none)", 1);
            for (int c = 0; c < (int) allChannels.size(); ++c)
                cb->addItem (allChannels[(size_t) c].text, c + 2);

            const int memberCh = g->memberChannels[(size_t) i];
            int sel = 1;
            for (int c = 0; c < (int) allChannels.size(); ++c)
                if (allChannels[(size_t) c].globalIdx == memberCh)
                {
                    sel = c + 2;
                    break;
                }
            cb->setSelectedId (sel, juce::dontSendNotification);

            cb->onChange = [this, i, cb, allChannels] {
                const int id = cb->getSelectedId();
                const int globalCh = (id <= 1) ? -1 : allChannels[(size_t) (id - 2)].globalIdx;
                dAssignChannel (selectedGroup, i, globalCh);

                // Auto-fill subsequent empty slots: pick the channels that come
                // after the one just chosen, in order.
                if (globalCh >= 0)
                {
                    int chPos = -1;
                    for (int c = 0; c < (int) allChannels.size(); ++c)
                        if (allChannels[(size_t) c].globalIdx == globalCh)
                        {
                            chPos = c;
                            break;
                        }

                    if (auto* g = dGetGroup (selectedGroup); g != nullptr && chPos >= 0)
                    {
                        const int nSlots = (int) g->memberChannels.size();
                        for (int slot = i + 1; slot < nSlots; ++slot)
                        {
                            if (g->memberChannels[(size_t) slot] >= 0)
                                continue; // already set
                            const int nextPos = chPos + (slot - i);
                            if (nextPos >= (int) allChannels.size())
                                break;
                            dAssignChannel (selectedGroup, slot, allChannels[(size_t) nextPos].globalIdx);
                        }
                    }
                }

                juce::MessageManager::callAsync ([this] { rebuildGroupList(); });
            };

            slotsHolder.addAndMakeVisible (*cb);
            slotCombos.add (cb);
        }

        resized();
    }

    void GroupManagerDialog::onSelectionChanged (int newRow)
    {
        selectedGroup = newRow;
        rebuildEditor();
    }

    void GroupManagerDialog::onCreateClicked()
    {
        const juce::String prefix = direction == Direction::Inputs ? "IN Group " : "OUT Group ";
        const int gi = dCreateGroup (prefix + juce::String (dGetNumGroups() + 1),
            juce::AudioChannelSet::stereo());
        if (auto* g = dGetGroup (gi))
            for (auto& slot : g->pluginSlots)
                if (slot)
                    slot->prepare (engine.getEngineSampleRate(),
                        engine.getEngineBlockSize(),
                        g->channelSet.size());
        selectedGroup = gi;
        rebuildGroupList();
    }

    void GroupManagerDialog::onDeleteClicked()
    {
        if (selectedGroup < 0)
            return;
        // Removing the group destroys its MultiChannelPluginHosts (and the AU
        // instances inside).  Close any open panel editors FIRST so their windows
        // don't outlive the plugin they reference (see onStructuralChange).
        if (onStructuralChange)
            onStructuralChange();
        dRemoveGroup (selectedGroup);
        if (selectedGroup >= dGetNumGroups())
            selectedGroup = dGetNumGroups() - 1;
        rebuildGroupList();
    }

    int GroupManagerDialog::ListModel::getNumRows()
    {
        return dlg.dGetNumGroups();
    }

    void GroupManagerDialog::ListModel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool sel)
    {
        auto* gp = dlg.dGetGroup (row);
        if (gp == nullptr)
            return;
        if (sel)
            g.fillAll (juce::Colour::fromRGB (60, 90, 140));
        else if (row % 2)
            g.fillAll (juce::Colour::fromRGB (28, 28, 32));
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (gp->name + "   (" + juce::String (gp->channelSet.size()) + " ch)",
            8,
            0,
            w - 16,
            h,
            juce::Justification::centredLeft,
            true);
    }

    void GroupManagerDialog::launch (AudioEngine& engine,
        std::function<void()> onClose,
        std::function<void()> onStructuralChange,
        Direction dir)
    {
        auto* content = new GroupManagerDialog (engine, dir);
        content->onClose = std::move (onClose);
        content->onStructuralChange = std::move (onStructuralChange);

        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = "Groups";
        o.content.setOwned (content);
        o.dialogBackgroundColour = juce::Colour::fromRGB (32, 32, 36);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = true;
        o.launchAsync();
    }

} // namespace dcr

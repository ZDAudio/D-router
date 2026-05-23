#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "Engine/AudioEngine.h"

namespace dcr {

// CRUD UI for output groups.  Each group has a layout (stereo / 5.1 / 7.1.4
// etc) and a per-slot member channel.  Assigning a channel that's already in
// another group auto-removes it from there.
class GroupManagerDialog : public juce::Component
{
public:
    explicit GroupManagerDialog (AudioEngine& engine);

    // Called when the dialog closes; signals the host to refresh its UI.
    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;

    static void launch (AudioEngine& engine, std::function<void()> onClose);

private:
    void rebuildGroupList();
    void rebuildEditor();
    void onSelectionChanged (int newRow);
    void onCreateClicked();
    void onDeleteClicked();

    AudioEngine& engine;
    int selectedGroup = -1;

    juce::Label      title       { {}, "Output groups" };
    juce::ListBox    groupList;
    juce::TextButton createBtn   { "Create" };
    juce::TextButton deleteBtn   { "Delete" };
    juce::TextButton doneBtn     { "Done" };

    // Editor panel (right side)
    juce::Component  editor;
    juce::Label      nameLbl     { {}, "Name" };
    juce::TextEditor nameEd;
    juce::Label      layoutLbl   { {}, "Layout" };
    juce::ComboBox   layoutCombo;
    juce::Label      slotsHdr    { {}, "Member channels" };
    juce::Viewport   slotsViewport;
    juce::Component  slotsHolder;
    juce::OwnedArray<juce::Label>    slotLabels;
    juce::OwnedArray<juce::ComboBox> slotCombos;

    struct ListModel : public juce::ListBoxModel
    {
        explicit ListModel (GroupManagerDialog& d) : dlg (d) {}
        int getNumRows() override;
        void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool sel) override;
        void selectedRowsChanged (int row) override { dlg.onSelectionChanged (row); }
        GroupManagerDialog& dlg;
    };
    ListModel listModel { *this };
};

} // namespace dcr

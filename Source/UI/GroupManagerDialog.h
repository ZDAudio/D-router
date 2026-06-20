#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "Engine/AudioEngine.h"

namespace dcr {

struct OutputGroup;


// CRUD UI for output groups.  Each group has a layout (stereo / 5.1 / 7.1.4
// etc) and a per-slot member channel.  Assigning a channel that's already in
// another group auto-removes it from there.
class GroupManagerDialog : public juce::Component
{
public:
    enum class Direction { Outputs, Inputs };

    explicit GroupManagerDialog (AudioEngine& engine, Direction dir = Direction::Outputs);

    // Switch which side (input vs output) the dialog is editing.  Called by
    // the in-dialog toggle row.
    void setDirection (Direction d);

    // Called when the dialog closes; signals the host to refresh BOTH panels
    // (user may have edited either side during the session).
    std::function<void()> onClose;

    // Fired immediately BEFORE any action that frees or reconfigures a group's
    // plugin instances (delete group, change layout).  The dialog is NON-MODAL,
    // so the group panels stay interactive behind it: the user can re-open a
    // plugin editor after launch, then delete/relayout that group.  The host
    // wires this to close the panels' editor windows first -- each holds a raw
    // AudioPluginInstance&, so it must be torn down while the plugin is still
    // alive (otherwise ~PluginEditorWindow runs the AU's editorBeingDeleted()
    // on freed memory and segfaults).
    std::function<void()> onStructuralChange;

    void paint (juce::Graphics&) override;
    void resized() override;

    static void launch (AudioEngine& engine,
                        std::function<void()> onClose,
                        std::function<void()> onStructuralChange,
                        Direction dir = Direction::Outputs);

private:
    void rebuildGroupList();
    void rebuildEditor();
    void onSelectionChanged (int newRow);
    void onCreateClicked();
    void onDeleteClicked();

    AudioEngine& engine;
    Direction    direction;
    int selectedGroup = -1;

    // Direction-aware dispatch (input vs output manager + matching channel list).
    int          dGetNumGroups() const;
    OutputGroup* dGetGroup (int idx);
    int          dCreateGroup (juce::String name, juce::AudioChannelSet cs);
    void         dRemoveGroup (int idx);
    void         dAssignChannel (int gIdx, int slot, int globalCh);

    juce::Label      title       { {}, "GROUPS" };
    juce::TextButton inSideBtn   { "INPUTS" };
    juce::TextButton outSideBtn  { "OUTPUTS" };
    juce::ListBox    groupList;
    juce::TextButton createBtn   { "+" };
    juce::TextButton deleteBtn   { "-" };
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

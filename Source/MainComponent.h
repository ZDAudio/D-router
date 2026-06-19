#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <vector>

#include <atomic>
#include <thread>

#include "Diagnostics/PerfMonitor.h"
#include "Engine/AudioEngine.h"
#include "Persistence/SnapshotStore.h"
#include "UI/LoadingOverlay.h"
#include "UI/MatrixView.h"
#include "UI/OutputGroupPanel.h"
#include "UI/StatusPanel.h"
#include "UI/LookAndFeel.h"

namespace dcr {

class MainComponent : public juce::Component,
                      public juce::MenuBarModel,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;
    bool keyPressed (const juce::KeyPress& k) override;

    // ----- juce::MenuBarModel (native macOS top menu bar) --------------------
    // Installed via setMacMainMenu(this) in the ctor.  The menu only shows
    // while the app is a "regular" app (window visible / Dock icon on); when
    // the window is hidden to the tray the app becomes accessory and macOS
    // hides the menu bar automatically -- the model stays installed.
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu   getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
    void              menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

private:
    void timerCallback() override;

    // Hide the window to the menu bar (and drop the Dock icon).  Shared by
    // the red close button, File > Close Window, and the tray toggle.
    void hideToTray();

    // Modeless About dialog: app name, PRIVATE BETA mark, version, contact.
    void showAboutDialog();

    void openDeviceDialog();
    void applyDeviceSelection (std::vector<AudioEngine::DeviceSpec> newSpecs);
    void refreshStatus();
    void stopEngine();

    // PANIC: first press mutes every input + output (saving the prior state);
    // second press restores it.  If the user manually flips any mute button
    // while panic is active, the saved state is discarded so the next panic
    // press just re-mutes everything fresh.
    void panicActivate();
    void panicRelease();
    void panicResetRestart();   // RESET button: restore mutes + preserve-state restart
    void updatePanicButtonAppearance();
    bool inPanic = false;
    std::vector<unsigned char> savedInputMutes;
    std::vector<unsigned char> savedOutputMutes;
    Snapshot gatherCurrentSnapshot() const;
    // Per-channel FX harvest only (no group chains, no UI state).  Calls
    // getStateInformation() on every AU, so the caller MUST ensure the matrix
    // processor is stopped first (see applyDeviceSelection's worker thread).
    std::vector<Snapshot::ChannelChain> harvestChannelChains() const;
    void     applySnapshot (const Snapshot& s);
    void     saveSnapshotInteractive();
    void     loadSnapshotInteractive();

    // Matrix state keyed by (deviceName, channel) so it survives device add/remove.
    struct ChannelKey { juce::String dev; int ch; };
    struct MatrixStateByName
    {
        struct InEntry  { ChannelKey k; float trim = 1.0f; bool mute = false; bool solo = false; };
        struct OutEntry { ChannelKey k; float trim = 1.0f; bool mute = false; };
        struct CrossEntry { ChannelKey src; ChannelKey dst; float gain = 0.0f; };
        std::vector<InEntry>  inputs;
        std::vector<OutEntry> outputs;
        std::vector<CrossEntry> crosspoints;
    };
    MatrixStateByName captureMatrixByName() const;
    void              restoreMatrixByName (const MatrixStateByName& s);

    // Snapshot apply runs in two phases: the synchronous bit (settings,
    // groups) plus kicking applyDeviceSelection(), then the async tail that
    // touches the matrix AFTER engine.start() has resized it.  pendingSnap
    // bridges the two -- the reconfig's callAsync drains it.
    struct PendingSnapshotApply
    {
        std::vector<float>         inputTrim;
        std::vector<float>         outputTrim;
        std::vector<unsigned char> inputMute;
        std::vector<unsigned char> outputMute;
        std::vector<unsigned char> inputSolo;
        std::vector<Snapshot::Crosspoint> crosspoints;
        std::vector<Snapshot::ChannelChain> channelChains;
        std::vector<Snapshot::GroupChain>   groupChains;
        bool valid = false;
    };
    PendingSnapshotApply pendingSnap;
    void restorePluginChainsAsync();

    // Plugin-restore queue.  JUCE's createPluginInstanceAsync for AU is
    // "async" in name only -- on macOS the actual AudioComponent
    // instantiation has to run on the message thread.  If we fire every
    // restore in one shot the message thread gets pegged for tens of
    // seconds (one user's snapshot with 11 input chains of a single big
    // EQ wedged the UI at ~1 fps for 30 s).  Process strictly one at a
    // time with a callAsync yield between each, so the message thread
    // can service paint/mouse/meter events between loads.
    struct PendingPluginLoad
    {
        juce::PluginDescription desc;
        juce::MemoryBlock       state;
        bool                    bypassed = false;
        int                     slotIdx  = 0;

        enum class Kind { ChannelSlot, GroupSlot } kind = Kind::ChannelSlot;

        // For ChannelSlot.
        bool isInputChannel  = false;
        int  globalChannelIdx = -1;

        // For GroupSlot.
        bool                  isInputGroup = false;
        int                   groupIdx     = -1;
        juce::AudioChannelSet channelSet { juce::AudioChannelSet::stereo() };
    };
    std::vector<PendingPluginLoad> pluginLoadQueue;
    int      pluginLoadCursor   = 0;
    uint32_t pluginLoadStartMs  = 0;
    void processNextPluginLoad();

    // Heap-allocated "this is still alive" sentinel.  Captured by async
    // plugin-restore callbacks; flipped to false in ~MainComponent so a
    // late createPluginInstanceAsync callback can early-out instead of
    // dereferencing this/engine/matrixView after destruction.
    std::shared_ptr<std::atomic<bool>> aliveToken
        = std::make_shared<std::atomic<bool>> (true);

    AudioEngine engine;
    std::vector<AudioEngine::DeviceSpec> currentSpecs;
    std::unique_ptr<juce::FileChooser> activeChooser;

    // Periodic perf snapshot into the Logger -- 5 sec interval, free of
    // engine state during reconfigure (atomic reads only).
    PerfMonitor perfMonitor { engine };

    std::thread       reconfigThread;
    std::atomic<bool> isReconfiguring { false };

    LookAndFeel customLookAndFeel;

    // GPU-backed renderer for the entire top-level component tree.  Drops
    // composite & paint cost dramatically for the slider/meter-heavy UI.
    juce::OpenGLContext openGLContext;

    juce::Label      title { {}, "ZDAudio D-Router" };
    juce::TextButton devicesButton  { "Devices..." };
    juce::TextButton settingsButton { "Settings..." };
    juce::TextButton groupsButton    { "Groups..." };
    juce::TextButton saveButton     { "Save..." };
    juce::TextButton loadButton     { "Load..." };
    juce::TextButton logsButton     { "Logs..." };
    juce::TextButton stopButton     { "PANIC" };
    // Appears beside PANIC only while panic is engaged.  Restores the
    // pre-panic mute state, then does a preserve-state engine restart --
    // the in-app "turn it off and on again" that also clears an OS-driven
    // device-format desync without quitting the app.
    juce::TextButton resetButton    { "RESET" };

    // Top Navigation Tabs
    enum Tab { RoutingTab, GroupsTab, StatusTab };
    Tab currentTab = RoutingTab;

    juce::TextButton matrixTabBtn { "MATRIX ROUTING" };
    juce::TextButton groupsTabBtn { "IN / OUT GROUPS" };
    juce::TextButton statusTabBtn { "ENGINE MONITOR" };

    juce::Label groupsPlaceholder;
    juce::Label inputGroupsPlaceholder;
    juce::Label statusPlaceholder;

    MatrixView       matrixView { engine };
    OutputGroupPanel groupPanel       { engine, OutputGroupPanel::Direction::Outputs };
    OutputGroupPanel inputGroupPanel  { engine, OutputGroupPanel::Direction::Inputs  };
    StatusPanel      statusPanel { engine };

    // Full-window overlay shown during startup splash + matrix rebuilds.
    LoadingOverlay   loadingOverlay;

    bool groupPanelDetached       = false;
    bool inputGroupPanelDetached  = false;
    bool statusPanelDetached      = false;
    std::unique_ptr<juce::DocumentWindow> groupWindow;
    std::unique_ptr<juce::DocumentWindow> inputGroupWindow;
    std::unique_ptr<juce::DocumentWindow> statusWindow;
    
    void toggleGroupPanelDetach();
    void toggleInputGroupPanelDetach();
    void toggleStatusPanelDetach();
    void switchTab (Tab newTab);
    static int cards_default_width();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace dcr

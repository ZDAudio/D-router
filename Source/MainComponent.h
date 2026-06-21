#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <vector>

#include <atomic>
#include <thread>

#include "Diagnostics/PerfMonitor.h"
#include "Engine/AudioEngine.h"
#include "Engine/ReconfigurationController.h"
#include "Persistence/SnapshotStore.h"
#include "Routing/PanicController.h"
#include "UI/DeviceVolumePanel.h"
#include "UI/LoadingOverlay.h"
#include "UI/LookAndFeel.h"
#include "UI/MatrixView.h"
#include "UI/OutputGroupPanel.h"
#include "UI/PanelHost.h"
#include "UI/StatusPanel.h"
#include "Update/UpdateChecker.h"

namespace dcr
{

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
        juce::PopupMenu getMenuForIndex (int topLevelMenuIndex, const juce::String& menuName) override;
        void menuItemSelected (int menuItemID, int topLevelMenuIndex) override;

    private:
        void timerCallback() override;

        // Hide the window to the menu bar (and drop the Dock icon).  Shared by
        // the red close button, File > Close Window, and the tray toggle.
        void hideToTray();

        // Modeless About dialog: app name, PRIVATE BETA mark, version, contact.
        void showAboutDialog();

        // Opt-in GitHub auto-update.  Runs once ~3 s after launch (silent: only a
        // newer version pops a prompt) and from the "Check for Updates..." menu item
        // (userInitiated: also reports "up to date" / "couldn't check").
        void checkForUpdates (bool userInitiated);
        void showUpdatePrompt (std::unique_ptr<dcr::update::ReleaseInfo> info);

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
        void panicResetRestart(); // RESET button: restore mutes + preserve-state restart
        void updatePanicButtonAppearance();
        PanicController panic;
        Snapshot gatherCurrentSnapshot() const;
        // Per-channel FX harvest only (no group chains, no UI state).  Calls
        // getStateInformation() on every AU, so the caller MUST ensure the matrix
        // processor is stopped first (see applyDeviceSelection's worker thread).
        std::vector<Snapshot::ChannelChain> harvestChannelChains() const;
        void applySnapshot (const Snapshot& s);
        void saveSnapshotInteractive();
        void loadSnapshotInteractive();

        // Matrix state keyed by (deviceName, channel) so it survives device add/remove.
        struct ChannelKey
        {
            juce::String dev;
            int ch;
        };
        struct MatrixStateByName
        {
            struct InEntry
            {
                ChannelKey k;
                float trim = 1.0f;
                bool mute = false;
                bool solo = false;
            };
            struct OutEntry
            {
                ChannelKey k;
                float trim = 1.0f;
                bool mute = false;
            };
            struct CrossEntry
            {
                ChannelKey src;
                ChannelKey dst;
                float gain = 0.0f;
            };
            std::vector<InEntry> inputs;
            std::vector<OutEntry> outputs;
            std::vector<CrossEntry> crosspoints;
        };
        MatrixStateByName captureMatrixByName() const;
        void restoreMatrixByName (const MatrixStateByName& s);

        // The reconfigure payload (pending snapshot + plugin-restore queue) now
        // lives in ReconfigurationController as its single owner; access it via
        // `reconfig.snapshot()` / `reconfig.pluginQueue()` / etc.  These aliases
        // keep the struct names short at the call sites.
        using PendingSnapshotApply = ReconfigurationController::PendingSnapshotApply;
        using PendingPluginLoad = ReconfigurationController::PendingPluginLoad;
        void restorePluginChainsAsync();
        void processNextPluginLoad();

        // Heap-allocated "this is still alive" sentinel.  Captured by async
        // plugin-restore callbacks; flipped to false in ~MainComponent so a
        // late createPluginInstanceAsync callback can early-out instead of
        // dereferencing this/engine/matrixView after destruction.
        std::shared_ptr<std::atomic<bool>> aliveToken = std::make_shared<std::atomic<bool>> (true);

        AudioEngine engine;
        std::vector<AudioEngine::DeviceSpec> currentSpecs;
        std::unique_ptr<juce::FileChooser> activeChooser;

        // GitHub auto-updater (opt-in); background-threaded, message-thread callback.
        dcr::update::UpdateChecker updateChecker;

        // Periodic perf snapshot into the Logger -- 5 sec interval, free of
        // engine state during reconfigure (atomic reads only).
        PerfMonitor perfMonitor { engine };

        std::thread reconfigThread;
        // Explicit reconfigure lifecycle (Phase C3) -- single owner of "are we
        // reconfiguring", replacing a bare atomic bool with ordered phases.
        ReconfigurationController reconfig;

        LookAndFeel customLookAndFeel;

        // GPU-backed renderer for the entire top-level component tree.  Drops
        // composite & paint cost dramatically for the slider/meter-heavy UI.
        juce::OpenGLContext openGLContext;

        juce::Label title { {}, "ZDAudio D-Router" };
        juce::TextButton devicesButton { "Devices..." };
        juce::TextButton settingsButton { "Settings..." };
        juce::TextButton groupsButton { "Groups..." };
        juce::TextButton saveButton { "Save..." };
        juce::TextButton loadButton { "Load..." };
        juce::TextButton logsButton { "Logs..." };
        juce::TextButton stopButton { "PANIC" };
        // Appears beside PANIC only while panic is engaged.  Restores the
        // pre-panic mute state, then does a preserve-state engine restart --
        // the in-app "turn it off and on again" that also clears an OS-driven
        // device-format desync without quitting the app.
        juce::TextButton resetButton { "RESET" };

        // Top Navigation Tabs
        enum Tab { RoutingTab,
            GroupsTab,
            AudioSetupTab,
            StatusTab };
        Tab currentTab = RoutingTab;

        juce::TextButton matrixTabBtn { "MATRIX ROUTING" };
        juce::TextButton groupsTabBtn { "IN / OUT GROUPS" };
        juce::TextButton audioSetupTabBtn { "AUDIO SETUP" };
        juce::TextButton statusTabBtn { "ENGINE MONITOR" };

        juce::Label groupsPlaceholder;
        juce::Label inputGroupsPlaceholder;
        juce::Label statusPlaceholder;

        MatrixView matrixView { engine };
        OutputGroupPanel groupPanel { engine, OutputGroupPanel::Direction::Outputs };
        OutputGroupPanel inputGroupPanel { engine, OutputGroupPanel::Direction::Inputs };
        StatusPanel statusPanel { engine };

        // AUDIO SETUP tab: per-device hardware volume/mute, inputs top / outputs
        // bottom (mirrors the IN / OUT GROUPS split).  Non-detachable -> no PanelHost.
        DeviceVolumePanel inputDeviceVolPanel { engine, DeviceVolumePanel::Direction::Inputs };
        DeviceVolumePanel outputDeviceVolPanel { engine, DeviceVolumePanel::Direction::Outputs };

        // Full-window overlay shown during startup splash + matrix rebuilds.
        LoadingOverlay loadingOverlay;

        // Each detachable panel is one PanelHost slot (Phase C2).  Declared
        // after the panels they reference so they tear down first.
        PanelHost groupHost { *this, groupPanel, "Output groups" };
        PanelHost inputGroupHost { *this, inputGroupPanel, "Input groups" };
        PanelHost statusHost { *this, statusPanel, "Engine status" };

        void switchTab (Tab newTab);
        static int cards_default_width();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
    };

} // namespace dcr

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <vector>

#include <atomic>
#include <thread>

#include "Diagnostics/PerfMonitor.h"
#include "Engine/AudioEngine.h"
#include "Engine/FormatRestartGuard.h"
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
#include "UI/ZDFace.h"
#include "Update/UpdateChecker.h"

namespace dcr
{

    class AppAudioProcesses; // fwd (Source/Engine/AppAudioProcesses.h)

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

        // App-audio capture (Part 3b): reconcile attach/detach against the running
        // processes (message-thread, gated by reconfig.active()), plus the minimal
        // "Add App Audio Input" picker.
        void reconcileAppAudioAttachments();
        void openAppInputMenu();
        void addAppInput (const juce::String& bundleId, const juce::String& displayName);
        void removeAppInput (const juce::String& bundleId);
        void clearAppInputs();

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
        // Per-group FX harvest.  Like harvestChannelChains, calls getStateInformation
        // on every group AU, so the matrix processor MUST be stopped first.  When
        // derivedOnly is true, only DERIVED groups (DeviceAuto / SoftIn, rebuilt each
        // engine start) are harvested -- used by the settings-change preserve path,
        // where Regular groups survive in place and re-harvesting them would
        // double-load.  When false (snapshot save), every group is harvested; derived
        // groups carry a groupName so restore can match them back after rebuild.
        std::vector<Snapshot::GroupChain> harvestGroupChains (bool derivedOnly) const;
        // Resolve the live group a restored chain targets.  Derived chains carry a
        // non-empty name and are matched by (direction, name) against the rebuilt
        // groups; Regular chains (empty name) use the deterministic index.
        OutputGroup* resolveGroupForRestore (bool isInput, const juce::String& name, int idx);
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

        // App-audio capture: the desired source list, the process watcher, and the
        // per-source currently-attached process id (parallel to currentAppInputs;
        // 0 == detached).  currentAppInputs is threaded through applyDeviceSelection
        // exactly like currentSpecs.
        std::vector<AudioEngine::AppInputSpec> currentAppInputs;
        std::unique_ptr<AppAudioProcesses> appAudioProcesses;
        std::vector<int> appAttachedPids;

        std::unique_ptr<juce::FileChooser> activeChooser;

        // The "ZD" easter-egg smiley, shown next to the brand title once the
        // Stereo Meter is unlocked (5 clicks on the title).
        ZDFace zdFace;

        // GitHub auto-updater (opt-in); background-threaded, message-thread callback.
        dcr::update::UpdateChecker updateChecker;

        // Periodic perf snapshot into the Logger -- 5 sec interval, free of
        // engine state during reconfigure (atomic reads only).
        PerfMonitor perfMonitor { engine };

        std::thread reconfigThread;
        // Explicit reconfigure lifecycle (Phase C3) -- single owner of "are we
        // reconfiguring", replacing a bare atomic bool with ordered phases.
        ReconfigurationController reconfig;

        // Backstop for the device-format-change watchdog (refreshStatus).  With
        // rate-following open() a real OS rate change converges in one restart;
        // this rate-limits restarts so a pathologically flapping device/driver
        // can't spin the engine.  See FormatRestartGuard.h.
        dcr::FormatRestartGuard formatRestartGuard;
        bool formatBackoffLogged = false;

        LookAndFeel customLookAndFeel;

        // GPU-backed renderer for the entire top-level component tree.  Drops
        // composite & paint cost dramatically for the slider/meter-heavy UI.
        juce::OpenGLContext openGLContext;

        // Title label that reveals the hidden 3D stereo-scatter meter on 5 quick
        // clicks (each within 0.6 s of the last). Deliberately undiscoverable.
        struct SecretTitle : public juce::Label, private juce::Timer
        {
            using juce::Label::Label;
            std::function<void()> onReveal;
            void mouseDown (const juce::MouseEvent&) override
            {
                if (++clickCount >= 5)
                {
                    clickCount = 0;
                    stopTimer();
                    if (onReveal)
                        onReveal();
                }
                else
                    startTimer (600);
            }
            void timerCallback() override
            {
                clickCount = 0;
                stopTimer();
            }
            int clickCount = 0;
        };
        SecretTitle title { {}, "ZDAudio D-Router" };
        juce::TextButton devicesButton { "Devices..." };
        juce::TextButton softwareButton { "Software..." };
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

        // Subtle captions over the two grouped toolbar zones (left = where sound
        // comes in + how it's configured, right = saved-session files).  PANIC
        // stands alone on the far right with no caption.
        juce::Label sourcesSectionLabel { {}, "SOURCES & SETUP" };
        juce::Label sessionSectionLabel { {}, "SESSION" };

        // Loud full-width alert shown only while panic is engaged, so a held-audio
        // state is impossible to miss.  Hidden otherwise.
        juce::Label panicBanner { {}, "ALL AUDIO MUTED   \xe2\x80\x94   click PANIC to restore, or RESET to restart the engine" };

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
        juce::Label matrixPlaceholder;
        juce::Label audioPlaceholder;

        // Pop-out triggers for the single-panel detachable views (Matrix + Audio
        // Setup), placed in a header strip above the content so they never
        // overlap the panel.  Groups / Engine Monitor carry their own in-panel
        // button instead.
        juce::TextButton matrixPopOutBtn { "->" };
        juce::TextButton audioPopOutBtn { "->" };

        MatrixView matrixView { engine };
        OutputGroupPanel groupPanel { engine, OutputGroupPanel::Direction::Outputs };
        OutputGroupPanel inputGroupPanel { engine, OutputGroupPanel::Direction::Inputs };
        StatusPanel statusPanel { engine };

        // AUDIO SETUP tab: per-device hardware volume/mute, inputs top / outputs
        // bottom (mirrors the IN / OUT GROUPS split).  The two panels live inside
        // audioSetupView so the whole tab detaches as one floating window.
        DeviceVolumePanel inputDeviceVolPanel { engine, DeviceVolumePanel::Direction::Inputs };
        DeviceVolumePanel outputDeviceVolPanel { engine, DeviceVolumePanel::Direction::Outputs };

        // Tiny container that stacks two child components (top half / bottom half)
        // -- used so Audio Setup's two device panels move together when detached.
        struct SplitView : juce::Component
        {
            juce::Component* top = nullptr;
            juce::Component* bottom = nullptr;
            void resized() override
            {
                auto r = getLocalBounds();
                auto t = r.removeFromTop (r.getHeight() / 2);
                r.removeFromTop (6);
                if (top != nullptr)
                    top->setBounds (t);
                if (bottom != nullptr)
                    bottom->setBounds (r);
            }
        };
        SplitView audioSetupView;

        // Left-rail width (px).  Eases between wide (168) and the icon-only strip
        // (56) so the compact transition is animated rather than snapping.
        // railWidthPx is the live (animated) value shared by resized() + paint();
        // railTargetW is where it's heading (chosen by the width threshold).
        // The icon/label swap is keyed off the live width, so the rail visibly
        // morphs.  A small Timer drives the ease and re-lays-out each frame.
        int railWidthPx = 168;
        int railTargetW = 168;
        struct CallbackTimer : juce::Timer
        {
            std::function<void()> fn;
            void timerCallback() override
            {
                if (fn)
                    fn();
            }
        };
        CallbackTimer railAnim;
        void stepRailAnimation();

        // Full-window overlay shown during startup splash + matrix rebuilds.
        LoadingOverlay loadingOverlay;

        // Each detachable panel is one PanelHost slot (Phase C2).  Declared
        // after the panels they reference so they tear down first.
        PanelHost groupHost { *this, groupPanel, "Output groups" };
        PanelHost inputGroupHost { *this, inputGroupPanel, "Input groups" };
        PanelHost statusHost { *this, statusPanel, "Engine status" };
        PanelHost matrixHost { *this, matrixView, "Matrix routing" };
        PanelHost audioHost { *this, audioSetupView, "Audio setup" };

        void switchTab (Tab newTab);
        static int cards_default_width();

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
    };

} // namespace dcr

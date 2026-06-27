#include "MainComponent.h"

#include "DSP/MultiChannelPluginHost.h"
#include "DSP/PluginHost.h"
#include "DSP/SafePluginOps.h"
#include "Diagnostics/Logger.h"
#include "Engine/AppAudioProcesses.h"
#include "Engine/AppAudioWorker.h"
#include "Engine/AppInputResolver.h"
#include "Persistence/CrashGuard.h"
#include "Persistence/SettingsStore.h"
#include "Routing/InputGroupManager.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"
#include "UI/DeviceManagerDialog.h"
#include "UI/GroupManagerDialog.h"
#include "UI/LogViewerDialog.h"
#include "UI/SettingsDialog.h"
#include "Update/UpdatePrompt.h"

namespace dcr
{

    // Apple (app-name) menu command ids.  Defined at file scope so both the
    // constructor (which builds the Apple menu) and menuItemSelected (far below,
    // where the rest of the menu ids live) can see them.
    static constexpr int kMenuAboutId = 1500;
    static constexpr int kMenuCheckUpdatesId = 1501;

    MainComponent::MainComponent()
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&customLookAndFeel);
        setLookAndFeel (&customLookAndFeel);

        // Continuous repainting forces a full-frame redraw at vsync (~60 Hz)
        // regardless of UI state.  That sounded harmless on Apple Silicon but
        // in practice the constant paint loop preempts the matrix audio thread
        // hard enough to cause underruns ("popping") plus a generally
        // sluggish system / choppy animations.  Leave it OFF; components
        // already call repaint() on their own when their state actually
        // changes (LevelMeter only repaints when its segment count changes,
        // CrosspointGrid only on cell hit / hover, etc.).  Tradeoff: live
        // resize can show brief frame staleness.
        openGLContext.setContinuousRepainting (false);

        setSize (1100, 700);

        // Required so MainComponent::keyPressed() receives Cmd+S / Cmd+O /
        // Cmd+= / Cmd+- shortcuts.  setWantsKeyboardFocus alone isn't enough --
        // the component still has to be the focused one, which it will be by
        // default once the window opens since no child has called grabKeyboard.
        setWantsKeyboardFocus (true);

        title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
        // App brand label is always white -- not theme-tinted -- so the brand
        // reads cleanly against any accent colour the user picks.
        title.setColour (juce::Label::textColourId, juce::Colours::white);
        title.setJustificationType (juce::Justification::centredLeft);
        title.setInterceptsMouseClicks (true, false);
        title.onReveal = [this] {
            engine.setStereoMeterUnlocked (true);
            zdFace.setVisible (true);
            resized();
        };
        addAndMakeVisible (title);
        addChildComponent (zdFace); // hidden until unlocked

        // App-audio capture: watch process launch/quit so captured apps
        // auto-(re)attach via the message-thread reconcile.
        appAudioProcesses = std::make_unique<AppAudioProcesses>();
        appAudioProcesses->onProcessesChanged = [this] { reconcileAppAudioAttachments(); };

        devicesButton.onClick = [this] { openDeviceDialog(); };
        softwareButton.onClick = [this] { openAppInputMenu(); };
        settingsButton.onClick = [this] {
            SettingsDialog::launch (engine.getSettings(),
                [this] (std::optional<EngineSettings> s, bool persistToDisk) {
                    if (!s.has_value())
                        return; // Cancel
                    const bool needsRestart = !s->audioPathEquals (engine.getSettings());

                    if (needsRestart && !currentSpecs.empty())
                    {
                        // Engine restart needed: applyDeviceSelection() will tear
                        // down workers + matrix-processor and rebuild on a worker
                        // thread, then resume meter / status timers from its own
                        // callAsync().  We MUST NOT touch any engine-reading UI
                        // timer here, or it will fire mid-rebuild and read a
                        // moved-from inputPeaks / outputPeaks vector -> crash.
                        engine.setSettings (*s);
                        if (persistToDisk)
                            SettingsStore::save (*s);
                        applyDeviceSelection (currentSpecs);
                    }
                    else
                    {
                        // UI-only or no-active-device change.  Safe to refresh
                        // timers immediately since the audio thread isn't being
                        // torn down.
                        engine.setSettings (*s);
                        if (persistToDisk)
                            SettingsStore::save (*s);
                        startTimer (engine.getSettings().statusTimerMs);
                        matrixView.pauseUpdates();
                        matrixView.resumeUpdates();
                    }

                    // Theme + repaint are pure UI; safe in either path.
                    customLookAndFeel.applyTheme (engine.getSettings());
                    // App brand label is always white -- not theme-tinted -- so the brand
                    // reads cleanly against any accent colour the user picks.
                    title.setColour (juce::Label::textColourId, juce::Colours::white);
                    repaint();
                });
        };
        saveButton.onClick = [this] { saveSnapshotInteractive(); };
        loadButton.onClick = [this] { loadSnapshotInteractive(); };
        stopButton.onClick = [this] { if (panic.isActive()) panicRelease(); else panicActivate(); };
        stopButton.setTooltip ("Mute every input and output.  Click again to restore the prior state.");

        // RESET: restore pre-panic mutes, then preserve-state engine restart.
        // Only visible while panic is engaged.
        resetButton.setColour (juce::TextButton::buttonColourId, juce::Colour::fromRGB (40, 90, 110));
        resetButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour::fromRGB (40, 90, 110));
        resetButton.setTooltip ("Restart the audio engine while keeping your routing, FX and groups.  "
                                "Use this if audio is crackling (e.g. after another app changed the "
                                "device sample rate) -- it re-syncs without quitting.");
        resetButton.onClick = [this] { panicResetRestart(); };
        resetButton.setVisible (false);
        updatePanicButtonAppearance();
        groupsButton.onClick = [this] {
            // The dialog lets the user remove groups or change a group's layout,
            // which destroys / reconfigures their MultiChannelPluginHosts (and the
            // AudioPluginInstances inside).  Close any editors open RIGHT NOW up
            // front...
            groupPanel.closeAllPluginEditors();
            inputGroupPanel.closeAllPluginEditors();

            // ...but the dialog is NON-MODAL, so the panels stay interactive behind
            // it: the user can re-open an editor after launch, then delete/relayout
            // that group.  The launch-time close above can't cover that.  Wire
            // onStructuralChange so the dialog re-closes panel editors from inside,
            // immediately before each such destructive action -- otherwise
            // ~PluginEditorWindow runs editorBeingDeleted() on a freed
            // AudioProcessor and segfaults (the PR #10 mechanism, re-opened by the
            // non-modal panels).  After close we rebuild BOTH panels since either
            // side may have changed.
            GroupManagerDialog::launch (engine, [this] {
                groupPanel     .rebuild();
                inputGroupPanel.rebuild(); }, [this] {
                groupPanel     .closeAllPluginEditors();
                inputGroupPanel.closeAllPluginEditors(); }, GroupManagerDialog::Direction::Outputs);
        };
        addAndMakeVisible (devicesButton);
        addAndMakeVisible (softwareButton);
        addAndMakeVisible (settingsButton);
        addAndMakeVisible (groupsButton);

        addAndMakeVisible (saveButton);
        addAndMakeVisible (loadButton);
        addAndMakeVisible (logsButton);
        addAndMakeVisible (stopButton);
        addChildComponent (resetButton); // visibility toggled with panic state

        logsButton.onClick = [] { LogViewerDialog::launch(); };

        // Bulk expand/collapse-all now lives as four buttons in the matrix
        // corner legend (next to INPUTS / OUTPUTS).  The View menu also still
        // exposes Expand-all / Collapse-all for both directions at once.

        // Auto-save whenever the user folds / unfolds a device so the next
        // launch reflects the layout they left -- same pattern other panels use.
        matrixView.onCollapseStateChanged = [this] {
            SnapshotStore::save (SnapshotStore::getLastUsedFile(), gatherCurrentSnapshot());
        };
        logsButton.setTooltip ("Open in-app diagnostics log.  Contents are also flushed to "
                               "~/Library/Logs/D-Router/ on every line so a crash leaves the "
                               "lead-up context recoverable.");

        // Group buttons for radio toggling
        matrixTabBtn.setRadioGroupId (100);
        groupsTabBtn.setRadioGroupId (100);
        audioSetupTabBtn.setRadioGroupId (100);
        statusTabBtn.setRadioGroupId (100);

        matrixTabBtn.setClickingTogglesState (true);
        groupsTabBtn.setClickingTogglesState (true);
        audioSetupTabBtn.setClickingTogglesState (true);
        statusTabBtn.setClickingTogglesState (true);

        matrixTabBtn.setToggleState (true, juce::dontSendNotification);

        matrixTabBtn.onClick = [this] { switchTab (RoutingTab); };
        groupsTabBtn.onClick = [this] { switchTab (GroupsTab); };
        audioSetupTabBtn.onClick = [this] { switchTab (AudioSetupTab); };
        statusTabBtn.onClick = [this] { switchTab (StatusTab); };

        addAndMakeVisible (matrixTabBtn);
        addAndMakeVisible (groupsTabBtn);
        addAndMakeVisible (audioSetupTabBtn);
        addAndMakeVisible (statusTabBtn);

        groupsPlaceholder.setText ("OUTPUT GROUPS DETACHED\n\nPanel is floating in an external window.", juce::dontSendNotification);
        groupsPlaceholder.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold));
        groupsPlaceholder.setJustificationType (juce::Justification::centred);
        groupsPlaceholder.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        addChildComponent (groupsPlaceholder);

        statusPlaceholder.setText ("ENGINE MONITOR DETACHED\n\nPanel is floating in an external window.", juce::dontSendNotification);
        statusPlaceholder.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold));
        statusPlaceholder.setJustificationType (juce::Justification::centred);
        statusPlaceholder.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        addChildComponent (statusPlaceholder);

        // Output Group Panel setup
        addChildComponent (groupPanel);
        groupHost.windowSize = [] { return juce::Point<int> { juce::jmax (820, cards_default_width()), 240 }; };
        groupHost.setPanelDetached = [this] (bool d) { groupPanel.setDetached (d); };
        groupHost.onChanged = [this] { switchTab (currentTab); };
        groupPanel.onPopOutRequested = [this] { groupHost.toggle(); };
        groupPanel.onGroupHover = [this] (const std::vector<int>& outs) {
            matrixView.setHighlightedOutputs (outs);
        };

        // Input Group Panel setup
        addChildComponent (inputGroupPanel);
        inputGroupHost.windowSize = [] { return juce::Point<int> { juce::jmax (820, cards_default_width()), 240 }; };
        inputGroupHost.setPanelDetached = [this] (bool d) { inputGroupPanel.setDetached (d); };
        inputGroupHost.onChanged = [this] { switchTab (currentTab); };
        inputGroupPanel.onPopOutRequested = [this] { inputGroupHost.toggle(); };
        inputGroupPanel.onGroupHover = [this] (const std::vector<int>& ins) {
            matrixView.setHighlightedInputs (ins);
        };

        inputGroupsPlaceholder.setText ("INPUT GROUPS DETACHED\n\nPanel is floating in an external window.",
            juce::dontSendNotification);
        inputGroupsPlaceholder.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
            13.0f,
            juce::Font::bold));
        inputGroupsPlaceholder.setJustificationType (juce::Justification::centred);
        inputGroupsPlaceholder.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
        addChildComponent (inputGroupsPlaceholder);

        // AUDIO SETUP tab panels (hidden until that tab is selected).
        addChildComponent (inputDeviceVolPanel);
        addChildComponent (outputDeviceVolPanel);

        addChildComponent (matrixView);

        // Loading overlay: shown immediately so the user sees the brand splash
        // while devices spin up + initial matrix builds.  Hidden when matrixView
        // finishes its first rebuild from the snapshot-restored engine state.
        addAndMakeVisible (loadingOverlay);
        loadingOverlay.showOverlay ("Starting engine and building routing...");

        matrixView.onRebuildProgress = [this] (int done, int total) {
            loadingOverlay.setProgress (done, total);
            if (total > 0)
                loadingOverlay.setMessage ("Building routing  " + juce::String (done)
                                           + " / " + juce::String (total) + " channels");
        };
        matrixView.onRebuildFinished = [this] {
            // Hide the overlay only once BOTH the chunked widget rebuild AND
            // the serialized plugin-restore queue have drained.  If we hide
            // while plugins are still loading, the user perceives the UI as
            // "frozen" because the message thread is pegged installing AUs
            // even though the splash is gone.  processNextPluginLoad bumps
            // pluginLoadCursor as it finishes each load; when cursor reaches
            // size (or the queue was empty to begin with) we're done.
            auto& pluginLoadQueue = reconfig.pluginQueue();
            const int pluginLoadCursor = reconfig.pluginCursor();
            if (pluginLoadCursor >= (int) pluginLoadQueue.size())
                loadingOverlay.hideOverlay();
            else
                loadingOverlay.setMessage ("Loading saved plugins  0 / "
                                           + juce::String (pluginLoadQueue.size()));
        };

        // When the user clicks any per-channel mute button while a panic is
        // active, discard the saved pre-panic snapshot so the next panic press
        // mutes everything from scratch (matches the user's spec: "panic forgets
        // the prior state once you manually touch a mute").
        matrixView.onUserMuteChanged = [this] {
            panic.noteUserMuteChanged();
            updatePanicButtonAppearance();
        };

        // Status Panel setup
        addChildComponent (statusPanel);
        statusHost.windowSize = [] { return juce::Point<int> { 600, 280 }; };
        statusHost.setPanelDetached = [this] (bool d) { statusPanel.setDetached (d); };
        statusHost.onChanged = [this] { switchTab (currentTab); };
        statusPanel.onPopOutRequested = [this] { statusHost.toggle(); };

        // Load persistent settings (engine SR, ring sizes, SRC quality, theme).
        engine.setSettings (SettingsStore::load());
        customLookAndFeel.applyTheme (engine.getSettings());
        // App brand label is always white -- not theme-tinted -- so the brand
        // reads cleanly against any accent colour the user picks.
        title.setColour (juce::Label::textColourId, juce::Colours::white);

        // React to hotplug: if a routed device disappears, gracefully drop it.
        engine.onDeviceListChanged = [this] {
            if (currentSpecs.empty())
                return;
            auto ins = engine.getAvailableInputDevices();
            auto outs = engine.getAvailableOutputDevices();
            std::vector<AudioEngine::DeviceSpec> filtered;
            bool lostDirection = false;
            for (auto& sp : currentSpecs)
            {
                const bool stillIn = sp.wantInput && ins.contains (sp.name);
                const bool stillOut = sp.wantOutput && outs.contains (sp.name);
                // A device we route actually lost a wanted direction (real removal).
                if (stillIn != sp.wantInput || stillOut != sp.wantOutput)
                    lostDirection = true;
                if (stillIn || stillOut)
                    filtered.push_back ({ sp.name, stillIn, stillOut });
            }
            // ONLY restart when a routed device actually lost a wanted direction.
            // Restarting on every device-list change loops forever once an app-audio
            // input is active: each attach/detach of the tap's private aggregate
            // device churns the CoreAudio device list, which would fire this handler,
            // which restarts the engine, which re-attaches the tap, which churns
            // again... (observed 2026-06-23 — a tight reconfigure loop). Benign churn
            // (our own tap aggregate, or an unrelated device appearing) leaves every
            // routed device intact, so we do nothing.
            if (lostDirection)
                applyDeviceSelection (filtered);
        };

        // Crash-guard: if the previous launch never reached markCleanExit() the
        // marker file is still there.  In that case ASK the user whether to
        // restore the last auto-saved session or start fresh, instead of silently
        // re-applying potentially-toxic state.  The marker is then re-armed for
        // this launch and only cleared in ~MainComponent after a successful save.
        const bool previousLaunchCrashed = CrashGuard::wasUnclean();
        const bool haveSnapshot = SnapshotStore::getLastUsedFile().existsAsFile();
        CrashGuard::markRunning();

        if (previousLaunchCrashed && haveSnapshot)
        {
            // Defer the ask to the next event loop tick so the main window is
            // already on screen when the dialog appears.
            auto alive = aliveToken;
            juce::MessageManager::callAsync ([this, alive] {
                if (!alive->load (std::memory_order_acquire))
                    return;
                juce::NativeMessageBox::showAsync (
                    juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                        .withTitle ("Recover previous session?")
                        .withMessage ("The last session of D-Router ended unexpectedly "
                                      "(likely a crash or force-quit).\n\n"
                                      "Would you like to restore the previous routing, "
                                      "groups and settings, or start with a blank project?")
                        .withButton ("Restore previous")
                        .withButton ("Start blank"),
                    [this, alive] (int result) {
                        if (!alive->load (std::memory_order_acquire))
                            return;
                        bool restored = false;
                        if (result == 1) // first button => Restore
                        {
                            Snapshot s;
                            if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s)
                                == SnapshotStore::LoadResult::Ok)
                            {
                                applySnapshot (s);
                                restored = true;
                            }
                        }
                        // "Start blank" (or a failed restore) leaves the engine with
                        // no devices selected; the user's snapshot stays on disk so
                        // they can still hand-load it via Load... later.  Crucially,
                        // applySnapshot() is what eventually hides the cold-start
                        // splash (via the matrix rebuild) -- so if we DIDN'T restore,
                        // hide it here or the app sits on the splash forever.
                        if (!restored)
                        {
                            loadingOverlay.hideOverlay();
                            matrixView.rebuildFromEngine(); // empty matrix + guidance
                        }
                    });
            });
        }
        else
        {
            // Normal clean-exit path: auto-restore last session immediately.
            Snapshot s;
            if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s)
                == SnapshotStore::LoadResult::Ok)
                applySnapshot (s);
            else
            {
                // Fresh install / no saved session: there's nothing to restore, so
                // applySnapshot() never runs -- and it's what hides the cold-start
                // splash (via the matrix rebuild).  Build the (empty) matrix view
                // ourselves: that hides the splash AND shows the "No devices
                // selected" guidance.  (This is what every first-time tester hits.)
                loadingOverlay.hideOverlay();
                matrixView.rebuildFromEngine();
            }
        }

        refreshStatus();
        switchTab (RoutingTab);
        startTimer (engine.getSettings().statusTimerMs);

#if JUCE_MAC
        // Install the native top menu bar (File / Edit / View / Window / Developer).
        // Shows only while the app is "regular" (window visible); macOS hides it
        // automatically when we drop to accessory (window hidden to the tray).
        // "About D-Router" goes in the app-name (Apple) menu where macOS users
        // expect it.  setMacMainMenu copies the extra menu, so the local is fine.
        juce::PopupMenu appleMenu;
        appleMenu.addItem (kMenuAboutId, "About D-Router");
        appleMenu.addItem (kMenuCheckUpdatesId, "Check for Updates...");
        juce::MenuBarModel::setMacMainMenu (this, &appleMenu);
#endif

        // Opt-in auto-update: one silent check a few seconds after launch (only a
        // newer version surfaces a prompt; ignoring it leaves this version running).
        juce::Component::SafePointer<MainComponent> safe (this);
        juce::Timer::callAfterDelay (3000, [safe] {
            if (safe != nullptr)
                safe->checkForUpdates (false);
        });
    }

    void MainComponent::timerCallback()
    {
        refreshStatus();
        // PDC reconcile backstop: plugin load/bypass and group edits don't funnel
        // through a single engine call, so re-derive the plan here.  Cheap and
        // idempotent (an unchanged plan is a no-op on the audio thread); skipped
        // entirely while PDC is off.
        if (engine.isPdcEnabled())
            engine.replanPdc();

        // Backstop: reconcile app-tap attach/detach against running processes.
        // Self-gates on reconfig.active(); the watcher's launch/quit callback also
        // calls this directly for promptness.
        reconcileAppAudioAttachments();
    }

    MainComponent::~MainComponent()
    {
#if JUCE_MAC
        // Tear the global menu's reference to us down FIRST, before any member
        // it dispatches into starts unwinding.
        juce::MenuBarModel::setMacMainMenu (nullptr);
#endif

        // Detach GPU context before the component tree starts unwinding.
        if (openGLContext.isAttached())
            openGLContext.detach();

        setLookAndFeel (nullptr);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

        if (reconfigThread.joinable())
            reconfigThread.join();

        // Flip the alive sentinel FIRST so any in-flight async plugin-restore
        // callbacks that haven't fired yet bail at their entry check instead of
        // touching us / engine / matrixView through dangling references.
        aliveToken->store (false, std::memory_order_release);

        // CRITICAL ORDER:
        //   1. stopProcessor() -- halt the matrix-processing thread so no more
        //      processBlock calls fire.  Plugin hosts STAY ALIVE.
        //   2. gatherCurrentSnapshot() -- safe to call getStateInformation()
        //      on every plugin now that nothing is processing them concurrently.
        //   3. engine.stop() -- full teardown that drops pluginHosts, workers
        //      and devices.
        // Previously we called engine.stop() first which clears pluginHosts,
        // so the snapshot saw an empty channel chain list and per-channel FX
        // never persisted across launches.  The stopProcessor() variant lets
        // us keep the race-protection AND save the FX.
        engine.stopProcessor();
        auto outgoingSnap = gatherCurrentSnapshot();
        engine.stop();
        SnapshotStore::save (SnapshotStore::getLastUsedFile(), outgoingSnap);
        // Only mark the clean-exit marker AFTER the snapshot write has been
        // requested; if save() throws or the process is killed before this
        // line, the marker stays on disk and next launch will prompt the user.
        CrashGuard::markCleanExit();
    }

    void MainComponent::parentHierarchyChanged()
    {
        // OpenGL deliberately NOT attached.  GL-using plugin editors (analyzer /
        // metering plug-ins) want to set up their own NSOpenGLContext.  When our
        // main window
        // already owns a JUCE OpenGLContext, the two contexts fight: the
        // plugin's editor reads the wrong framebuffer, GL state leaks across
        // contexts, and the plugin segfaults somewhere deep in AGXMetalG13X /
        // the AppleMetalOpenGLRenderer.  CoreAnimation paints this UI fine on
        // Apple Silicon; the OpenGLContext member is kept so we can re-enable
        // selectively in the future if we ever need it again.
        juce::ignoreUnused (openGLContext);
    }

    void MainComponent::paint (juce::Graphics& g)
    {
        g.fillAll (juce::Colour::fromRGB (12, 12, 14)); // Deep cyber black background
    }

    bool MainComponent::keyPressed (const juce::KeyPress& k)
    {
        // Cmd+S / Cmd+O -- map directly onto the existing Save... / Load...
        // dialogs so the toolbar button workflow stays unchanged.
        if (k == juce::KeyPress ('s', juce::ModifierKeys::commandModifier, 0))
        {
            saveSnapshotInteractive();
            return true;
        }
        if (k == juce::KeyPress ('o', juce::ModifierKeys::commandModifier, 0))
        {
            loadSnapshotInteractive();
            return true;
        }
        // Cmd+= / Cmd++ (both produce '=' on US layouts since '+' needs shift) /
        // Cmd+- adjust the engine-monitor body font size when that panel has any
        // visibility (covers detached window too).  Cmd+0 resets to default.
        if (k.getModifiers().isCommandDown())
        {
            const auto c = k.getTextCharacter();
            if (c == '=' || c == '+')
            {
                statusPanel.bumpBodyFontSize (+1);
                return true;
            }
            if (c == '-' || c == '_')
            {
                statusPanel.bumpBodyFontSize (-1);
                return true;
            }
            if (c == '0')
            {
                statusPanel.bumpBodyFontSize (0);
                return true;
            }
        }
        return false;
    }

    // ============================================================================
    // Native macOS menu bar (File / Edit / View / Window / Developer)
    // ============================================================================
    namespace
    {
        // Menu item command IDs.  Grouped by top-level menu for readability.
        enum MenuId {
            // File
            miSave = 1000,
            miLoad,
            miDevices,
            miSoftware,
            miSettings,
            miGroups,
            miCloseWindow,
            miQuit,
            // Edit
            miPanic = 1100,
            miReset,
            miTogglePdc,
            miAddAppInput,
            miClearAppInputs,
            // View
            miTabMatrix = 1200,
            miTabGroups,
            miTabAudioSetup,
            miTabMonitor,
            miExpandAll,
            miCollapseAll,
            miFontBigger,
            miFontSmaller,
            miFontReset,
            // Window
            miMinimize = 1300,
            miBringToFront,
            miShowDRouter,
            // Developer
            miOpenLogs = 1400,
            miRevealLogFolder,
            miRevealSettings,
            miForceRestart
        };
    }

    juce::StringArray MainComponent::getMenuBarNames()
    {
        return { "File", "Config", "View", "Window", "Developer" };
    }

    juce::PopupMenu MainComponent::getMenuForIndex (int topLevelMenuIndex, const juce::String&)
    {
        juce::PopupMenu m;
        const auto cmd = juce::ModifierKeys::commandModifier;

        switch (topLevelMenuIndex)
        {
            case 0: // File
                m.addItem (miSave, "Save Snapshot...", true, false);
                m.addItem (miLoad, "Load Snapshot...", true, false);
                m.addSeparator();
                m.addItem (miDevices, "Devices...", true, false);
                m.addItem (miSoftware, "Software...", appAudioProcesses != nullptr, false);
                m.addItem (miGroups, "Groups...", true, false);
                m.addItem (miSettings, "Settings...", true, false);
                m.addSeparator();
                m.addItem (miCloseWindow, "Close Window (hide to menu bar)", true, false);
                m.addItem (miQuit, "Quit D-Router", true, false);
                break;

            case 1: // Config (formerly Edit)
                m.addItem (miPanic, panic.isActive() ? "Release PANIC (restore mutes)" : "PANIC (mute everything)", true, panic.isActive());
                m.addItem (miReset, "Reset Engine (keep routing & FX)", !currentSpecs.empty(), false);
                m.addSeparator();
                m.addItem (miTogglePdc, "Plugin Delay Compensation (PDC)", true, engine.isPdcEnabled());
                m.addSeparator();
                m.addItem (miAddAppInput, "Add App Audio Input...", appAudioProcesses != nullptr, false);
                m.addItem (miClearAppInputs, "Clear App Audio Inputs", !currentAppInputs.empty(), false);
                break;

            case 2: // View
                m.addItem (miTabMatrix, "Matrix Routing", true, currentTab == RoutingTab);
                m.addItem (miTabGroups, "In / Out Groups", true, currentTab == GroupsTab);
                m.addItem (miTabAudioSetup, "Audio Setup", true, currentTab == AudioSetupTab);
                m.addItem (miTabMonitor, "Engine Monitor", true, currentTab == StatusTab);
                m.addSeparator();
                m.addItem (miExpandAll, "Expand All Devices", true, false);
                m.addItem (miCollapseAll, "Collapse All Devices", true, false);
                m.addSeparator();
                m.addItem (miFontBigger, "Monitor Font Bigger", true, false);
                m.addItem (miFontSmaller, "Monitor Font Smaller", true, false);
                m.addItem (miFontReset, "Monitor Font Reset", true, false);
                break;

            case 3: // Window
                m.addItem (miShowDRouter, "Show D-Router", true, false);
                m.addItem (miMinimize, "Minimize", true, false);
                m.addItem (miBringToFront, "Bring D-Router to Front", true, false);
                break;

            case 4: // Developer
                m.addItem (miOpenLogs, "Open Logs...", true, false);
                m.addItem (miRevealLogFolder, "Reveal Log Folder", true, false);
                m.addItem (miRevealSettings, "Reveal Settings File", true, false);
                m.addSeparator();
                m.addItem (miForceRestart, "Force Engine Restart (re-sync devices)", !currentSpecs.empty(), false);
                break;

            default:
                break;
        }

        juce::ignoreUnused (cmd);
        return m;
    }

    void MainComponent::menuItemSelected (int menuItemID, int /*topLevelMenuIndex*/)
    {
        auto fire = [] (juce::TextButton& b) { if (b.onClick) b.onClick(); };

        switch (menuItemID)
        {
            // File
            case miSave:
                saveSnapshotInteractive();
                break;
            case miLoad:
                loadSnapshotInteractive();
                break;
            case miDevices:
                fire (devicesButton);
                break;
            case miSoftware:
                fire (softwareButton);
                break;
            case miSettings:
                fire (settingsButton);
                break;
            case miGroups:
                fire (groupsButton);
                break;
            case miCloseWindow:
                hideToTray();
                break;
            case miQuit:
                if (auto* app = juce::JUCEApplication::getInstance())
                    app->systemRequestedQuit();
                break;

            // Edit
            case miPanic:
                if (panic.isActive())
                    panicRelease();
                else
                    panicActivate();
                break;
            case miReset:
                panicResetRestart();
                break;
            case miTogglePdc:
                engine.setPdcEnabled (!engine.isPdcEnabled());
                SettingsStore::save (engine.getSettings()); // persist the choice
                // Force the native menu bar to rebuild so the PDC item's checkmark
                // reflects the new state.  Without this the macOS menu keeps the
                // tick it was last built with -- the user toggles PDC and sees no
                // change (the reported bug).  Same reason the panic/tab handlers below
                // call it.
                menuItemsChanged();
                break;
            case miAddAppInput:
                openAppInputMenu();
                break;
            case miClearAppInputs:
                clearAppInputs();
                break;

            // View
            case miTabMatrix:
                switchTab (RoutingTab);
                break;
            case miTabGroups:
                switchTab (GroupsTab);
                break;
            case miTabAudioSetup:
                switchTab (AudioSetupTab);
                break;
            case miTabMonitor:
                switchTab (StatusTab);
                break;
            case miExpandAll:
                matrixView.collapseAllDevices (true, false);
                matrixView.collapseAllDevices (false, false);
                break;
            case miCollapseAll:
                matrixView.collapseAllDevices (true, true);
                matrixView.collapseAllDevices (false, true);
                break;
            case miFontBigger:
                statusPanel.bumpBodyFontSize (+1);
                break;
            case miFontSmaller:
                statusPanel.bumpBodyFontSize (-1);
                break;
            case miFontReset:
                statusPanel.bumpBodyFontSize (0);
                break;

            // Window
            case miMinimize:
                if (auto* w = getTopLevelComponent())
                    if (auto* dw = dynamic_cast<juce::DocumentWindow*> (w))
                        dw->setMinimised (true);
                break;
            case miBringToFront:
                if (auto* w = getTopLevelComponent())
                    w->toFront (true);
                break;
            case miShowDRouter:
                // Same as the tray's "Show D-Router": restore Dock + menu bar,
                // make the window visible, and pull it to the front / focus.
                juce::Process::setDockIconVisible (true);
                if (auto* w = getTopLevelComponent())
                {
                    w->setVisible (true);
                    w->toFront (true);
                }
                juce::Process::makeForegroundProcess();
                break;

            // Developer
            case miOpenLogs:
                fire (logsButton);
                break;
            case miRevealLogFolder:
                Logger::getLogDirectory().revealToUser();
                break;
            case miRevealSettings:
                SettingsStore::getFile().revealToUser();
                break;
            case miForceRestart:
                if (!currentSpecs.empty())
                    applyDeviceSelection (currentSpecs);
                break;

            // Apple menu
            case kMenuAboutId:
                showAboutDialog();
                break;
            case kMenuCheckUpdatesId:
                checkForUpdates (true);
                break;

            default:
                break;
        }
    }

    void MainComponent::showAboutDialog()
    {
        struct AboutContent : public juce::Component
        {
            juce::Label email;
            juce::TextButton copyBtn { "Copy email" };
            juce::TextButton closeBtn { "Close" };
            const juce::String kEmail { "yuanmz2005nf@gmail.com" };

            AboutContent()
            {
                email.setText (kEmail, juce::dontSendNotification);
                email.setJustificationType (juce::Justification::centred);
                email.setColour (juce::Label::textColourId, juce::Colour::fromRGB (0, 255, 210));
                email.setFont (juce::FontOptions (15.0f, juce::Font::bold));
                addAndMakeVisible (email);

                copyBtn.onClick = [this] {
                    juce::SystemClipboard::copyTextToClipboard (kEmail);
                    copyBtn.setButtonText ("Copied!");
                };
                addAndMakeVisible (copyBtn);

                closeBtn.onClick = [this] {
                    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
                        dw->exitModalState (0);
                };
                addAndMakeVisible (closeBtn);

                setSize (440, 320);
            }

            void paint (juce::Graphics& g) override
            {
                g.fillAll (juce::Colour::fromRGB (18, 18, 22));
                auto r = getLocalBounds().reduced (24).removeFromTop (200);

                g.setColour (juce::Colours::white);
                g.setFont (juce::FontOptions (32.0f, juce::Font::bold));
                g.drawText ("D-Router", r.removeFromTop (46), juce::Justification::centred);

                // PRIVATE BETA badge -- neon pill.
                auto badge = r.removeFromTop (30);
                r.removeFromTop (4);
                auto pill = badge.withSizeKeepingCentre (148, 24);
                g.setColour (juce::Colour::fromRGB (0, 255, 210).withAlpha (0.14f));
                g.fillRoundedRectangle (pill.toFloat(), 12.0f);
                g.setColour (juce::Colour::fromRGB (0, 255, 210));
                g.drawRoundedRectangle (pill.toFloat(), 12.0f, 1.2f);
                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText ("PRIVATE BETA", pill, juce::Justification::centred);

                g.setColour (juce::Colour::fromRGB (150, 150, 158));
                g.setFont (juce::FontOptions (12.0f));
                g.drawText (juce::String ("Version ") + JUCE_APPLICATION_VERSION_STRING
                                + "   -   built " + juce::String (__DATE__),
                    r.removeFromTop (20),
                    juce::Justification::centred);
                g.drawText ("NxM CoreAudio matrix router  -  ZDAudio",
                    r.removeFromTop (18),
                    juce::Justification::centred);

                g.setColour (juce::Colour::fromRGB (100, 100, 110));
                g.setFont (juce::FontOptions (11.0f));
                g.drawText ("Private beta -- not for redistribution. Contact:",
                    r.removeFromTop (26).removeFromBottom (16),
                    juce::Justification::centred);
            }

            void resized() override
            {
                auto r = getLocalBounds().reduced (24);
                auto bottom = r.removeFromBottom (40);
                email.setBounds (r.removeFromBottom (60).removeFromBottom (24));
                closeBtn.setBounds (bottom.removeFromRight (90));
                bottom.removeFromRight (8);
                copyBtn.setBounds (bottom.removeFromRight (110));
            }
        };

        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (new AboutContent());
        o.dialogTitle = "About D-Router";
        o.dialogBackgroundColour = juce::Colour::fromRGB (18, 18, 22);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void MainComponent::checkForUpdates (bool userInitiated)
    {
        const juce::String current = juce::JUCEApplication::getInstance()->getApplicationVersion();
        juce::Component::SafePointer<MainComponent> safe (this);

        updateChecker.check (current,
            [safe, userInitiated] (std::unique_ptr<dcr::update::ReleaseInfo> info, bool ok) {
                if (safe == nullptr)
                    return; // component gone (callback is on the msg thread)

                if (info != nullptr)
                {
                    safe->showUpdatePrompt (std::move (info));
                    return;
                }

                if (!userInitiated)
                    return; // silent launch check: stay quiet

                const juce::String cur = juce::JUCEApplication::getInstance()->getApplicationVersion();
                if (ok)
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon,
                        "You're up to date",
                        "D-Router " + cur + " is the latest version.");
                else
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon,
                        "Couldn't check for updates",
                        "Couldn't reach GitHub. Check your connection and try again.");
            });
    }

    void MainComponent::showUpdatePrompt (std::unique_ptr<dcr::update::ReleaseInfo> info)
    {
        const juce::String current = juce::JUCEApplication::getInstance()->getApplicationVersion();

        juce::DialogWindow::LaunchOptions o;
        o.content.setOwned (new dcr::update::UpdatePrompt (*info, current));
        o.dialogTitle = "Software Update";
        o.dialogBackgroundColour = juce::Colour::fromRGB (18, 18, 22);
        o.escapeKeyTriggersCloseButton = true;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.launchAsync();
    }

    void MainComponent::hideToTray()
    {
        if (auto* w = getTopLevelComponent())
            w->setVisible (false);
#if JUCE_MAC
        // Drop the Dock icon + menu bar -> pure menu-bar utility while hidden.
        juce::Process::setDockIconVisible (false);
#endif
    }

    void MainComponent::resized()
    {
        // Loading overlay always covers the entire window.
        loadingOverlay.setBounds (getLocalBounds());

        auto r = getLocalBounds().reduced (12);
        auto top = r.removeFromTop (32);

        // Left Configuration Section
        title.setBounds (top.removeFromLeft (240)); // wide enough for "ZDAudio D-Router" @ 22pt bold
        zdFace.setBounds (top.removeFromLeft (26).withSizeKeepingCentre (20, 20)); // easter-egg; only visible once unlocked
        top.removeFromLeft (10);
        devicesButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (4);
        softwareButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (4);
        groupsButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (4);
        settingsButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (4);

        // Right Session Section (Save, Load, Logs, [Reset], PANIC)
        stopButton.setBounds (top.removeFromRight (60));
        if (resetButton.isVisible())
        {
            top.removeFromRight (4);
            resetButton.setBounds (top.removeFromRight (60));
        }
        top.removeFromRight (12); // Extra separation for safety
        logsButton.setBounds (top.removeFromRight (70));
        top.removeFromRight (4);
        loadButton.setBounds (top.removeFromRight (70));
        top.removeFromRight (4);
        saveButton.setBounds (top.removeFromRight (70));

        r.removeFromTop (6);

        // Tab navigation row -- FlexBox keeps three buttons centered and equal
        // width regardless of how wide the window is.
        {
            auto tabRect = r.removeFromTop (28);
            juce::FlexBox fb;
            fb.flexDirection = juce::FlexBox::Direction::row;
            fb.justifyContent = juce::FlexBox::JustifyContent::center;
            fb.alignContent = juce::FlexBox::AlignContent::stretch;
            fb.items.add (juce::FlexItem (matrixTabBtn).withFlex (1.0f).withMinWidth (90.0f).withMaxWidth (220.0f).withMargin (juce::FlexItem::Margin (0, 2, 0, 0)));
            fb.items.add (juce::FlexItem (groupsTabBtn).withFlex (1.0f).withMinWidth (90.0f).withMaxWidth (220.0f).withMargin (juce::FlexItem::Margin (0, 2, 0, 2)));
            fb.items.add (juce::FlexItem (audioSetupTabBtn).withFlex (1.0f).withMinWidth (90.0f).withMaxWidth (220.0f).withMargin (juce::FlexItem::Margin (0, 2, 0, 2)));
            fb.items.add (juce::FlexItem (statusTabBtn).withFlex (1.0f).withMinWidth (90.0f).withMaxWidth (220.0f).withMargin (juce::FlexItem::Margin (0, 0, 0, 2)));
            fb.performLayout (tabRect);
        }

        r.removeFromTop (8);

        // Position active component in viewport space r
        if (currentTab == RoutingTab)
        {
            matrixView.setBounds (r);
        }
        else if (currentTab == GroupsTab)
        {
            // Top half: INPUT GROUPS  /  Bottom half: OUTPUT GROUPS
            auto topHalf = r.removeFromTop (r.getHeight() / 2);
            r.removeFromTop (6);
            auto bottomHalf = r;

            if (inputGroupHost.isDetached())
                inputGroupsPlaceholder.setBounds (topHalf);
            else
                inputGroupPanel.setBounds (topHalf);

            if (groupHost.isDetached())
                groupsPlaceholder.setBounds (bottomHalf);
            else
                groupPanel.setBounds (bottomHalf);
        }
        else if (currentTab == AudioSetupTab)
        {
            // Top half: INPUT DEVICES  /  Bottom half: OUTPUT DEVICES
            auto topHalf = r.removeFromTop (r.getHeight() / 2);
            r.removeFromTop (6);
            inputDeviceVolPanel.setBounds (topHalf);
            outputDeviceVolPanel.setBounds (r);
        }
        else if (currentTab == StatusTab)
        {
            if (statusHost.isDetached())
                statusPlaceholder.setBounds (r);
            else
                statusPanel.setBounds (r);
        }
    }

    int MainComponent::cards_default_width() { return 800; }

    void MainComponent::refreshStatus()
    {
        statusPanel.refreshNow();

        // Watchdog: if the OS renegotiated a device's sample rate / buffer size
        // out from under us (e.g. another app opened the same shared device and
        // flipped its nominal rate), our SRCs are now misconfigured and the
        // audio crackles.  Auto-recover with a preserve-state restart -- the same
        // path the Reset button uses.  Guarded by reconfig.active() so we don't
        // stack restarts.  open() now ADOPTS the device's new rate instead of
        // forcing it back (DeviceRateChoice.h), so a real change converges in a
        // single restart rather than ping-ponging against the other app.  The
        // FormatRestartGuard is the backstop for a device that flaps its rate on
        // its own: it rate-limits restarts so the watchdog can never spin.
        if (!reconfig.active()
            && !currentSpecs.empty()
            && engine.anyDeviceFormatChanged())
        {
            const double nowMs = (double) juce::Time::getMillisecondCounter();
            if (formatRestartGuard.allowRestart (nowMs))
            {
                formatBackoffLogged = false;
                juce::Logger::writeToLog ("MainComponent: device format change detected "
                                          "-- auto preserve-state restart to re-sync SRC.");
                applyDeviceSelection (currentSpecs);
            }
            else if (formatRestartGuard.isBackedOff() && !formatBackoffLogged)
            {
                formatBackoffLogged = true;
                juce::Logger::writeToLog ("MainComponent: device sample rate is flapping "
                                          "repeatedly; backing off auto-restart until it "
                                          "settles (re-select the device or Reset to retry).");
            }
        }
    }

    void MainComponent::stopEngine()
    {
        engine.stop();
        currentSpecs.clear();
        matrixView.rebuildFromEngine();
        refreshStatus();
    }

    void MainComponent::panicActivate()
    {
        if (!panic.engage (engine.getRoutingMatrix()))
            return; // empty matrix -- nothing to mute
        matrixView.refreshMuteButtonStates();
        updatePanicButtonAppearance();
        menuItemsChanged(); // Edit menu's PANIC label + tick follow inPanic
    }

    void MainComponent::panicRelease()
    {
        panic.release (engine.getRoutingMatrix());
        matrixView.refreshMuteButtonStates();
        updatePanicButtonAppearance();
        menuItemsChanged(); // Edit menu's PANIC label + tick follow inPanic
    }

    void MainComponent::updatePanicButtonAppearance()
    {
        const bool active = panic.isActive();
        // Bright red when active so the user always knows panic is engaged.
        stopButton.setButtonText (active ? "PANIC*" : "PANIC");
        stopButton.setColour (juce::TextButton::buttonColourId,
            active ? juce::Colour::fromRGB (180, 30, 30)
                   : juce::Colour::fromRGB (50, 50, 56));
        stopButton.setColour (juce::TextButton::buttonOnColourId,
            active ? juce::Colour::fromRGB (180, 30, 30)
                   : juce::Colour::fromRGB (50, 50, 56));
        stopButton.repaint();

        // RESET appears beside PANIC only while panic is engaged.  Re-run the
        // toolbar layout so the button slides in/out without leaving a gap.
        if (resetButton.isVisible() != active)
        {
            resetButton.setVisible (active);
            resized();
        }
    }

    void MainComponent::panicResetRestart()
    {
        // 1. Restore the pre-panic mute state FIRST, so the restart harvests the
        //    user's real routing -- not the all-muted panic snapshot (otherwise
        //    everything would come back muted).
        if (panic.isActive())
            panicRelease();

        // 2. Preserve-state engine restart: re-opens every device (re-reading the
        //    current OS sample rate / buffer size, fixing any crackle) while
        //    keeping routing, trims, FX and groups intact.
        if (!currentSpecs.empty())
            applyDeviceSelection (currentSpecs);
    }

    void MainComponent::openDeviceDialog()
    {
        DeviceManagerDialog::launch (engine, currentSpecs, [this] (std::optional<std::vector<AudioEngine::DeviceSpec>> sel) {
            if (!sel.has_value())
                return;
            // No-op guard: clicking OK without changing the selection must NOT tear
            // down and restart the engine -- a restart is audible (brief gap) and
            // pointless when nothing changed.  (Cancel already returns above.)
            if (*sel == currentSpecs)
                return;
            applyDeviceSelection (std::move (*sel));
        });
    }

    void MainComponent::applyDeviceSelection (std::vector<AudioEngine::DeviceSpec> newSpecs)
    {
        auto& pendingSnap = reconfig.snapshot(); // single owner; message-thread only
        if (!reconfig.tryBegin())
        {
            // Concurrent call rejected.  If applySnapshot stuffed pendingSnap
            // expecting THIS call to drain it, that data now belongs to the
            // dropped request -- clear it so the in-flight reconfig's tail
            // doesn't apply the wrong gains / mutes / plugin chains.
            pendingSnap = {};
            return;
        }

        // Matrix is about to be rebuilt - any saved-panic indices would point at
        // the old channel layout, so drop the panic state cleanly.
        if (panic.reset())
            updatePanicButtonAppearance();

        // ---- Preserve per-channel FX chains across the restart ---------------
        // engine.stop() wipes pluginHosts / inputPluginHosts (and the AU
        // instances inside them), so without explicit preservation every
        // Settings change would silently nuke the user's per-channel FX.
        // Group plugins survive automatically because OutputGroupManager is
        // not touched by engine.stop().
        //
        // The actual harvest (gatherCurrentSnapshot -> getStateInformation on
        // every AU) is DEFERRED to the worker thread below.  Reading plugin state
        // here -- on the message thread, while the matrix processor is still
        // running -- is UB per JUCE (concurrent with processBlock) and can yield a
        // torn state blob for heavy stateful AUs.  The worker
        // harvests only AFTER its fade-out + stopProcessor(), mirroring the safe
        // ~MainComponent shutdown order.
        //
        // preserveChains is false on the applySnapshot() path -- there pendingSnap
        // is already valid and owns the snapshot's own (different) plugin chains,
        // which must not be overwritten by a harvest of the live engine.
        const bool preserveChains = !pendingSnap.valid;

        // Close any open per-channel plugin editors NOW (message thread) before
        // the worker tears down PluginHosts.  Skipping this lets ~PluginEditor
        // call editorBeingDeleted() on a dead AudioPluginInstance -> segfault.
        matrixView.closeAllPluginEditors();

        // Capture current state on message thread (cheap), then offload the slow
        // CoreAudio open/close to a worker thread.
        auto preserved = captureMatrixByName();

        devicesButton.setEnabled (false);
        softwareButton.setEnabled (false);
        settingsButton.setEnabled (false);
        groupsButton.setEnabled (false);
        saveButton.setEnabled (false);
        loadButton.setEnabled (false);
        stopButton.setEnabled (false);
        // Status panel will pick this up on its next refresh tick.
        // Freeze EVERY UI-thread reader of engine state -- not just matrixView --
        // for the duration of the reconfigure.  Otherwise StatusPanel /
        // OutputGroupPanel timers can fire during processor.configure()'s
        // inputPeaks / outputPeaks move-assignment and hit a moved-from vector.
        matrixView.pauseUpdates();
        statusPanel.pauseUpdates();
        groupPanel.pauseUpdates();
        inputGroupPanel.pauseUpdates();
        inputDeviceVolPanel.pauseUpdates();
        outputDeviceVolPanel.pauseUpdates();
        stopTimer();
        // The 5 s PERF probe is a message-thread reader of engine state too -- it
        // walks the worker / plugin-host vectors via engine.getDeviceLiveness(),
        // getMinOutputRingHeadroomMs(), collectTopPluginCpu() etc.  Left running it
        // races the worker thread's engine.stop()/start() below and dereferences a
        // moved-from DeviceWorker* (observed SIGSEGV in PerfMonitor::emitSnapshot,
        // KERN_INVALID_ADDRESS at 0x20).  Freeze it for the reconfigure window too.
        perfMonitor.pause();

        // Show the loading splash so the user knows the app is busy and
        // hasn't crashed.  Hidden when matrixView's chunked rebuild finishes.
        // The overlay is AlwaysOnTop and intercepts every mouse click, which
        // also serves as our input-disable guard against the matrix.resize()
        // UAF race -- much cheaper than recursively setEnabled(false) on
        // 600+ children (each enablementChanged() triggers repaint()).
        loadingOverlay.showOverlay ("Reconfiguring engine...");

        if (reconfigThread.joinable())
            reconfigThread.join();

        auto specs = std::move (newSpecs);

        // Instantiate the AudioIODevices NOW, on the message thread, BEFORE handing
        // the reconfigure to the worker thread.  scanForDevices() + createDevice()
        // touch the non-thread-safe deviceType, which JUCE ALSO rescans on the
        // message thread on every hardware change -- doing it on the worker thread
        // (as the old engine.start did) raced that internal scan and corrupted the
        // heap (SIGABRT in CoreAudioIODeviceType::scanForDevices, seen under
        // reconfigure + hotplug / app-tap aggregate teardown).  The worker thread
        // below only OPENS these pre-created devices, never touching deviceType.
        // Cost: the (usually fast) scan runs on the message thread; the slow
        // device->open() + setup stays on the worker thread, so the UI stays
        // responsive behind the "Reconfiguring..." overlay.
        engine.createDeviceWorkers (specs);

        reconfigThread = std::thread ([this, specs, preserved, preserveChains, appInputs = currentAppInputs] {
            // Graceful fade-out before tearing the engine down.  Ramp the MASTER
            // output gain to silence (the matrix smooths it over ~5*tau), then
            // sleep for the ramp to complete before stopping devices.  This MUST
            // run before stopProcessor() -- once the matrix thread is halted no
            // processBlock fires and the ramp can't advance (-> click).
            //
            // CRITICAL: we fade the master gain, NOT the per-channel output trims.
            // Zeroing the trims here used to leak a permanent -60 dB ("silent")
            // state -- if the restart aborted, or a snapshot save raced the fade
            // window, the user's output trims stayed at 0 and every fresh route was
            // inaudible.  The master gain dies with this engine and the next one
            // starts at unity, so trims are never touched.
            {
                const int smoothMs = juce::jmax (5, engine.getSettings().gainSmoothingMs);
                engine.setOutputMasterGain (0.0f);
                std::this_thread::sleep_for (std::chrono::milliseconds (smoothMs * 5));
            }

            // The fade has completed, so halt the matrix-processing thread BEFORE
            // reading any plugin state.  After stopProcessor() no processBlock()
            // can fire, so harvestChannelChains() -> getStateInformation() is
            // race-free (the same ordering ~MainComponent uses).  Plugin hosts stay
            // alive until engine.stop() just below.  We harvest into a LOCAL and
            // hand it to the message thread via callAsync -- writing pendingSnap
            // from this worker thread would race the re-entrancy reset
            // (pendingSnap = {}) at the top of applyDeviceSelection, which a
            // CoreAudio device-hotplug callback can trigger mid-reconfigure.
            engine.stopProcessor();
            std::vector<Snapshot::ChannelChain> harvestedChains;
            if (preserveChains)
            {
                harvestedChains = harvestChannelChains();
                juce::Logger::writeToLog ("applyDeviceSelection: preserved "
                                          + juce::String ((int) harvestedChains.size())
                                          + " channel FX chain(s) across restart");
            }

            reconfig.advance (ReconfigurationController::Phase::Rebuilding);
            engine.stop();
            const bool started = specs.empty() ? true : engine.start (specs, appInputs);

            auto alive = aliveToken;
            juce::MessageManager::callAsync (
                [this, alive, specs, preserved, started, preserveChains, chainsToRestore = std::move (harvestedChains)]() mutable {
                    if (!alive->load (std::memory_order_acquire))
                        return;
                    auto& pendingSnap = reconfig.snapshot(); // single owner; message-thread only
                    reconfig.advance (ReconfigurationController::Phase::RestoringMatrix);
                    currentSpecs = specs;
                    // App workers come back DETACHED from engine.start(); reset the
                    // tracked attach state so the next reconcile re-binds running apps.
                    appAttachedPids.assign ((size_t) engine.getNumAppWorkers(), 0);
                    if (!specs.empty() && !started)
                    {
                    } // failure is visible via empty matrix + status panel
                    else
                        restoreMatrixByName (preserved);

                    // Settings-apply (non-snapshot) path: publish the chains the worker
                    // harvested after stopProcessor into pendingSnap NOW, on the message
                    // thread, so the drain below re-instantiates them.  Doing it here
                    // (not in the worker) keeps pendingSnap single-threaded.  groupChains
                    // stays empty -- group plugin objects survive the restart in-place.
                    if (preserveChains)
                    {
                        pendingSnap.channelChains = std::move (chainsToRestore);
                        pendingSnap.valid = true;
                    }

                    // Snapshot apply (cold-start or Load...): drain pendingSnap NOW,
                    // after engine.start() resized the matrix to the new (in, out)
                    // size.  If we did this earlier the resize would have zeroed
                    // our writes.
                    if (pendingSnap.valid)
                    {
                        auto& m = engine.getRoutingMatrix();
                        const int nIn = m.getNumInputs();
                        const int nOut = m.getNumOutputs();
                        for (size_t n = 0; n < pendingSnap.inputTrim.size() && (int) n < nIn; ++n)
                            m.setInputTrim ((int) n, pendingSnap.inputTrim[n]);
                        for (size_t n = 0; n < pendingSnap.outputTrim.size() && (int) n < nOut; ++n)
                            m.setOutputTrim ((int) n, pendingSnap.outputTrim[n]);
                        for (size_t n = 0; n < pendingSnap.inputMute.size() && (int) n < nIn; ++n)
                            m.setInputMute ((int) n, pendingSnap.inputMute[n] != 0);
                        for (size_t n = 0; n < pendingSnap.outputMute.size() && (int) n < nOut; ++n)
                            m.setOutputMute ((int) n, pendingSnap.outputMute[n] != 0);
                        for (size_t n = 0; n < pendingSnap.inputSolo.size() && (int) n < nIn; ++n)
                            m.setInputSolo ((int) n, pendingSnap.inputSolo[n] != 0);
                        for (const auto& xp : pendingSnap.crosspoints)
                            if (xp.outputCh < nOut && xp.inputCh < nIn)
                                m.setCrosspoint (xp.outputCh, xp.inputCh, xp.gain);

                        // Plugin instantiation is async; fire it off and let each
                        // callback land back here over the next few hundred ms.
                        restorePluginChainsAsync();

                        pendingSnap = {};
                    }

                    // Async plugin restore has been kicked (or there was none);
                    // the loads themselves continue past finish() under aliveToken.
                    reconfig.advance (ReconfigurationController::Phase::RestoringPlugins);

                    matrixView.rebuildFromEngine();
                    matrixView.resumeUpdates();
                    groupPanel.rebuild();
                    inputGroupPanel.rebuild();
                    inputDeviceVolPanel.rebuild();
                    outputDeviceVolPanel.rebuild();
                    // Resume the panels that were paused at the top of this call.
                    statusPanel.resumeUpdates();
                    groupPanel.resumeUpdates();
                    inputGroupPanel.resumeUpdates();
                    inputDeviceVolPanel.resumeUpdates();
                    outputDeviceVolPanel.resumeUpdates();
                    // Re-arm the PERF probe frozen before the worker thread launched.
                    // This continuation runs on the message thread AFTER engine.start()
                    // returned, so the worker / plugin-host vectors are stable again --
                    // and it runs even when start() failed (workers then empty -> the
                    // probe's loops are no-ops), so the probe always comes back.
                    perfMonitor.resume();
                    // Input was blocked via the loadingOverlay's interceptsMouseClicks,
                    // which the overlay drops on its own when it hides.  No setEnabled
                    // toggle needed any more (and it was triggering hundreds of
                    // enablementChanged() repaints, lagging the UI right after rebuild).
                    startTimer (engine.getSettings().statusTimerMs);
                    refreshStatus();

                    devicesButton.setEnabled (true);
                    softwareButton.setEnabled (true);
                    settingsButton.setEnabled (true);
                    groupsButton.setEnabled (true);
                    saveButton.setEnabled (true);
                    loadButton.setEnabled (true);
                    stopButton.setEnabled (true);
                    reconfig.advance (ReconfigurationController::Phase::Running);
                    reconfig.finish();
                });
        });
    }

    void MainComponent::reconcileAppAudioAttachments()
    {
        // Runs on the message thread (the status timer + the watcher's launch/quit
        // callback).  applyDeviceSelection also runs here, so gating on
        // reconfig.active() alone is race-free: a reconfigure can't interleave
        // mid-reconcile on a single thread, and app workers are stable while Idle.
        if (reconfig.active() || appAudioProcesses == nullptr)
            return;
        const int n = engine.getNumAppWorkers();
        if (n == 0)
            return;
        const auto& specs = engine.getAppInputSpecs();
        if ((int) specs.size() != n)
            return; // invariant: one worker per spec
        if ((int) appAttachedPids.size() != n)
            appAttachedPids.assign ((size_t) n, 0);

        // Currently-running tappable processes (bundle id -> process-object id).
        std::vector<dcr::appinput::RunningProcess> running;
        for (const auto& e : appAudioProcesses->enumerate())
            running.push_back ({ e.bundleId, (int) e.processObject });

        // The configured sources with their current attach state.
        std::vector<dcr::appinput::Source> sources;
        for (int i = 0; i < n; ++i)
            sources.push_back ({ specs[(size_t) i].bundleId.toStdString(), appAttachedPids[(size_t) i] });

        for (const auto& cmd : dcr::appinput::reconcile (sources, running))
        {
            auto* w = engine.getAppWorker (cmd.sourceIndex);
            if (w == nullptr)
                continue;
            if (cmd.type == dcr::appinput::CommandType::Attach)
            {
                const bool ok = w->attach ((AudioObjectID) cmd.processId);
                juce::Logger::writeToLog ("appaudio.reconcile: ATTACH src=" + juce::String (cmd.sourceIndex)
                                          + " proc=" + juce::String (cmd.processId) + " -> " + juce::String (ok ? "ok" : "FAIL"));
                if (ok)
                    appAttachedPids[(size_t) cmd.sourceIndex] = cmd.processId;
            }
            else
            {
                w->detach();
                juce::Logger::writeToLog ("appaudio.reconcile: DETACH src=" + juce::String (cmd.sourceIndex));
                appAttachedPids[(size_t) cmd.sourceIndex] = 0;
            }
        }
    }

    void MainComponent::openAppInputMenu()
    {
        if (appAudioProcesses == nullptr)
            return;

        // Picker of apps currently producing output, de-duped by bundle id so a
        // helper process can't flood the list.
        juce::PopupMenu menu;
        std::vector<std::pair<juce::String, juce::String>> items; // bundleId, displayName
        juce::StringArray seen;
        for (const auto& e : appAudioProcesses->enumerate())
        {
            if (!e.runningOutput)
                continue;
            const juce::String bid = juce::String::fromUTF8 (e.bundleId.c_str());
            if (bid.isEmpty() || seen.contains (bid))
                continue;
            if (bid.startsWith ("com.zdaudio.drouter"))
                continue; // never capture ourselves -- that's a feedback loop
            seen.add (bid);
            const juce::String name = juce::String::fromUTF8 (
                e.displayName.empty() ? e.bundleId.c_str() : e.displayName.c_str());
            items.push_back ({ bid, name });
            menu.addItem ((int) items.size(), name);
        }
        if (items.empty())
            menu.addItem (-1, "(no app is currently producing audio)", false, false);

        menu.showMenuAsync (juce::PopupMenu::Options {}, [this, items] (int choice) {
            if (choice >= 1 && choice <= (int) items.size())
                addAppInput (items[(size_t) (choice - 1)].first, items[(size_t) (choice - 1)].second);
        });
    }

    void MainComponent::addAppInput (const juce::String& bundleId, const juce::String& displayName)
    {
        for (const auto& s : currentAppInputs)
            if (s.bundleId == bundleId)
                return; // one capture per app (v1)

        AudioEngine::AppInputSpec spec;
        spec.bundleId = bundleId;
        spec.displayName = displayName;
        currentAppInputs.push_back (spec);
        applyDeviceSelection (currentSpecs); // reconfigure: existing devices + new app inputs
    }

    void MainComponent::clearAppInputs()
    {
        if (currentAppInputs.empty())
            return;
        currentAppInputs.clear();
        applyDeviceSelection (currentSpecs);
    }

    namespace
    {
        struct LayoutMap
        {
            const char* name;
            juce::AudioChannelSet set;
        };
        const std::vector<LayoutMap>& layoutTable()
        {
            static const std::vector<LayoutMap> t = {
                { "Mono", juce::AudioChannelSet::mono() },
                { "Stereo", juce::AudioChannelSet::stereo() },
                { "Quad", juce::AudioChannelSet::quadraphonic() },
                { "5.1", juce::AudioChannelSet::create5point1() },
                { "7.1", juce::AudioChannelSet::create7point1() },
                { "7.1.2", juce::AudioChannelSet::create7point1point2() },
                { "7.1.4", juce::AudioChannelSet::create7point1point4() },
            };
            return t;
        }
        juce::String layoutName (const juce::AudioChannelSet& s)
        {
            for (auto& e : layoutTable())
                if (e.set == s)
                    return e.name;
            return "Stereo";
        }
        juce::AudioChannelSet layoutFromName (const juce::String& n)
        {
            for (auto& e : layoutTable())
                if (n == e.name)
                    return e.set;
            return juce::AudioChannelSet::stereo();
        }

        Snapshot::PluginSlotState gatherPluginSlot (juce::AudioPluginInstance* p, bool bypassed)
        {
            Snapshot::PluginSlotState ps;
            if (p == nullptr)
                return ps;
            // Both calls go through SafePluginOps -- they NSException-catch the
            // AU bridge.  If either throws, we just save an empty slot record;
            // snapshot save mustn't take the app down.
            dcr::safe::getPluginDescriptionXml (*p, ps.descriptionXml);
            juce::MemoryBlock blob;
            dcr::safe::getStateInformation (*p, blob);
            if (blob.getSize() > 0)
                ps.stateB64 = juce::Base64::toBase64 (blob.getData(), blob.getSize());
            ps.bypassed = bypassed;
            return ps;
        }
    }

    std::vector<Snapshot::ChannelChain> MainComponent::harvestChannelChains() const
    {
        // NOTE: getStateInformation() (inside gatherPluginSlot) is UB when run
        // concurrently with the audio thread's processBlock(); callers must stop
        // the matrix processor first.  This intentionally omits group chains -- the
        // group plugin objects survive an engine restart in-place, so re-saving and
        // re-instantiating them would double-load for nothing.
        std::vector<Snapshot::ChannelChain> chains;
        auto& engineRef = const_cast<AudioEngine&> (engine);
        const auto& m = engine.getRoutingMatrix();
        const int nIn = m.getNumInputs();
        const int nOut = m.getNumOutputs();

        auto gatherChain = [&] (PluginHost* host, int globalIdx, bool isInput) {
            if (host == nullptr)
                return;
            Snapshot::ChannelChain cc;
            cc.globalIdx = globalIdx;
            cc.isInput = isInput;
            cc.slots.resize ((size_t) PluginHost::kNumSlots);
            bool any = false;
            for (int sl = 0; sl < PluginHost::kNumSlots; ++sl)
            {
                cc.slots[(size_t) sl] = gatherPluginSlot (host->getPluginAt (sl), host->isBypassedAt (sl));
                if (!cc.slots[(size_t) sl].isEmpty())
                    any = true;
            }
            if (any)
                chains.push_back (std::move (cc));
        };

        for (int ch = 0; ch < nIn; ++ch)
            gatherChain (engineRef.getInputPluginHost (ch), ch, true);
        for (int ch = 0; ch < nOut; ++ch)
            gatherChain (engineRef.getPluginHost (ch), ch, false);
        return chains;
    }

    Snapshot MainComponent::gatherCurrentSnapshot() const
    {
        Snapshot s;
        s.engineSampleRate = engine.getEngineSampleRate();
        s.engineBlockSize = engine.getEngineBlockSize();
        s.devices = currentSpecs;
        s.appInputs = currentAppInputs;

        auto gatherFromManager = [&] (auto& mgr, std::vector<Snapshot::Group>& dest) {
            for (int gi = 0; gi < mgr.getNumGroups(); ++gi)
            {
                const auto* g = mgr.getGroup (gi);
                if (g == nullptr)
                    continue;
                // Auto-derived groups -- SoftIn (over an app-audio source) and
                // DeviceAuto (over a stereo output device) -- are rebuilt on every
                // engine start.  Persisting one would restore an orphaned group; skip
                // it -- restoring the source (app input / device) rebuilds it instead.
                if (g->kind.load (std::memory_order_relaxed) != OutputGroup::Kind::Regular)
                    continue; // SoftIn / DeviceAuto are derived + rebuilt each start
                Snapshot::Group gs;
                gs.name = g->name;
                gs.layoutName = layoutName (g->channelSet);
                gs.memberChannels = g->memberChannels;
                gs.faderDb = g->faderDb.load();
                gs.muted = g->muted.load();
                gs.faderMode = (int) g->faderMode.load();
                dest.push_back (std::move (gs));
            }
        };
        gatherFromManager (engine.getGroupManager(), s.outputGroups);
        gatherFromManager (engine.getInputGroupManager(), s.inputGroups);

        const auto& m = engine.getRoutingMatrix();
        const int nIn = m.getNumInputs();
        const int nOut = m.getNumOutputs();
        s.inputTrim.resize ((size_t) nIn);
        s.outputTrim.resize ((size_t) nOut);
        s.inputMute.resize ((size_t) nIn, 0);
        s.outputMute.resize ((size_t) nOut, 0);
        s.inputSolo.resize ((size_t) nIn, 0);
        for (int n = 0; n < nIn; ++n)
        {
            s.inputTrim[(size_t) n] = m.getInputTrim (n);
            s.inputMute[(size_t) n] = m.getInputMute (n) ? 1 : 0;
            s.inputSolo[(size_t) n] = m.getInputSolo (n) ? 1 : 0;
        }
        for (int n = 0; n < nOut; ++n)
        {
            s.outputTrim[(size_t) n] = m.getOutputTrim (n);
            s.outputMute[(size_t) n] = m.getOutputMute (n) ? 1 : 0;
        }
        for (int o = 0; o < nOut; ++o)
            for (int i = 0; i < nIn; ++i)
            {
                const float g = m.getCrosspoint (o, i);
                if (g > 1.0e-6f)
                    s.crosspoints.push_back ({ o, i, g });
            }

        // ===== Plugin chains: per-channel ========================================
        s.channelChains = harvestChannelChains();

        // ===== Plugin chains: per-group ==========================================
        auto gatherGroupChains = [&] (auto& mgr, bool isInput, std::vector<Snapshot::GroupChain>& out) {
            for (int gi = 0; gi < mgr.getNumGroups(); ++gi)
            {
                auto* g = mgr.getGroup (gi);
                if (g == nullptr)
                    continue;
                if (g->kind.load (std::memory_order_relaxed) != OutputGroup::Kind::Regular)
                    continue; // SoftIn / DeviceAuto are derived -- not persisted (see gatherFromManager)
                Snapshot::GroupChain gc;
                gc.groupIdx = gi;
                gc.isInput = isInput;
                gc.slots.resize (g->pluginSlots.size());
                bool any = false;
                for (size_t sl = 0; sl < g->pluginSlots.size(); ++sl)
                {
                    if (auto& host = g->pluginSlots[sl])
                    {
                        gc.slots[sl] = gatherPluginSlot (host->getPlugin(), host->isBypassed());
                        if (!gc.slots[sl].isEmpty())
                            any = true;
                    }
                }
                if (any)
                    out.push_back (std::move (gc));
            }
        };
        gatherGroupChains (const_cast<AudioEngine&> (engine).getGroupManager(), false, s.groupChains);
        gatherGroupChains (const_cast<AudioEngine&> (engine).getInputGroupManager(), true, s.groupChains);

        // Matrix-view UI state: which devices are folded.  Stored as names so a
        // future device ordering change doesn't silently shift the mapping.
        s.collapsedInputDevices = matrixView.getCollapsedDeviceNames (true);
        s.collapsedOutputDevices = matrixView.getCollapsedDeviceNames (false);

        return s;
    }

    void MainComponent::applySnapshot (const Snapshot& s)
    {
        // Snapshot's SR/block override the persisted settings.  Other tunables
        // (ring sizes, SRC, timers) keep the user's settings.
        auto newSettings = engine.getSettings();
        newSettings.engineSampleRate = s.engineSampleRate;
        newSettings.engineBlockSize = s.engineBlockSize;
        engine.setSettings (newSettings);

        // Group plugin editors reference AudioPluginInstance objects owned by the
        // groups about to be wiped.  Close them on the message thread NOW so the
        // editor dtor doesn't run against a dead processor later (segfault).
        groupPanel.closeAllPluginEditors();
        inputGroupPanel.closeAllPluginEditors();

        // Wipe and re-create groups from the snapshot BEFORE restarting the
        // engine -- the engine restart will then prepare their plugin hosts.
        auto restoreToManager = [&] (auto& mgr, const std::vector<Snapshot::Group>& src) {
            while (mgr.getNumGroups() > 0)
                mgr.removeGroup (0);
            for (const auto& gs : src)
            {
                const int gi = mgr.createGroup (gs.name, layoutFromName (gs.layoutName));
                if (auto* g = mgr.getGroup (gi))
                {
                    const int n = juce::jmin ((int) gs.memberChannels.size(),
                        (int) g->memberChannels.size());
                    for (int i = 0; i < n; ++i)
                        g->memberChannels[(size_t) i] = gs.memberChannels[(size_t) i];
                    g->faderDb.store (gs.faderDb);
                    g->muted.store (gs.muted);
                    g->faderMode.store ((OutputGroup::FaderMode) gs.faderMode);
                    // channelRouterGain is recomputed by the engine restart's
                    // setNum*Channels() that runs right after this restore.
                }
            }
        };
        restoreToManager (engine.getGroupManager(), s.outputGroups);
        restoreToManager (engine.getInputGroupManager(), s.inputGroups);

        // Cannot write the matrix here -- engine.start() is about to run on the
        // reconfig worker thread and will resize the matrix to (totalIns,
        // totalOuts), zeroing everything.  Stash the values; the reconfig
        // callAsync drains pendingSnap AFTER the new matrix is live.
        auto& pendingSnap = reconfig.snapshot(); // single owner; message-thread only
        pendingSnap.inputTrim = s.inputTrim;
        pendingSnap.outputTrim = s.outputTrim;
        pendingSnap.inputMute = s.inputMute;
        pendingSnap.outputMute = s.outputMute;
        pendingSnap.inputSolo = s.inputSolo;
        pendingSnap.crosspoints = s.crosspoints;
        pendingSnap.channelChains = s.channelChains;
        pendingSnap.groupChains = s.groupChains;
        pendingSnap.valid = true;

        // Restore matrix-view UI collapse state BEFORE the rebuild so the
        // first paint already reflects it (no flash-then-collapse).
        matrixView.setCollapsedDeviceNames (true, s.collapsedInputDevices);
        matrixView.setCollapsedDeviceNames (false, s.collapsedOutputDevices);

        // Restore app-audio (Soft-In) sources so engine.start rebuilds their
        // Soft-In groups and the watcher re-attaches the taps once those apps are
        // running.  Must precede applyDeviceSelection, which reads currentAppInputs
        // (and resets appAttachedPids to match).
        currentAppInputs = s.appInputs;

        applyDeviceSelection (s.devices);
        // Don't touch the matrix or rebuildFromEngine() here.  applyDeviceSelection
        // does both, then drains pendingSnap, then rebuilds the UI.
    }

    void MainComponent::restorePluginChainsAsync()
    {
        // The reconfigure payload + queue live in the controller (single owner);
        // alias them so the body below reads unchanged.  Message-thread only.
        auto& pendingSnap = reconfig.snapshot();
        auto& pluginLoadQueue = reconfig.pluginQueue();
        int& pluginLoadCursor = reconfig.pluginCursor();
        uint32_t& pluginLoadStartMs = reconfig.pluginStartMs();

        if (pendingSnap.channelChains.empty() && pendingSnap.groupChains.empty())
            return;

        // Build a single linear queue, then drain it strictly one-at-a-time
        // from processNextPluginLoad().  Firing all loads at once used to
        // peg the message thread for ~30 s on a snapshot with a dozen heavy
        // AUs -- see PendingPluginLoad's comment.
        pluginLoadQueue.clear();
        pluginLoadCursor = 0;
        pluginLoadStartMs = (uint32_t) juce::Time::getMillisecondCounter();

        auto decode = [] (const juce::String& b64, juce::MemoryBlock& out) {
            if (b64.isEmpty())
                return;
            juce::MemoryOutputStream mos (out, false);
            juce::Base64::convertFromBase64 (mos, b64);
        };

        // Per-channel chains.
        for (auto& ch : pendingSnap.channelChains)
        {
            for (int slotIdx = 0; slotIdx < (int) ch.slots.size(); ++slotIdx)
            {
                const auto& ps = ch.slots[(size_t) slotIdx];
                if (ps.isEmpty())
                    continue;

                PendingPluginLoad job;
                if (!dcr::safe::loadPluginDescriptionFromXml (ps.descriptionXml, job.desc))
                    continue;
                decode (ps.stateB64, job.state);
                job.bypassed = ps.bypassed;
                job.slotIdx = slotIdx;
                job.kind = PendingPluginLoad::Kind::ChannelSlot;
                job.isInputChannel = ch.isInput;
                job.globalChannelIdx = ch.globalIdx;
                pluginLoadQueue.push_back (std::move (job));
            }
        }

        // Per-group chains.
        for (auto& gc : pendingSnap.groupChains)
        {
            // channelSet is captured at queue time -- this lets the worker
            // resolve which channels the group covers without re-looking-up the
            // OutputGroup pointer until just before the install.
            OutputGroup* g = gc.isInput ? engine.getInputGroupManager().getGroup (gc.groupIdx)
                                        : engine.getGroupManager().getGroup (gc.groupIdx);
            if (g == nullptr)
                continue;

            for (int slotIdx = 0; slotIdx < (int) gc.slots.size(); ++slotIdx)
            {
                const auto& ps = gc.slots[(size_t) slotIdx];
                if (ps.isEmpty())
                    continue;

                PendingPluginLoad job;
                if (!dcr::safe::loadPluginDescriptionFromXml (ps.descriptionXml, job.desc))
                    continue;
                decode (ps.stateB64, job.state);
                job.bypassed = ps.bypassed;
                job.slotIdx = slotIdx;
                job.kind = PendingPluginLoad::Kind::GroupSlot;
                job.isInputGroup = gc.isInput;
                job.groupIdx = gc.groupIdx;
                job.channelSet = g->channelSet;
                pluginLoadQueue.push_back (std::move (job));
            }
        }

        if (pluginLoadQueue.empty())
            return;

        juce::Logger::writeToLog ("PluginRestore: queued " + juce::String (pluginLoadQueue.size())
                                  + " plugin(s) for serialized cold-start instantiation");
        processNextPluginLoad();
    }

    void MainComponent::processNextPluginLoad()
    {
        // Queue + cursor live in the controller (single owner); alias so the
        // body reads unchanged.  Message-thread only.
        auto& pluginLoadQueue = reconfig.pluginQueue();
        int& pluginLoadCursor = reconfig.pluginCursor();
        const uint32_t pluginLoadStartMs = reconfig.pluginStartMs();

        if (pluginLoadCursor >= (int) pluginLoadQueue.size())
        {
            juce::Logger::writeToLog ("PluginRestore: queue drained ("
                                      + juce::String ((uint32_t) juce::Time::getMillisecondCounter()
                                                      - pluginLoadStartMs)
                                      + " ms total)");
            // Now that the queue is empty we can safely drop the splash.
            // The matrix-rebuild branch may have already tried to hide it
            // and bailed because we were still loading; clean it up here.
            loadingOverlay.hideOverlay();
            return;
        }

        const auto& job = pluginLoadQueue[(size_t) pluginLoadCursor];
        const auto jobStart = juce::Time::getMillisecondCounter();
        const auto cursor = pluginLoadCursor;
        const auto pluginName = job.desc.name;

        // Show progress on the splash so the user sees the app is busy
        // doing useful work, not stuck.
        loadingOverlay.setMessage ("Loading saved plugins  "
                                   + juce::String (cursor + 1) + " / "
                                   + juce::String (pluginLoadQueue.size())
                                   + "   (" + pluginName + ")");
        loadingOverlay.setProgress (cursor, (int) pluginLoadQueue.size());

        auto& fmt = engine.getPluginFormatManager();
        const double sr = engine.getEngineSampleRate();
        const int bs = engine.getEngineBlockSize();

        juce::Logger::writeToLog ("PluginRestore: [" + juce::String (cursor + 1) + "/"
                                  + juce::String (pluginLoadQueue.size())
                                  + "] loading '" + pluginName + "'...");

        auto alive = aliveToken;
        fmt.createPluginInstanceAsync (job.desc, sr, bs, [this, alive, jobStart, cursor, pluginName] (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err) {
            juce::ignoreUnused (err);
            if (!alive->load (std::memory_order_acquire))
                return;
            auto& pluginLoadQueue = reconfig.pluginQueue(); // single owner; message-thread only
            int& pluginLoadCursor = reconfig.pluginCursor();
            // Re-read the job (its slot in the queue may have moved if a new
            // restore was kicked off, but pluginLoadCursor is stable per-run).
            if (cursor >= (int) pluginLoadQueue.size())
                return;
            const auto& j = pluginLoadQueue[(size_t) cursor];

            if (instance != nullptr)
            {
                // IMPORTANT: do NOT call setStateInformation on the raw,
                // unprepared instance here.  The host's setPluginAt / setPlugin
                // now applies the saved state in the canonical order
                // (prepareToPlay THEN setStateInformation) while the instance
                // is still off the audio thread.  The old "set state then let
                // setPluginAt re-prepare" sequence discarded the state or threw
                // out of stateful AUs, leaving the slot broken
                // and silent until a manual reload.
                const juce::MemoryBlock* statePtr =
                    j.state.getSize() > 0 ? &j.state : nullptr;

                if (j.kind == PendingPluginLoad::Kind::ChannelSlot)
                {
                    auto* host = j.isInputChannel
                                     ? engine.getInputPluginHost (j.globalChannelIdx)
                                     : engine.getPluginHost (j.globalChannelIdx);
                    if (host != nullptr)
                    {
                        host->setPluginAt (j.slotIdx, std::move (instance), statePtr);
                        host->setBypassedAt (j.slotIdx, j.bypassed);
                        matrixView.updateFxButtonAppearance (j.isInputChannel,
                            j.globalChannelIdx);
                    }
                }
                else // GroupSlot
                {
                    auto* g = j.isInputGroup
                                  ? engine.getInputGroupManager().getGroup (j.groupIdx)
                                  : engine.getGroupManager().getGroup (j.groupIdx);
                    if (g != nullptr
                        && j.slotIdx >= 0
                        && j.slotIdx < (int) g->pluginSlots.size())
                    {
                        auto& host = g->pluginSlots[(size_t) j.slotIdx];
                        if (host)
                        {
                            host->setPlugin (std::move (instance), j.channelSet, statePtr);
                            host->setBypassed (j.bypassed);
                            if (j.isInputGroup)
                                inputGroupPanel.rebuild();
                            else
                                groupPanel.rebuild();
                        }
                    }
                }
            }

            juce::Logger::writeToLog ("PluginRestore: [" + juce::String (cursor + 1) + "/"
                                      + juce::String (pluginLoadQueue.size())
                                      + "] '" + pluginName + "' done in "
                                      + juce::String ((uint32_t) juce::Time::getMillisecondCounter()
                                                      - jobStart)
                                      + " ms");

            ++pluginLoadCursor;

            // YIELD before kicking the next one -- this is the whole point of
            // the queue.  callAsync goes to the back of the message queue, so
            // any pending paint/mouse/meter events run before the next plugin
            // instantiation starts.  Without this the UI stays at ~1 fps for
            // the entire duration of the queue drain.
            juce::MessageManager::callAsync ([this, alive] {
                if (!alive->load (std::memory_order_acquire))
                    return;
                processNextPluginLoad();
            });
        });
    }

    MainComponent::MatrixStateByName MainComponent::captureMatrixByName() const
    {
        MatrixStateByName s;
        const auto& info = engine.getDeviceInfo();
        const auto& m = engine.getRoutingMatrix();

        std::vector<ChannelKey> inKeys, outKeys;
        for (auto& d : info)
        {
            for (int c = 0; c < d.numInputChannels; ++c)
                inKeys.push_back ({ d.name, c });
            for (int c = 0; c < d.numOutputChannels; ++c)
                outKeys.push_back ({ d.name, c });
        }

        for (int n = 0; n < (int) inKeys.size(); ++n)
        {
            s.inputs.push_back ({ inKeys[(size_t) n],
                m.getInputTrim (n),
                m.getInputMute (n),
                m.getInputSolo (n) });
        }
        for (int o = 0; o < (int) outKeys.size(); ++o)
        {
            s.outputs.push_back ({ outKeys[(size_t) o],
                m.getOutputTrim (o),
                m.getOutputMute (o) });
        }
        for (int o = 0; o < (int) outKeys.size(); ++o)
            for (int i = 0; i < (int) inKeys.size(); ++i)
            {
                const float g = m.getCrosspoint (o, i);
                if (g > 1.0e-6f)
                    s.crosspoints.push_back ({ inKeys[(size_t) i], outKeys[(size_t) o], g });
            }
        return s;
    }

    void MainComponent::restoreMatrixByName (const MatrixStateByName& s)
    {
        auto& m = engine.getRoutingMatrix();
        const auto& info = engine.getDeviceInfo();

        auto findInputIdx = [&] (const ChannelKey& key) -> int {
            int idx = 0;
            for (auto& d : info)
            {
                if (d.name == key.dev)
                {
                    if (key.ch < d.numInputChannels)
                        return idx + key.ch;
                    return -1;
                }
                idx += d.numInputChannels;
            }
            return -1;
        };
        auto findOutputIdx = [&] (const ChannelKey& key) -> int {
            int idx = 0;
            for (auto& d : info)
            {
                if (d.name == key.dev)
                {
                    if (key.ch < d.numOutputChannels)
                        return idx + key.ch;
                    return -1;
                }
                idx += d.numOutputChannels;
            }
            return -1;
        };

        for (const auto& e : s.inputs)
        {
            int idx = findInputIdx (e.k);
            if (idx >= 0)
            {
                m.setInputTrim (idx, e.trim);
                m.setInputMute (idx, e.mute);
                m.setInputSolo (idx, e.solo);
            }
        }
        for (const auto& e : s.outputs)
        {
            int idx = findOutputIdx (e.k);
            if (idx >= 0)
            {
                m.setOutputTrim (idx, e.trim);
                m.setOutputMute (idx, e.mute);
            }
        }
        for (const auto& xp : s.crosspoints)
        {
            int oi = findOutputIdx (xp.dst);
            int ii = findInputIdx (xp.src);
            if (oi >= 0 && ii >= 0)
                m.setCrosspoint (oi, ii, xp.gain);
        }
    }

    void MainComponent::saveSnapshotInteractive()
    {
        auto dir = SnapshotStore::getDirectory();
        activeChooser = std::make_unique<juce::FileChooser> (
            "Save snapshot",
            dir.getChildFile ("snapshot.xml"),
            "*.xml");

        activeChooser->launchAsync (
            juce::FileBrowserComponent::saveMode
                | juce::FileBrowserComponent::canSelectFiles
                | juce::FileBrowserComponent::warnAboutOverwriting,
            [this] (const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File {})
                    return;
                if (file.getFileExtension().isEmpty())
                    file = file.withFileExtension ("xml");
                SnapshotStore::save (file, gatherCurrentSnapshot());
            });
    }

    void MainComponent::loadSnapshotInteractive()
    {
        auto dir = SnapshotStore::getDirectory();
        activeChooser = std::make_unique<juce::FileChooser> (
            "Load snapshot",
            dir,
            "*.xml");

        activeChooser->launchAsync (
            juce::FileBrowserComponent::openMode
                | juce::FileBrowserComponent::canSelectFiles,
            [this] (const juce::FileChooser& fc) {
                auto file = fc.getResult();
                if (file == juce::File {} || !file.existsAsFile())
                    return;
                Snapshot s;
                const auto res = SnapshotStore::load (file, s);
                if (res == SnapshotStore::LoadResult::Ok)
                    applySnapshot (s);
                else if (res != SnapshotStore::LoadResult::NoFile)
                    juce::NativeMessageBox::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Could not load snapshot",
                        "\"" + file.getFileName() + "\" could not be loaded -- it may be "
                                                    "corrupt or saved by a newer version of D-Router.");
            });
    }

    void MainComponent::switchTab (Tab newTab)
    {
        currentTab = newTab;

        matrixTabBtn.setToggleState (currentTab == RoutingTab, juce::dontSendNotification);
        groupsTabBtn.setToggleState (currentTab == GroupsTab, juce::dontSendNotification);
        audioSetupTabBtn.setToggleState (currentTab == AudioSetupTab, juce::dontSendNotification);
        statusTabBtn.setToggleState (currentTab == StatusTab, juce::dontSendNotification);

        matrixView.setVisible (currentTab == RoutingTab);

        const bool audioSetup = (currentTab == AudioSetupTab);
        inputDeviceVolPanel.setVisible (audioSetup);
        outputDeviceVolPanel.setVisible (audioSetup);
        if (audioSetup)
        {
            // Refresh strips from the current device list every time the tab opens,
            // so device add/remove is reflected even if it happened on another tab.
            inputDeviceVolPanel.rebuild();
            outputDeviceVolPanel.rebuild();
        }

        // Keep detached panels themselves visible (they live in their own windows).
        if (groupHost.isDetached())
            groupPanel.setVisible (true);
        if (inputGroupHost.isDetached())
            inputGroupPanel.setVisible (true);
        if (statusHost.isDetached())
            statusPanel.setVisible (true);

        if (currentTab == GroupsTab)
        {
            groupsPlaceholder.setVisible (groupHost.isDetached());
            inputGroupsPlaceholder.setVisible (inputGroupHost.isDetached());
            if (!groupHost.isDetached())
                groupPanel.setVisible (true);
            if (!inputGroupHost.isDetached())
                inputGroupPanel.setVisible (true);
            if (!statusHost.isDetached())
                statusPanel.setVisible (false);
            statusPlaceholder.setVisible (false);
        }
        else if (currentTab == AudioSetupTab)
        {
            if (!groupHost.isDetached())
                groupPanel.setVisible (false);
            if (!inputGroupHost.isDetached())
                inputGroupPanel.setVisible (false);
            if (!statusHost.isDetached())
                statusPanel.setVisible (false);
            groupsPlaceholder.setVisible (false);
            inputGroupsPlaceholder.setVisible (false);
            statusPlaceholder.setVisible (false);
        }
        else if (currentTab == StatusTab)
        {
            statusPlaceholder.setVisible (statusHost.isDetached());
            if (!statusHost.isDetached())
                statusPanel.setVisible (true);
            if (!groupHost.isDetached())
                groupPanel.setVisible (false);
            if (!inputGroupHost.isDetached())
                inputGroupPanel.setVisible (false);
            groupsPlaceholder.setVisible (false);
            inputGroupsPlaceholder.setVisible (false);
        }
        else // RoutingTab
        {
            if (!groupHost.isDetached())
                groupPanel.setVisible (false);
            if (!inputGroupHost.isDetached())
                inputGroupPanel.setVisible (false);
            if (!statusHost.isDetached())
                statusPanel.setVisible (false);
            groupsPlaceholder.setVisible (false);
            inputGroupsPlaceholder.setVisible (false);
            statusPlaceholder.setVisible (false);
        }

        resized();
        menuItemsChanged(); // View menu's tab tick follows currentTab
    }

} // namespace dcr

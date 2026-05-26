#include "MainComponent.h"

#include "DSP/PluginHost.h"
#include "DSP/MultiChannelPluginHost.h"
#include "DSP/SafePluginOps.h"
#include "Persistence/CrashGuard.h"
#include "Persistence/SettingsStore.h"
#include "Routing/InputGroupManager.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"
#include "UI/DeviceManagerDialog.h"
#include "UI/GroupManagerDialog.h"
#include "UI/LogViewerDialog.h"
#include "UI/SettingsDialog.h"

namespace dcr {

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

    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    // App brand label is always white -- not theme-tinted -- so the brand
    // reads cleanly against any accent colour the user picks.
    title.setColour (juce::Label::textColourId, juce::Colours::white);
    title.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (title);

    devicesButton .onClick = [this] { openDeviceDialog(); };
    settingsButton.onClick = [this]
    {
        SettingsDialog::launch (engine.getSettings(),
            [this] (std::optional<EngineSettings> s, bool persistToDisk)
            {
                if (! s.has_value()) return;        // Cancel
                const bool needsRestart = ! s->audioPathEquals (engine.getSettings());

                if (needsRestart && ! currentSpecs.empty())
                {
                    // Engine restart needed: applyDeviceSelection() will tear
                    // down workers + matrix-processor and rebuild on a worker
                    // thread, then resume meter / status timers from its own
                    // callAsync().  We MUST NOT touch any engine-reading UI
                    // timer here, or it will fire mid-rebuild and read a
                    // moved-from inputPeaks / outputPeaks vector -> crash.
                    engine.setSettings (*s);
                    if (persistToDisk) SettingsStore::save (*s);
                    applyDeviceSelection (currentSpecs);
                }
                else
                {
                    // UI-only or no-active-device change.  Safe to refresh
                    // timers immediately since the audio thread isn't being
                    // torn down.
                    engine.setSettings (*s);
                    if (persistToDisk) SettingsStore::save (*s);
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
    saveButton    .onClick = [this] { saveSnapshotInteractive(); };
    loadButton    .onClick = [this] { loadSnapshotInteractive(); };
    stopButton    .onClick = [this] { if (inPanic) panicRelease(); else panicActivate(); };
    stopButton    .setTooltip ("Mute every input and output.  Click again to restore the prior state.");
    updatePanicButtonAppearance();
    groupsButton.onClick = [this]
    {
        // The dialog lets the user remove groups, which immediately destroys
        // their MultiChannelPluginHosts (and the AudioPluginInstances inside).
        // Close any open group plugin editors BEFORE the dialog can do that
        // -- otherwise the Card dtor later runs editor.reset() against a
        // dead AudioProcessor and segfaults.
        groupPanel     .closeAllPluginEditors();
        inputGroupPanel.closeAllPluginEditors();

        // Single dialog with an internal Inputs / Outputs toggle.  After
        // closing we rebuild BOTH panels since either side may have changed.
        GroupManagerDialog::launch (engine,
            [this]
            {
                groupPanel     .rebuild();
                inputGroupPanel.rebuild();
            },
            GroupManagerDialog::Direction::Outputs);
    };
    addAndMakeVisible (devicesButton);
    addAndMakeVisible (settingsButton);
    addAndMakeVisible (groupsButton);
    
    addAndMakeVisible (saveButton);
    addAndMakeVisible (loadButton);
    addAndMakeVisible (logsButton);
    addAndMakeVisible (stopButton);

    logsButton.onClick = [] { LogViewerDialog::launch(); };
    logsButton.setTooltip ("Open in-app diagnostics log.  Contents are also flushed to "
                           "~/Library/Logs/D-Router/ on every line so a crash leaves the "
                           "lead-up context recoverable.");

    // Group buttons for radio toggling
    matrixTabBtn.setRadioGroupId (100);
    groupsTabBtn.setRadioGroupId (100);
    statusTabBtn.setRadioGroupId (100);

    matrixTabBtn.setClickingTogglesState (true);
    groupsTabBtn.setClickingTogglesState (true);
    statusTabBtn.setClickingTogglesState (true);

    matrixTabBtn.setToggleState (true, juce::dontSendNotification);

    matrixTabBtn.onClick = [this] { switchTab (RoutingTab); };
    groupsTabBtn.onClick = [this] { switchTab (GroupsTab); };
    statusTabBtn.onClick = [this] { switchTab (StatusTab); };

    addAndMakeVisible (matrixTabBtn);
    addAndMakeVisible (groupsTabBtn);
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
    groupPanel.onPopOutRequested = [this] { toggleGroupPanelDetach(); };
    groupPanel.onGroupHover = [this] (const std::vector<int>& outs)
    {
        matrixView.setHighlightedOutputs (outs);
    };

    // Input Group Panel setup
    addChildComponent (inputGroupPanel);
    inputGroupPanel.onPopOutRequested = [this] { toggleInputGroupPanelDetach(); };
    inputGroupPanel.onGroupHover = [this] (const std::vector<int>& ins)
    {
        matrixView.setHighlightedInputs (ins);
    };

    inputGroupsPlaceholder.setText ("INPUT GROUPS DETACHED\n\nPanel is floating in an external window.",
                                    juce::dontSendNotification);
    inputGroupsPlaceholder.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                       13.0f, juce::Font::bold));
    inputGroupsPlaceholder.setJustificationType (juce::Justification::centred);
    inputGroupsPlaceholder.setColour (juce::Label::textColourId, juce::Colour::fromRGB (160, 160, 165));
    addChildComponent (inputGroupsPlaceholder);

    addChildComponent (matrixView);

    // When the user clicks any per-channel mute button while a panic is
    // active, discard the saved pre-panic snapshot so the next panic press
    // mutes everything from scratch (matches the user's spec: "panic forgets
    // the prior state once you manually touch a mute").
    matrixView.onUserMuteChanged = [this]
    {
        if (! inPanic) return;
        inPanic = false;
        savedInputMutes.clear();
        savedOutputMutes.clear();
        updatePanicButtonAppearance();
    };

    // Status Panel setup
    addChildComponent (statusPanel);
    statusPanel.onPopOutRequested = [this] { toggleStatusPanelDetach(); };

    // Load persistent settings (engine SR, ring sizes, SRC quality, theme).
    engine.setSettings (SettingsStore::load());
    customLookAndFeel.applyTheme (engine.getSettings());
    // App brand label is always white -- not theme-tinted -- so the brand
    // reads cleanly against any accent colour the user picks.
    title.setColour (juce::Label::textColourId, juce::Colours::white);

    // React to hotplug: if a routed device disappears, gracefully drop it.
    engine.onDeviceListChanged = [this]
    {
        if (currentSpecs.empty()) return;
        auto ins  = engine.getAvailableInputDevices();
        auto outs = engine.getAvailableOutputDevices();
        std::vector<AudioEngine::DeviceSpec> filtered;
        for (auto& sp : currentSpecs)
        {
            const bool stillIn  = sp.wantInput  && ins .contains (sp.name);
            const bool stillOut = sp.wantOutput && outs.contains (sp.name);
            if (stillIn || stillOut)
                filtered.push_back ({ sp.name, stillIn, stillOut });
        }
        // Restart engine with the surviving specs so it picks up the change.
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
        juce::MessageManager::callAsync ([this]
        {
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
                [this] (int result)
                {
                    if (result == 1)   // first button => Restore
                    {
                        Snapshot s;
                        if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s))
                            applySnapshot (s);
                    }
                    // "Start blank" leaves the engine with no devices selected;
                    // the user's snapshot stays on disk so they can still
                    // hand-load it via the Load... button if they change their
                    // mind later.
                });
        });
    }
    else
    {
        // Normal clean-exit path: auto-restore last session immediately.
        Snapshot s;
        if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s))
            applySnapshot (s);
    }

    refreshStatus();
    switchTab (RoutingTab);
    startTimer (engine.getSettings().statusTimerMs);
}

void MainComponent::timerCallback() { refreshStatus(); }

MainComponent::~MainComponent()
{
    // Detach GPU context before the component tree starts unwinding.
    if (openGLContext.isAttached()) openGLContext.detach();

    setLookAndFeel (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);

    if (reconfigThread.joinable()) reconfigThread.join();

    // Flip the alive sentinel FIRST so any in-flight async plugin-restore
    // callbacks that haven't fired yet bail at their entry check instead of
    // touching us / engine / matrixView through dangling references.
    aliveToken->store (false, std::memory_order_release);

    // CRITICAL ORDER: stop the audio engine BEFORE harvesting the snapshot.
    // gatherCurrentSnapshot() reads every PluginHost's getStateInformation()
    // -- a non-RT API that mutates plugin internals -- and the matrix audio
    // thread is meanwhile calling processBlock() on the same instances.
    // Concurrent getStateInformation + processBlock is undefined per JUCE
    // contract; we have seen plugins corrupt their parameter caches and
    // crash here.  Stop first, then read state safely.
    engine.stop();
    SnapshotStore::save (SnapshotStore::getLastUsedFile(), gatherCurrentSnapshot());
    // Only mark the clean-exit marker AFTER the snapshot write has been
    // requested; if save() throws or the process is killed before this
    // line, the marker stays on disk and next launch will prompt the user.
    CrashGuard::markCleanExit();
}

void MainComponent::parentHierarchyChanged()
{
    // OpenGL deliberately NOT attached.  GL-using plugin editors (analyzer /
    // metering plugins like Youlean Loudness Meter, FabFilter visualisers,
    // SPAN) want to set up their own NSOpenGLContext.  When our main window
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

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto top = r.removeFromTop (32);
    
    // Left Configuration Section
    title.setBounds (top.removeFromLeft (240));   // wide enough for "ZDAudio D-Router" @ 22pt bold
    top.removeFromLeft (10);
    devicesButton.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (4);
    settingsButton.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (4);
    groupsButton  .setBounds (top.removeFromLeft (90));

    // Right Session Section (Save, Load, Logs, PANIC)
    stopButton.setBounds (top.removeFromRight (60));
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
        fb.flexDirection  = juce::FlexBox::Direction::row;
        fb.justifyContent = juce::FlexBox::JustifyContent::center;
        fb.alignContent   = juce::FlexBox::AlignContent::stretch;
        fb.items.add (juce::FlexItem (matrixTabBtn).withFlex (1.0f).withMinWidth (90.0f)
                                                    .withMaxWidth (220.0f).withMargin (juce::FlexItem::Margin (0, 2, 0, 0)));
        fb.items.add (juce::FlexItem (groupsTabBtn).withFlex (1.0f).withMinWidth (90.0f)
                                                    .withMaxWidth (220.0f).withMargin (juce::FlexItem::Margin (0, 2, 0, 2)));
        fb.items.add (juce::FlexItem (statusTabBtn).withFlex (1.0f).withMinWidth (90.0f)
                                                    .withMaxWidth (220.0f).withMargin (juce::FlexItem::Margin (0, 0, 0, 2)));
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

        if (inputGroupPanelDetached) inputGroupsPlaceholder.setBounds (topHalf);
        else                          inputGroupPanel.setBounds (topHalf);

        if (groupPanelDetached) groupsPlaceholder.setBounds (bottomHalf);
        else                    groupPanel.setBounds (bottomHalf);
    }
    else if (currentTab == StatusTab)
    {
        if (statusPanelDetached)
            statusPlaceholder.setBounds (r);
        else
            statusPanel.setBounds (r);
    }
}

namespace
{
    class GroupFloatingWindow : public juce::DocumentWindow
    {
    public:
        GroupFloatingWindow (std::function<void()> onClose)
            : DocumentWindow ("Output groups",
                              juce::Colour::fromRGB (28, 28, 32),
                              DocumentWindow::closeButton),
              closeFn (std::move (onClose))
        {
            setUsingNativeTitleBar (true);
            setResizable (true, false);
        }
        void closeButtonPressed() override { if (closeFn) closeFn(); }
    private:
        std::function<void()> closeFn;
    };
}

void MainComponent::toggleStatusPanelDetach()
{
    statusPanelDetached = ! statusPanelDetached;

    if (statusPanelDetached)
    {
        removeChildComponent (&statusPanel);
        statusWindow.reset (new GroupFloatingWindow ([this]
        {
            if (statusPanelDetached) toggleStatusPanelDetach();
        }));
        statusWindow->setName ("Engine status");
        statusWindow->setContentNonOwned (&statusPanel, false);
        statusWindow->centreWithSize (600, 280);
        statusWindow->setVisible (true);
        statusPanel.setVisible (true);
        statusPanel.setDetached (true);
    }
    else
    {
        if (statusWindow)
        {
            statusWindow->clearContentComponent();
            statusWindow.reset();
        }
        addAndMakeVisible (statusPanel);
        statusPanel.setDetached (false);
    }
    switchTab (currentTab);
}

void MainComponent::toggleInputGroupPanelDetach()
{
    inputGroupPanelDetached = ! inputGroupPanelDetached;

    if (inputGroupPanelDetached)
    {
        removeChildComponent (&inputGroupPanel);
        inputGroupWindow.reset (new GroupFloatingWindow ([this]
        {
            if (inputGroupPanelDetached) toggleInputGroupPanelDetach();
        }));
        inputGroupWindow->setName ("Input groups");
        inputGroupWindow->setContentNonOwned (&inputGroupPanel, false);
        inputGroupWindow->centreWithSize (juce::jmax (820, cards_default_width()), 240);
        inputGroupWindow->setVisible (true);
        inputGroupPanel.setVisible (true);
        inputGroupPanel.setDetached (true);
    }
    else
    {
        if (inputGroupWindow)
        {
            inputGroupWindow->clearContentComponent();
            inputGroupWindow.reset();
        }
        addAndMakeVisible (inputGroupPanel);
        inputGroupPanel.setDetached (false);
    }
    switchTab (currentTab);
}

void MainComponent::toggleGroupPanelDetach()
{
    groupPanelDetached = ! groupPanelDetached;

    if (groupPanelDetached)
    {
        // Move panel out into a separate window.
        removeChildComponent (&groupPanel);
        groupWindow.reset (new GroupFloatingWindow ([this]
        {
            // Window closed -> re-embed.
            if (groupPanelDetached) toggleGroupPanelDetach();
        }));
        groupWindow->setContentNonOwned (&groupPanel, false);
        groupWindow->centreWithSize (juce::jmax (820, cards_default_width()),
                                     juce::jmax (240, 240));
        groupWindow->setVisible (true);
        groupPanel.setVisible (true);
        groupPanel.setDetached (true);
    }
    else
    {
        if (groupWindow)
        {
            groupWindow->clearContentComponent();
            groupWindow.reset();
        }
        addAndMakeVisible (groupPanel);
        groupPanel.setDetached (false);
    }
    switchTab (currentTab);
}

int MainComponent::cards_default_width() { return 800; }

void MainComponent::refreshStatus()
{
    statusPanel.refreshNow();
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
    auto& m = engine.getRoutingMatrix();
    const int nIn  = m.getNumInputs();
    const int nOut = m.getNumOutputs();
    if (nIn == 0 && nOut == 0) return;

    savedInputMutes .assign ((size_t) nIn,  0);
    savedOutputMutes.assign ((size_t) nOut, 0);
    for (int n = 0; n < nIn;  ++n)
    {
        savedInputMutes [(size_t) n] = m.getInputMute (n) ? 1 : 0;
        m.setInputMute  (n, true);
    }
    for (int o = 0; o < nOut; ++o)
    {
        savedOutputMutes[(size_t) o] = m.getOutputMute (o) ? 1 : 0;
        m.setOutputMute (o, true);
    }
    inPanic = true;
    matrixView.refreshMuteButtonStates();
    updatePanicButtonAppearance();
}

void MainComponent::panicRelease()
{
    auto& m = engine.getRoutingMatrix();
    const int nIn  = juce::jmin ((int) savedInputMutes .size(), m.getNumInputs());
    const int nOut = juce::jmin ((int) savedOutputMutes.size(), m.getNumOutputs());
    for (int n = 0; n < nIn;  ++n) m.setInputMute  (n, savedInputMutes [(size_t) n] != 0);
    for (int o = 0; o < nOut; ++o) m.setOutputMute (o, savedOutputMutes[(size_t) o] != 0);
    savedInputMutes .clear();
    savedOutputMutes.clear();
    inPanic = false;
    matrixView.refreshMuteButtonStates();
    updatePanicButtonAppearance();
}

void MainComponent::updatePanicButtonAppearance()
{
    // Bright red when active so the user always knows panic is engaged.
    stopButton.setButtonText (inPanic ? "PANIC*" : "PANIC");
    stopButton.setColour (juce::TextButton::buttonColourId,
                          inPanic ? juce::Colour::fromRGB (180, 30, 30)
                                  : juce::Colour::fromRGB (50, 50, 56));
    stopButton.setColour (juce::TextButton::buttonOnColourId,
                          inPanic ? juce::Colour::fromRGB (180, 30, 30)
                                  : juce::Colour::fromRGB (50, 50, 56));
    stopButton.repaint();
}

void MainComponent::openDeviceDialog()
{
    DeviceManagerDialog::launch (engine, currentSpecs,
        [this] (std::optional<std::vector<AudioEngine::DeviceSpec>> sel)
        {
            if (! sel.has_value()) return;
            applyDeviceSelection (std::move (*sel));
        });
}

void MainComponent::applyDeviceSelection (std::vector<AudioEngine::DeviceSpec> newSpecs)
{
    if (isReconfiguring.exchange (true))
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
    if (inPanic || ! savedInputMutes.empty() || ! savedOutputMutes.empty())
    {
        inPanic = false;
        savedInputMutes.clear();
        savedOutputMutes.clear();
        updatePanicButtonAppearance();
    }

    // Close any open per-channel plugin editors NOW (message thread) before
    // the worker tears down PluginHosts.  Skipping this lets ~PluginEditor
    // call editorBeingDeleted() on a dead AudioPluginInstance -> segfault.
    matrixView.closeAllPluginEditors();

    // Capture current state on message thread (cheap), then offload the slow
    // CoreAudio open/close to a worker thread.
    auto preserved = captureMatrixByName();

    devicesButton.setEnabled (false);
    settingsButton.setEnabled (false);
    groupsButton.setEnabled (false);
    saveButton   .setEnabled (false);
    loadButton   .setEnabled (false);
    stopButton   .setEnabled (false);
    // Status panel will pick this up on its next refresh tick.
    // Freeze EVERY UI-thread reader of engine state -- not just matrixView --
    // for the duration of the reconfigure.  Otherwise StatusPanel /
    // OutputGroupPanel timers can fire during processor.configure()'s
    // inputPeaks / outputPeaks move-assignment and hit a moved-from vector.
    matrixView.pauseUpdates();
    statusPanel.pauseUpdates();
    groupPanel.pauseUpdates();
    inputGroupPanel.pauseUpdates();
    stopTimer();

    // Disable INPUT to the matrix grid + group cards too, not just the timers.
    // While engine.start() runs matrix.resize() on the worker thread, any UI
    // mouse click that calls matrix.setCrosspoint / setInputTrim / set*Mute
    // races with the non-atomic vector move and can UAF.  setEnabled(false)
    // propagates down the children and stops mouse events at the root.
    matrixView.setEnabled (false);
    groupPanel.setEnabled (false);
    inputGroupPanel.setEnabled (false);

    if (reconfigThread.joinable()) reconfigThread.join();

    auto specs = std::move (newSpecs);
    reconfigThread = std::thread ([this, specs, preserved]
    {
        // Graceful fade-out before tearing the engine down.  Setting all
        // output trims to 0 makes the smoothing in MatrixProcessor ramp
        // every active route to silence; we then sleep long enough (~5*tau)
        // for the ramp to complete before stopping devices.
        {
            const int smoothMs = juce::jmax (5, engine.getSettings().gainSmoothingMs);
            auto& mtx = engine.getRoutingMatrix();
            const int nOut = mtx.getNumOutputs();
            for (int o = 0; o < nOut; ++o) mtx.setOutputTrim (o, 0.0f);
            std::this_thread::sleep_for (std::chrono::milliseconds (smoothMs * 5));
        }

        engine.stop();
        const bool started = specs.empty() ? true : engine.start (specs);

        juce::MessageManager::callAsync ([this, specs, preserved, started]
        {
            currentSpecs = specs;
            if (! specs.empty() && ! started)
                {}   // failure is visible via empty matrix + status panel
            else
                restoreMatrixByName (preserved);

            // Snapshot apply (cold-start or Load...): drain pendingSnap NOW,
            // after engine.start() resized the matrix to the new (in, out)
            // size.  If we did this earlier the resize would have zeroed
            // our writes.
            if (pendingSnap.valid)
            {
                auto& m = engine.getRoutingMatrix();
                const int nIn  = m.getNumInputs();
                const int nOut = m.getNumOutputs();
                for (size_t n = 0; n < pendingSnap.inputTrim.size()  && (int) n < nIn;  ++n)
                    m.setInputTrim  ((int) n, pendingSnap.inputTrim [n]);
                for (size_t n = 0; n < pendingSnap.outputTrim.size() && (int) n < nOut; ++n)
                    m.setOutputTrim ((int) n, pendingSnap.outputTrim[n]);
                for (size_t n = 0; n < pendingSnap.inputMute.size()  && (int) n < nIn;  ++n)
                    m.setInputMute  ((int) n, pendingSnap.inputMute [n] != 0);
                for (size_t n = 0; n < pendingSnap.outputMute.size() && (int) n < nOut; ++n)
                    m.setOutputMute ((int) n, pendingSnap.outputMute[n] != 0);
                for (size_t n = 0; n < pendingSnap.inputSolo.size()  && (int) n < nIn;  ++n)
                    m.setInputSolo  ((int) n, pendingSnap.inputSolo [n] != 0);
                for (const auto& xp : pendingSnap.crosspoints)
                    if (xp.outputCh < nOut && xp.inputCh < nIn)
                        m.setCrosspoint (xp.outputCh, xp.inputCh, xp.gain);

                // Plugin instantiation is async; fire it off and let each
                // callback land back here over the next few hundred ms.
                restorePluginChainsAsync();

                pendingSnap = {};
            }

            matrixView.rebuildFromEngine();
            matrixView.resumeUpdates();
            groupPanel.rebuild();
            inputGroupPanel.rebuild();
            // Resume the panels that were paused at the top of this call.
            statusPanel.resumeUpdates();
            groupPanel.resumeUpdates();
            inputGroupPanel.resumeUpdates();
            // Re-enable matrix input now that engine is back in a stable state.
            matrixView.setEnabled (true);
            groupPanel.setEnabled (true);
            inputGroupPanel.setEnabled (true);
            startTimer (engine.getSettings().statusTimerMs);
            refreshStatus();

            devicesButton.setEnabled (true);
            settingsButton.setEnabled (true);
            groupsButton.setEnabled (true);
            saveButton   .setEnabled (true);
            loadButton   .setEnabled (true);
            stopButton   .setEnabled (true);
            isReconfiguring = false;
        });
    });
}

namespace
{
    struct LayoutMap { const char* name; juce::AudioChannelSet set; };
    const std::vector<LayoutMap>& layoutTable()
    {
        static const std::vector<LayoutMap> t = {
            { "Mono",   juce::AudioChannelSet::mono() },
            { "Stereo", juce::AudioChannelSet::stereo() },
            { "Quad",   juce::AudioChannelSet::quadraphonic() },
            { "5.1",    juce::AudioChannelSet::create5point1() },
            { "7.1",    juce::AudioChannelSet::create7point1() },
            { "7.1.2",  juce::AudioChannelSet::create7point1point2() },
            { "7.1.4",  juce::AudioChannelSet::create7point1point4() },
        };
        return t;
    }
    juce::String layoutName (const juce::AudioChannelSet& s)
    {
        for (auto& e : layoutTable()) if (e.set == s) return e.name;
        return "Stereo";
    }
    juce::AudioChannelSet layoutFromName (const juce::String& n)
    {
        for (auto& e : layoutTable()) if (n == e.name) return e.set;
        return juce::AudioChannelSet::stereo();
    }
}

Snapshot MainComponent::gatherCurrentSnapshot() const
{
    Snapshot s;
    s.engineSampleRate = engine.getEngineSampleRate();
    s.engineBlockSize  = engine.getEngineBlockSize();
    s.devices          = currentSpecs;

    auto gatherFromManager = [&] (auto& mgr, std::vector<Snapshot::Group>& dest)
    {
        for (int gi = 0; gi < mgr.getNumGroups(); ++gi)
        {
            const auto* g = mgr.getGroup (gi);
            if (g == nullptr) continue;
            Snapshot::Group gs;
            gs.name           = g->name;
            gs.layoutName     = layoutName (g->channelSet);
            gs.memberChannels = g->memberChannels;
            gs.faderDb        = g->faderDb.load();
            gs.muted          = g->muted.load();
            dest.push_back (std::move (gs));
        }
    };
    gatherFromManager (engine.getGroupManager(),      s.outputGroups);
    gatherFromManager (engine.getInputGroupManager(), s.inputGroups);

    const auto& m = engine.getRoutingMatrix();
    const int nIn  = m.getNumInputs();
    const int nOut = m.getNumOutputs();
    s.inputTrim.resize ((size_t) nIn);
    s.outputTrim.resize ((size_t) nOut);
    s.inputMute.resize ((size_t) nIn,  0);
    s.outputMute.resize ((size_t) nOut, 0);
    s.inputSolo.resize ((size_t) nIn,  0);
    for (int n = 0; n < nIn;  ++n)
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
    auto gatherSlot = [] (juce::AudioPluginInstance* p, bool bypassed) -> Snapshot::PluginSlotState
    {
        Snapshot::PluginSlotState ps;
        if (p == nullptr) return ps;
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
    };

    for (int ch = 0; ch < nIn; ++ch)
    {
        auto* host = const_cast<AudioEngine&> (engine).getInputPluginHost (ch);
        if (host == nullptr) continue;
        Snapshot::ChannelChain cc;
        cc.globalIdx = ch;
        cc.isInput   = true;
        cc.slots.resize ((size_t) PluginHost::kNumSlots);
        bool any = false;
        for (int sl = 0; sl < PluginHost::kNumSlots; ++sl)
        {
            cc.slots[(size_t) sl] = gatherSlot (host->getPluginAt (sl), host->isBypassedAt (sl));
            if (! cc.slots[(size_t) sl].isEmpty()) any = true;
        }
        if (any) s.channelChains.push_back (std::move (cc));
    }
    for (int ch = 0; ch < nOut; ++ch)
    {
        auto* host = const_cast<AudioEngine&> (engine).getPluginHost (ch);
        if (host == nullptr) continue;
        Snapshot::ChannelChain cc;
        cc.globalIdx = ch;
        cc.isInput   = false;
        cc.slots.resize ((size_t) PluginHost::kNumSlots);
        bool any = false;
        for (int sl = 0; sl < PluginHost::kNumSlots; ++sl)
        {
            cc.slots[(size_t) sl] = gatherSlot (host->getPluginAt (sl), host->isBypassedAt (sl));
            if (! cc.slots[(size_t) sl].isEmpty()) any = true;
        }
        if (any) s.channelChains.push_back (std::move (cc));
    }

    // ===== Plugin chains: per-group ==========================================
    auto gatherGroupChains = [&] (auto& mgr, bool isInput, std::vector<Snapshot::GroupChain>& out)
    {
        for (int gi = 0; gi < mgr.getNumGroups(); ++gi)
        {
            auto* g = mgr.getGroup (gi);
            if (g == nullptr) continue;
            Snapshot::GroupChain gc;
            gc.groupIdx = gi;
            gc.isInput  = isInput;
            gc.slots.resize (g->pluginSlots.size());
            bool any = false;
            for (size_t sl = 0; sl < g->pluginSlots.size(); ++sl)
            {
                if (auto& host = g->pluginSlots[sl])
                {
                    gc.slots[sl] = gatherSlot (host->getPlugin(), host->isBypassed());
                    if (! gc.slots[sl].isEmpty()) any = true;
                }
            }
            if (any) out.push_back (std::move (gc));
        }
    };
    gatherGroupChains (const_cast<AudioEngine&> (engine).getGroupManager(),      false, s.groupChains);
    gatherGroupChains (const_cast<AudioEngine&> (engine).getInputGroupManager(), true,  s.groupChains);

    return s;
}

void MainComponent::applySnapshot (const Snapshot& s)
{
    // Snapshot's SR/block override the persisted settings.  Other tunables
    // (ring sizes, SRC, timers) keep the user's settings.
    auto newSettings = engine.getSettings();
    newSettings.engineSampleRate = s.engineSampleRate;
    newSettings.engineBlockSize  = s.engineBlockSize;
    engine.setSettings (newSettings);

    // Group plugin editors reference AudioPluginInstance objects owned by the
    // groups about to be wiped.  Close them on the message thread NOW so the
    // editor dtor doesn't run against a dead processor later (segfault).
    groupPanel     .closeAllPluginEditors();
    inputGroupPanel.closeAllPluginEditors();

    // Wipe and re-create groups from the snapshot BEFORE restarting the
    // engine -- the engine restart will then prepare their plugin hosts.
    auto restoreToManager = [&] (auto& mgr, const std::vector<Snapshot::Group>& src)
    {
        while (mgr.getNumGroups() > 0) mgr.removeGroup (0);
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
                g->muted  .store (gs.muted);
            }
        }
    };
    restoreToManager (engine.getGroupManager(),      s.outputGroups);
    restoreToManager (engine.getInputGroupManager(), s.inputGroups);

    // Cannot write the matrix here -- engine.start() is about to run on the
    // reconfig worker thread and will resize the matrix to (totalIns,
    // totalOuts), zeroing everything.  Stash the values; the reconfig
    // callAsync drains pendingSnap AFTER the new matrix is live.
    pendingSnap.inputTrim    = s.inputTrim;
    pendingSnap.outputTrim   = s.outputTrim;
    pendingSnap.inputMute    = s.inputMute;
    pendingSnap.outputMute   = s.outputMute;
    pendingSnap.inputSolo    = s.inputSolo;
    pendingSnap.crosspoints  = s.crosspoints;
    pendingSnap.channelChains = s.channelChains;
    pendingSnap.groupChains   = s.groupChains;
    pendingSnap.valid = true;

    applyDeviceSelection (s.devices);
    // Don't touch the matrix or rebuildFromEngine() here.  applyDeviceSelection
    // does both, then drains pendingSnap, then rebuilds the UI.
}

void MainComponent::restorePluginChainsAsync()
{
    if (pendingSnap.channelChains.empty() && pendingSnap.groupChains.empty())
        return;

    auto& fmt = engine.getPluginFormatManager();
    const double sr = engine.getEngineSampleRate();
    const int    bs = engine.getEngineBlockSize();

    // ----- per-channel chains -----------------------------------------------
    for (auto& ch : pendingSnap.channelChains)
    {
        for (int slotIdx = 0; slotIdx < (int) ch.slots.size(); ++slotIdx)
        {
            const auto& ps = ch.slots[(size_t) slotIdx];
            if (ps.isEmpty()) continue;

            juce::PluginDescription desc;
            if (! dcr::safe::loadPluginDescriptionFromXml (ps.descriptionXml, desc)) continue;

            // Decode plugin's own state once on the message thread; the captured
            // copy is then moved into the async callback below.
            juce::MemoryBlock state;
            if (ps.stateB64.isNotEmpty())
            {
                juce::MemoryOutputStream mos (state, false);
                juce::Base64::convertFromBase64 (mos, ps.stateB64);
            }
            const bool wasBypassed = ps.bypassed;
            const bool isInput     = ch.isInput;
            const int  globalIdx   = ch.globalIdx;

            auto alive = aliveToken;
            fmt.createPluginInstanceAsync (desc, sr, bs,
                [this, alive, isInput, globalIdx, slotIdx, state, wasBypassed]
                (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err)
                {
                    juce::ignoreUnused (err);
                    // MainComponent (and therefore engine + matrixView) may
                    // already be gone if the user quit or kicked off another
                    // reconfigure while this load was still pending.
                    if (! alive->load (std::memory_order_acquire)) return;
                    if (instance == nullptr) return;
                    if (state.getSize() > 0)
                        dcr::safe::setStateInformation (*instance,
                                                        state.getData(),
                                                        (int) state.getSize());
                    auto* host = isInput ? engine.getInputPluginHost (globalIdx)
                                         : engine.getPluginHost     (globalIdx);
                    if (host == nullptr) return;
                    host->setPluginAt (slotIdx, std::move (instance));
                    host->setBypassedAt (slotIdx, wasBypassed);
                    matrixView.updateFxButtonAppearance (isInput, globalIdx);
                });
        }
    }

    // ----- per-group chains -------------------------------------------------
    // OutputGroupManager and InputGroupManager are sibling classes (no shared
    // base), so the lookup is duplicated rather than dispatched via a cast.
    auto getGroupForChain = [this] (bool isInput, int groupIdx) -> OutputGroup*
    {
        return isInput ? engine.getInputGroupManager().getGroup (groupIdx)
                       : engine.getGroupManager()     .getGroup (groupIdx);
    };

    for (auto& gc : pendingSnap.groupChains)
    {
        auto* g = getGroupForChain (gc.isInput, gc.groupIdx);
        if (g == nullptr) continue;
        const auto channelSet = g->channelSet;
        for (int slotIdx = 0; slotIdx < (int) gc.slots.size(); ++slotIdx)
        {
            const auto& ps = gc.slots[(size_t) slotIdx];
            if (ps.isEmpty()) continue;

            juce::PluginDescription desc;
            if (! dcr::safe::loadPluginDescriptionFromXml (ps.descriptionXml, desc)) continue;

            juce::MemoryBlock state;
            if (ps.stateB64.isNotEmpty())
            {
                juce::MemoryOutputStream mos (state, false);
                juce::Base64::convertFromBase64 (mos, ps.stateB64);
            }
            const bool wasBypassed = ps.bypassed;
            const bool isInput     = gc.isInput;
            const int  groupIdx    = gc.groupIdx;

            auto alive = aliveToken;
            fmt.createPluginInstanceAsync (desc, sr, bs,
                [this, alive, isInput, groupIdx, slotIdx, state, wasBypassed, channelSet, getGroupForChain]
                (std::unique_ptr<juce::AudioPluginInstance> instance, const juce::String& err)
                {
                    juce::ignoreUnused (err);
                    if (! alive->load (std::memory_order_acquire)) return;
                    if (instance == nullptr) return;
                    if (state.getSize() > 0)
                        dcr::safe::setStateInformation (*instance,
                                                        state.getData(),
                                                        (int) state.getSize());
                    auto* g2 = getGroupForChain (isInput, groupIdx);
                    if (g2 == nullptr) return;
                    if (slotIdx < 0 || slotIdx >= (int) g2->pluginSlots.size()) return;
                    auto& host = g2->pluginSlots[(size_t) slotIdx];
                    if (! host) return;
                    host->setPlugin (std::move (instance), channelSet);
                    host->setBypassed (wasBypassed);
                    if (isInput) inputGroupPanel.rebuild();
                    else         groupPanel.rebuild();
                });
        }
    }
}

MainComponent::MatrixStateByName MainComponent::captureMatrixByName() const
{
    MatrixStateByName s;
    const auto& info = engine.getDeviceInfo();
    const auto& m    = engine.getRoutingMatrix();

    std::vector<ChannelKey> inKeys, outKeys;
    for (auto& d : info)
    {
        for (int c = 0; c < d.numInputChannels;  ++c) inKeys .push_back ({ d.name, c });
        for (int c = 0; c < d.numOutputChannels; ++c) outKeys.push_back ({ d.name, c });
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

    auto findInputIdx = [&] (const ChannelKey& key) -> int
    {
        int idx = 0;
        for (auto& d : info)
        {
            if (d.name == key.dev)
            {
                if (key.ch < d.numInputChannels) return idx + key.ch;
                return -1;
            }
            idx += d.numInputChannels;
        }
        return -1;
    };
    auto findOutputIdx = [&] (const ChannelKey& key) -> int
    {
        int idx = 0;
        for (auto& d : info)
        {
            if (d.name == key.dev)
            {
                if (key.ch < d.numOutputChannels) return idx + key.ch;
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
        int ii = findInputIdx  (xp.src);
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
        [this] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{}) return;
            if (file.getFileExtension().isEmpty()) file = file.withFileExtension ("xml");
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
        [this] (const juce::FileChooser& fc)
        {
            auto file = fc.getResult();
            if (file == juce::File{} || ! file.existsAsFile()) return;
            Snapshot s;
            if (SnapshotStore::load (file, s))
                applySnapshot (s);
        });
}

void MainComponent::switchTab (Tab newTab)
{
    currentTab = newTab;

    matrixTabBtn.setToggleState (currentTab == RoutingTab, juce::dontSendNotification);
    groupsTabBtn.setToggleState (currentTab == GroupsTab, juce::dontSendNotification);
    statusTabBtn.setToggleState (currentTab == StatusTab, juce::dontSendNotification);

    matrixView.setVisible (currentTab == RoutingTab);

    // Keep floating/detached panels visible so they display in their windows
    if (groupPanelDetached)
        groupPanel.setVisible (true);
    if (statusPanelDetached)
        statusPanel.setVisible (true);

    // Keep detached panels themselves visible (they live in their own windows).
    if (groupPanelDetached)       groupPanel     .setVisible (true);
    if (inputGroupPanelDetached)  inputGroupPanel.setVisible (true);
    if (statusPanelDetached)      statusPanel    .setVisible (true);

    if (currentTab == GroupsTab)
    {
        groupsPlaceholder     .setVisible (groupPanelDetached);
        inputGroupsPlaceholder.setVisible (inputGroupPanelDetached);
        if (! groupPanelDetached)      groupPanel     .setVisible (true);
        if (! inputGroupPanelDetached) inputGroupPanel.setVisible (true);
        if (! statusPanelDetached)     statusPanel    .setVisible (false);
        statusPlaceholder.setVisible (false);
    }
    else if (currentTab == StatusTab)
    {
        statusPlaceholder.setVisible (statusPanelDetached);
        if (! statusPanelDetached)     statusPanel    .setVisible (true);
        if (! groupPanelDetached)      groupPanel     .setVisible (false);
        if (! inputGroupPanelDetached) inputGroupPanel.setVisible (false);
        groupsPlaceholder     .setVisible (false);
        inputGroupsPlaceholder.setVisible (false);
    }
    else // RoutingTab
    {
        if (! groupPanelDetached)      groupPanel     .setVisible (false);
        if (! inputGroupPanelDetached) inputGroupPanel.setVisible (false);
        if (! statusPanelDetached)     statusPanel    .setVisible (false);
        groupsPlaceholder     .setVisible (false);
        inputGroupsPlaceholder.setVisible (false);
        statusPlaceholder     .setVisible (false);
    }

    resized();
}

} // namespace dcr

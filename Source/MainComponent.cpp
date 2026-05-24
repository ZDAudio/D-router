#include "MainComponent.h"

#include "Persistence/SettingsStore.h"
#include "Routing/OutputGroup.h"
#include "Routing/OutputGroupManager.h"
#include "UI/DeviceManagerDialog.h"
#include "UI/GroupManagerDialog.h"
#include "UI/SettingsDialog.h"

namespace dcr {

MainComponent::MainComponent()
{
    juce::LookAndFeel::setDefaultLookAndFeel (&customLookAndFeel);
    setLookAndFeel (&customLookAndFeel);

    openGLContext.setContinuousRepainting (false);

    setSize (1100, 700);

    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    title.setColour (juce::Label::textColourId, customLookAndFeel.getAccent());
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
                engine.setSettings (*s);
                if (persistToDisk)
                    SettingsStore::save (*s);
                if (needsRestart && ! currentSpecs.empty())
                    applyDeviceSelection (currentSpecs);
                // Update UI-only timers immediately.
                startTimer (engine.getSettings().statusTimerMs);
                matrixView.pauseUpdates();
                matrixView.resumeUpdates();
                // Re-apply theme colours and force a global repaint.
                customLookAndFeel.applyTheme (engine.getSettings());
                title.setColour (juce::Label::textColourId, customLookAndFeel.getAccent());
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
    addAndMakeVisible (stopButton);

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
    title.setColour (juce::Label::textColourId, customLookAndFeel.getAccent());

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

    // Auto-restore last session.
    Snapshot s;
    if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s))
        applySnapshot (s);

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
    // Auto-save on shutdown.
    SnapshotStore::save (SnapshotStore::getLastUsedFile(), gatherCurrentSnapshot());
    engine.stop();
}

void MainComponent::parentHierarchyChanged()
{
    // Attach OpenGL only once we have a real top-level component with a peer.
    if (! openGLContext.isAttached())
        if (auto* top = getTopLevelComponent())
            if (top->getPeer() != nullptr)
                openGLContext.attachTo (*top);
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
    title.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (10);
    devicesButton.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (4);
    settingsButton.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (4);
    groupsButton  .setBounds (top.removeFromLeft (90));

    // Right Session Section (Save, Load, Stop)
    stopButton.setBounds (top.removeFromRight (60));
    top.removeFromRight (12); // Extra separation for safety
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
    if (isReconfiguring.exchange (true)) return;  // ignore concurrent calls

    // Matrix is about to be rebuilt - any saved-panic indices would point at
    // the old channel layout, so drop the panic state cleanly.
    if (inPanic || ! savedInputMutes.empty() || ! savedOutputMutes.empty())
    {
        inPanic = false;
        savedInputMutes.clear();
        savedOutputMutes.clear();
        updatePanicButtonAppearance();
    }

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
    matrixView.pauseUpdates();
    stopTimer();

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

            matrixView.rebuildFromEngine();
            matrixView.resumeUpdates();
            groupPanel.rebuild();
            inputGroupPanel.rebuild();
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

    // Restart engine with snapshot's devices.
    applyDeviceSelection (s.devices);    // this rebuilds matrix UI and starts engine

    // Now apply gains (defensively, in case channel counts don't match).
    auto& m = engine.getRoutingMatrix();
    const int nIn  = m.getNumInputs();
    const int nOut = m.getNumOutputs();
    for (size_t n = 0; n < s.inputTrim.size()  && (int) n < nIn;  ++n) m.setInputTrim  ((int) n, s.inputTrim [n]);
    for (size_t n = 0; n < s.outputTrim.size() && (int) n < nOut; ++n) m.setOutputTrim ((int) n, s.outputTrim[n]);
    for (size_t n = 0; n < s.inputMute.size()  && (int) n < nIn;  ++n) m.setInputMute  ((int) n, s.inputMute [n] != 0);
    for (size_t n = 0; n < s.outputMute.size() && (int) n < nOut; ++n) m.setOutputMute ((int) n, s.outputMute[n] != 0);
    for (size_t n = 0; n < s.inputSolo.size()  && (int) n < nIn;  ++n) m.setInputSolo  ((int) n, s.inputSolo [n] != 0);
    for (const auto& xp : s.crosspoints)
        if (xp.outputCh < nOut && xp.inputCh < nIn)
            m.setCrosspoint (xp.outputCh, xp.inputCh, xp.gain);

    // Update UI to reflect new gains.
    matrixView.rebuildFromEngine();
    refreshStatus();
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

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
    setSize (1100, 700);

    title.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    title.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (title);

    devicesButton .onClick = [this] { openDeviceDialog(); };
    settingsButton.onClick = [this]
    {
        SettingsDialog::launch (engine.getSettings(),
            [this] (std::optional<EngineSettings> s)
            {
                if (! s.has_value()) return;
                const bool needsRestart = ! s->audioPathEquals (engine.getSettings());
                engine.setSettings (*s);
                SettingsStore::save (*s);
                if (needsRestart && ! currentSpecs.empty())
                    applyDeviceSelection (currentSpecs);
                // Update UI-only timers immediately.
                startTimer (engine.getSettings().statusTimerMs);
                matrixView.pauseUpdates();
                matrixView.resumeUpdates();
            });
    };
    saveButton    .onClick = [this] { saveSnapshotInteractive(); };
    loadButton    .onClick = [this] { loadSnapshotInteractive(); };
    stopButton    .onClick = [this] { stopEngine(); };
    groupsButton.onClick = [this]
    {
        GroupManagerDialog::launch (engine, [this] { groupPanel.rebuild(); });
    };
    addAndMakeVisible (devicesButton);
    addAndMakeVisible (settingsButton);
    addAndMakeVisible (groupsButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (loadButton);
    addAndMakeVisible (stopButton);
    addAndMakeVisible (groupPanel);
    groupPanel.onPopOutRequested = [this] { toggleGroupPanelDetach(); };
    groupPanel.onGroupHover = [this] (const std::vector<int>& outs)
    {
        matrixView.setHighlightedOutputs (outs);
    };

    matrixViewport.setViewedComponent (&matrixView, false);
    matrixViewport.setScrollBarsShown (true, true);
    addAndMakeVisible (matrixViewport);

    addAndMakeVisible (statusPanel);
    statusPanel.onPopOutRequested = [this] { toggleStatusPanelDetach(); };

    // Load persistent settings (engine SR, ring sizes, SRC quality, etc.).
    engine.setSettings (SettingsStore::load());

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
    startTimer (engine.getSettings().statusTimerMs);
}

void MainComponent::timerCallback() { refreshStatus(); }

MainComponent::~MainComponent()
{
    if (reconfigThread.joinable()) reconfigThread.join();
    // Auto-save on shutdown.
    SnapshotStore::save (SnapshotStore::getLastUsedFile(), gatherCurrentSnapshot());
    engine.stop();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour::fromRGB (28, 28, 32));
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (12);
    auto top = r.removeFromTop (32);
    title.setBounds (top.removeFromLeft (160));
    top.removeFromLeft (10);
    devicesButton .setBounds (top.removeFromLeft (90));
    top.removeFromLeft (4);
    settingsButton.setBounds (top.removeFromLeft (90));
    top.removeFromLeft (4);
    groupsButton  .setBounds (top.removeFromLeft (90));
    top.removeFromLeft (4);
    saveButton    .setBounds (top.removeFromLeft (70));
    top.removeFromLeft (4);
    loadButton    .setBounds (top.removeFromLeft (70));
    top.removeFromLeft (4);
    stopButton    .setBounds (top.removeFromLeft (60));

    r.removeFromTop (8);
    if (! statusPanelDetached)
    {
        auto statusBox = r.removeFromBottom (160);
        r.removeFromBottom (6);
        statusPanel.setBounds (statusBox);
    }
    if (! groupPanelDetached)
    {
        auto groupBox = r.removeFromBottom (220);
        r.removeFromBottom (6);
        groupPanel.setBounds (groupBox);
    }
    matrixViewport.setBounds (r);
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
    resized();
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
    resized();
}

int MainComponent::cards_default_width() { return 800; }

void MainComponent::refreshStatus()
{
    statusPanel.refreshNow();
}

#if 0   // legacy in-line status code; superseded by StatusPanel
void legacy_refreshStatus_unused()
{
    juce::String s;
    {
        const int nIn  = 0, nOut = 0;
        const auto processedBlocks = engine.getMatrixBlocksProcessed();
        const auto stalledBlocks   = engine.getMatrixBlocksStalled();
        const auto totalBlocks     = processedBlocks + stalledBlocks;
        const float stalledRatio   = totalBlocks > 0 ? (float) stalledBlocks / (float) totalBlocks : 0.0f;
        const float cpuAvg         = engine.getCpuLoadAvg();
        const float cpuPeak        = engine.getCpuLoadPeak();

        s << "   CPU " << juce::String (cpuAvg  * 100.0f, 1) << "% (peak "
                       << juce::String (cpuPeak * 100.0f, 1) << "%)";
        s << "   stalled " << juce::String (stalledRatio * 100.0f, 2) << "%";
        s << "   xrun in=" << (juce::int64) engine.getTotalInputOverruns()
          << " out="       << (juce::int64) engine.getTotalOutputUnderruns();

        const double lastUnderrunMs = engine.getMostRecentUnderrunMs();
        if (lastUnderrunMs > 0.0)
        {
            const double agoSec = (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) / 1000.0;
            s << "   last dropout " << juce::String (agoSec, 1) << "s ago";
        }

        const auto& es = engine.getSettings();
        juce::Colour col = juce::Colours::lightgrey;
        if (cpuAvg >= es.cpuCritRatio   || stalledRatio >= es.stalledCritRatio
            || (lastUnderrunMs > 0.0 && (juce::Time::getMillisecondCounterHiRes() - lastUnderrunMs) < 5000.0))
            col = juce::Colour::fromRGB (240, 80, 70);
        else if (cpuAvg >= es.cpuWarnRatio || stalledRatio >= es.stalledWarnRatio)
            col = juce::Colour::fromRGB (240, 200, 60);
        juce::ignoreUnused (col);

        // Show fill levels for first few input and output rings.
        const int showN = juce::jmin (4, nIn);
        const int showM = juce::jmin (4, nOut);
        s << "   inRing[";
        for (int n = 0; n < showN; ++n)
            s << (n ? "," : "") << (juce::int64) engine.getInputRingFill (n);
        s << "]  outRing[";
        for (int m = 0; m < showM; ++m)
            s << (m ? "," : "") << (juce::int64) engine.getOutputRingFill (m);
        s << "]";
    }
    statusLabel.setText (s, juce::dontSendNotification);

    // --- Latency report panel -------------------------------------------------
    juce::String lat;
    auto rep = engine.getLatencyReport();
    const double eng = rep.engineSampleRate;

    auto pad = [] (const juce::String& s, int n) -> juce::String
    {
        return s.length() >= n ? s : (s + juce::String::repeatedString (" ", n - s.length()));
    };

    if (rep.devices.empty())
    {
        lat << "Latency:  (no devices open)";
    }
    else
    {
        lat << pad ("Device", 28) << pad ("Dir", 6) << pad ("HW spl", 10)
            << pad ("SRC spl", 10) << pad ("Total ms", 10) << "\n";

        for (auto& d : rep.devices)
        {
            if (d.hasInput)
            {
                lat << pad (d.name.substring (0, 27), 28)
                    << pad ("IN",  6)
                    << pad (juce::String (d.hwInputSamples)  + "@" + juce::String ((int) d.deviceSampleRate / 1000) + "k", 10)
                    << pad (juce::String (d.srcInLatencyEng) + "@" + juce::String ((int) eng / 1000) + "k", 10)
                    << pad (juce::String (d.getInputLatencyMs (eng), 2), 10) << "\n";
            }
            if (d.hasOutput)
            {
                lat << pad (d.name.substring (0, 27), 28)
                    << pad ("OUT", 6)
                    << pad (juce::String (d.hwOutputSamples)  + "@" + juce::String ((int) d.deviceSampleRate / 1000) + "k", 10)
                    << pad (juce::String (d.srcOutLatencyDev) + "@" + juce::String ((int) d.deviceSampleRate / 1000) + "k", 10)
                    << pad (juce::String (d.getOutputLatencyMs (eng), 2), 10) << "\n";
            }
        }

        lat << "\n";
        lat << "Engine path  = " << juce::String (rep.getEngineContributionMs(), 2) << " ms  "
            << "(1 wait block + " << rep.outputPreFillBlocks << " pre-fill blocks @ "
            << (int) rep.engineBlockSize << " spl / " << (int) eng << " Hz)\n";
        lat << "Round-trip worst = " << juce::String (rep.getRoundTripMsWorst(), 2) << " ms  "
            << "(input + engine + output)";
    }
    juce::ignoreUnused (lat);
}
#endif

void MainComponent::stopEngine()
{
    engine.stop();
    currentSpecs.clear();
    matrixView.rebuildFromEngine();
    refreshStatus();
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

    // Capture current state on message thread (cheap), then offload the slow
    // CoreAudio open/close to a worker thread.
    auto preserved = captureMatrixByName();

    devicesButton.setEnabled (false);
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
            startTimer (250);
            refreshStatus();

            devicesButton.setEnabled (true);
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

    // Output groups
    const auto& mgr = engine.getGroupManager();
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
        s.outputGroups.push_back (std::move (gs));
    }

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
    auto& mgr = engine.getGroupManager();
    while (mgr.getNumGroups() > 0) mgr.removeGroup (0);
    for (const auto& gs : s.outputGroups)
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

} // namespace dcr

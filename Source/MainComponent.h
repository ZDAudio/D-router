#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

#include <atomic>
#include <thread>

#include "Engine/AudioEngine.h"
#include "Persistence/SnapshotStore.h"
#include "UI/MatrixView.h"
#include "UI/OutputGroupPanel.h"
#include "UI/StatusPanel.h"

namespace dcr {

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    void openDeviceDialog();
    void applyDeviceSelection (std::vector<AudioEngine::DeviceSpec> newSpecs);
    void refreshStatus();
    void stopEngine();
    Snapshot gatherCurrentSnapshot() const;
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

    AudioEngine engine;
    std::vector<AudioEngine::DeviceSpec> currentSpecs;
    std::unique_ptr<juce::FileChooser> activeChooser;

    std::thread       reconfigThread;
    std::atomic<bool> isReconfiguring { false };

    juce::Label      title { {}, "dcorerouter" };
    juce::TextButton devicesButton  { "Devices..." };
    juce::TextButton settingsButton { "Settings..." };
    juce::TextButton groupsButton   { "Groups..." };
    juce::TextButton saveButton     { "Save..." };
    juce::TextButton loadButton     { "Load..." };
    juce::TextButton stopButton     { "Stop" };

    juce::Viewport   matrixViewport;
    MatrixView       matrixView { engine };
    OutputGroupPanel groupPanel  { engine };
    StatusPanel      statusPanel { engine };

    bool groupPanelDetached  = false;
    bool statusPanelDetached = false;
    std::unique_ptr<juce::DocumentWindow> groupWindow;
    std::unique_ptr<juce::DocumentWindow> statusWindow;
    void toggleGroupPanelDetach();
    void toggleStatusPanelDetach();
    static int cards_default_width();

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace dcr

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

#include <vector>

#include <atomic>
#include <thread>

#include "Engine/AudioEngine.h"
#include "Persistence/SnapshotStore.h"
#include "UI/MatrixView.h"
#include "UI/OutputGroupPanel.h"
#include "UI/StatusPanel.h"
#include "UI/LookAndFeel.h"

namespace dcr {

class MainComponent : public juce::Component,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void parentHierarchyChanged() override;

private:
    void timerCallback() override;

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
    void updatePanicButtonAppearance();
    bool inPanic = false;
    std::vector<unsigned char> savedInputMutes;
    std::vector<unsigned char> savedOutputMutes;
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

    LookAndFeel customLookAndFeel;

    // GPU-backed renderer for the entire top-level component tree.  Drops
    // composite & paint cost dramatically for the slider/meter-heavy UI.
    juce::OpenGLContext openGLContext;

    juce::Label      title { {}, "D-Core Router" };
    juce::TextButton devicesButton  { "Devices..." };
    juce::TextButton settingsButton { "Settings..." };
    juce::TextButton groupsButton    { "Groups..." };
    juce::TextButton saveButton     { "Save..." };
    juce::TextButton loadButton     { "Load..." };
    juce::TextButton stopButton     { "PANIC" };

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

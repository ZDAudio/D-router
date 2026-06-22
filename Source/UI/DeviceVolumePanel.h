#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Engine/AudioEngine.h"
#include "Engine/DeviceVolume.h"
#include "Engine/SystemAudioDevices.h"

namespace dcr
{

    // One half (inputs OR outputs) of the AUDIO SETUP tab: a horizontally
    // scrollable row of vertical "fader" strips, one per device on this direction.
    // Each strip drives that device's hardware volume + mute via DeviceVolume
    // (CoreAudio, message-thread).  Devices that expose no settable volume are
    // shown greyed with an "N/A" overlay -- macOS gives no control for them
    // (typical for virtual / loopback devices, many inputs, digital outputs).
    //
    // Mirrors OutputGroupPanel's shape (Viewport + strips) but deliberately
    // simpler: no plugin slots, no meters, no detach.  A low-frequency timer polls
    // the OS so the strips track external volume changes (media keys / menu-bar
    // slider / Audio MIDI Setup).
    class DeviceVolumePanel : public juce::Component,
                              private juce::Timer
    {
    public:
        enum class Direction { Inputs,
            Outputs };

        DeviceVolumePanel (AudioEngine& engine, Direction dir);

        // Rebuild strips from the current device list (call after engine restart /
        // device-selection change, and when the tab is opened).
        void rebuild();

        // Freeze / resume the OS-poll timer.  Called around engine reconfigure in
        // line with the other panels: rebuild() reads engine.getDeviceInfo(), which
        // is unsafe while the worker thread is swapping engine state.
        void pauseUpdates() { stopTimer(); }
        void resumeUpdates();

        void paint (juce::Graphics&) override;
        void resized() override;

    private:
        void timerCallback() override;

        // Direction -> SystemAudioDevices::Scope.
        SystemAudioDevices::Scope defaultScope() const noexcept;
        // OS -> UI: set the combo selection (by AudioDeviceID) and the strip ★ from
        // the current system default.  Skips the combo while its popup is open.
        void syncDefaultToOS();
        // UI -> OS: apply the combo's selection as the new system default.
        void applyDefaultSelection();

        // One vertical device strip: name + fader + mute, greyed when the device
        // has no controllable volume.
        struct Strip : public juce::Component
        {
            Strip (const juce::String& deviceName, DeviceVolume::Scope scope);
            void resized() override;
            void pull(); // OS -> UI (skips the fader while dragging)
            void applyEnabledLook();
            void setIsDefault (bool isDefault); // show/hide the system-default ★

            DeviceVolume vol;
            juce::Label nameLabel;
            juce::Slider fader { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
            juce::Label dbLabel; // dB readout (matches Audio MIDI Setup), 2 dp
            juce::TextButton mute { "M" };
            juce::Label naLabel; // "N/A" overlay when no controllable volume
            juce::Label starLabel; // "★" badge: this device is the system default
            bool dragging = false;
            bool lastMuted = false;
        };

        AudioEngine& engine;
        Direction direction;

        juce::Label title;
        juce::Label defaultLabel; // "System Output" / "System Input"
        juce::ComboBox defaultCombo; // pick the macOS default device for this direction
        juce::Array<AudioDeviceRef> defaultDevices; // parallel to combo items (id = index + 1)
        juce::Viewport viewport;
        juce::Component stripsHolder;
        juce::OwnedArray<Strip> strips;
        juce::Label emptyLabel; // shown when no devices on this direction
    };

} // namespace dcr

#pragma once

#include <juce_core/juce_core.h>

namespace dcr {

// Per-device hardware volume + mute via the CoreAudio HAL.  This is the
// device's own driver-level output/input level -- the same control as Audio
// MIDI Setup / the menu-bar slider -- NOT a per-route or per-app gain.  It is a
// SYSTEM-WIDE setting shared with every other app using the device, and a
// different layer from the router's crosspoint / trim / group / master gains.
//
// MESSAGE THREAD ONLY: every call is IPC to coreaudiod and may block briefly.
// Never call from the matrix thread or a CoreAudio IO callback.
//
// Devices are identified BY NAME (matching how the rest of D-Router names
// them).  The AudioDeviceID is resolved on construction / refresh(); if the
// device is gone or the name no longer matches, isAvailable() is false and the
// setters no-op.
//
// REALITY CHECK: many devices expose NO settable volume on a given scope --
// notably the virtual / loopback / aggregate devices this router is usually
// wired to, most digital outputs, and a lot of inputs.  hasVolume() /
// hasMute() report that so the UI can grey out; macOS simply does not provide
// the control for those.
class DeviceVolume
{
public:
    enum class Scope { Input, Output };

    DeviceVolume() = default;
    DeviceVolume (const juce::String& deviceName, Scope scope);

    // Re-resolve the AudioDeviceID and re-probe capabilities (call when the
    // device list changes).
    void refresh();

    bool isAvailable() const noexcept { return deviceID != 0; }
    bool hasVolume()   const noexcept { return volumeSettable; }
    bool hasMute()     const noexcept { return muteSettable; }

    // 0..1 scalar (Apple's perceptual mapping -- NOT dB).  0 if unavailable.
    float getVolume() const;
    bool  setVolume (float gain01);   // true on success

    bool getMute() const;             // false if no mute control
    bool setMute (bool shouldMute);   // true on success

    // Current volume in dB, matching Audio MIDI Setup
    // (kAudioDevicePropertyVolumeDecibels -- NOT a log of the scalar, the scalar
    // uses a different curve).  Only meaningful when hasDb(); returns 0 else.
    bool  hasDb() const noexcept { return dbElement >= 0; }
    float getVolumeDb() const;

    const juce::String& getDeviceName() const noexcept { return name; }
    Scope               getScope()      const noexcept { return scope; }

private:
    juce::String name;
    Scope        scope { Scope::Output };
    // AudioDeviceID kept as unsigned int so this header stays CoreAudio-free
    // (AudioDeviceID is a UInt32; kAudioObjectUnknown == 0).
    unsigned int deviceID = 0;
    bool         volumeSettable = false;
    bool         muteSettable   = false;
    // Element that carries kAudioDevicePropertyVolumeDecibels: 0 = master,
    // 1 = first channel (some devices only expose it per-channel), -1 = none.
    int          dbElement = -1;
};

} // namespace dcr

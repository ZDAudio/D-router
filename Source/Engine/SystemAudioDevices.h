#pragma once

#include <juce_core/juce_core.h>

namespace dcr
{

    // A device the OS can use as the system default on a given direction, paired
    // with its CoreAudio AudioDeviceID.  deviceID is kept as unsigned int so this
    // header stays CoreAudio-free (AudioDeviceID is a UInt32; 0 == kAudioObjectUnknown).
    struct AudioDeviceRef
    {
        juce::String name;
        unsigned int deviceID = 0;
    };

    // Read / set the macOS *default* output or input device -- the same setting as
    // System Settings -> Sound -> Output/Input and the menu-bar sound picker.  This
    // is a SYSTEM-WIDE pointer deciding where ordinary apps send/take audio; it is
    // INDEPENDENT of D-Router's own routing (the engine opens devices by explicit
    // ID, so changing this never reroutes the matrix).
    //
    // MESSAGE THREAD ONLY: every call is IPC to coreaudiod and may block briefly.
    // Never call from the matrix thread or a CoreAudio IO callback.
    class SystemAudioDevices
    {
    public:
        enum class Scope { Input,
            Output };

        // All devices carrying streams on this direction (i.e. eligible to be the
        // system default output/input).  Order is the HAL's own device order.
        static juce::Array<AudioDeviceRef> list (Scope);

        // The current system default device for this direction (deviceID 0 / empty
        // name if none, or on failure).
        static AudioDeviceRef getDefault (Scope);

        // Make the device with this AudioDeviceID the system default.  true on
        // success; no-op + false if id is 0 or the HAL rejects it.
        static bool setDefault (Scope, unsigned int deviceID);
    };

} // namespace dcr

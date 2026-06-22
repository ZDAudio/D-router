#include "Engine/SystemAudioDevices.h"

#include <CoreAudio/CoreAudio.h>

#include <vector>

namespace dcr
{

    namespace
    {

        AudioObjectPropertyScope caScope (SystemAudioDevices::Scope s) noexcept
        {
            return s == SystemAudioDevices::Scope::Input ? kAudioDevicePropertyScopeInput
                                                         : kAudioDevicePropertyScopeOutput;
        }

        // The system-object selector carrying the default device for this direction.
        AudioObjectPropertySelector defaultSelector (SystemAudioDevices::Scope s) noexcept
        {
            return s == SystemAudioDevices::Scope::Input ? kAudioHardwarePropertyDefaultInputDevice
                                                         : kAudioHardwarePropertyDefaultOutputDevice;
        }

        // Read a device's CoreAudio name (kAudioObjectPropertyName) as a juce::String.
        // (Local copy of the same helper in DeviceVolume.cpp -- kept here so this
        // module is self-contained and the proven DeviceVolume.cpp is untouched.)
        juce::String deviceNameOf (AudioObjectID dev)
        {
            CFStringRef cf = nullptr;
            UInt32 sz = sizeof (cf);
            AudioObjectPropertyAddress addr { kAudioObjectPropertyName,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain };
            if (AudioObjectGetPropertyData (dev, &addr, 0, nullptr, &sz, &cf) != noErr || cf == nullptr)
                return {};
            auto s = juce::String::fromCFString (cf);
            CFRelease (cf);
            return s;
        }

        // Does this device carry at least one stream on the given scope?
        bool hasStreams (AudioObjectID dev, AudioObjectPropertyScope scope)
        {
            AudioObjectPropertyAddress addr { kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
            UInt32 sz = 0;
            return AudioObjectGetPropertyDataSize (dev, &addr, 0, nullptr, &sz) == noErr && sz > 0;
        }

        // Every AudioObjectID the HAL knows about (empty on failure).
        std::vector<AudioObjectID> allDeviceIDs()
        {
            AudioObjectPropertyAddress addr { kAudioHardwarePropertyDevices,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain };
            UInt32 sz = 0;
            if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &sz) != noErr || sz == 0)
                return {};
            std::vector<AudioObjectID> ids (sz / sizeof (AudioObjectID));
            if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &sz, ids.data()) != noErr)
                return {};
            return ids;
        }

    } // namespace

    juce::Array<AudioDeviceRef> SystemAudioDevices::list (Scope scope)
    {
        const auto sc = caScope (scope);
        juce::Array<AudioDeviceRef> out;
        for (auto id : allDeviceIDs())
            if (hasStreams (id, sc))
                out.add ({ deviceNameOf (id), (unsigned int) id });
        return out;
    }

    AudioDeviceRef SystemAudioDevices::getDefault (Scope scope)
    {
        AudioObjectPropertyAddress addr { defaultSelector (scope),
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        AudioDeviceID dev = kAudioObjectUnknown;
        UInt32 sz = sizeof (dev);
        if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &sz, &dev) != noErr
            || dev == kAudioObjectUnknown)
            return {};
        return { deviceNameOf (dev), (unsigned int) dev };
    }

    bool SystemAudioDevices::setDefault (Scope scope, unsigned int deviceID)
    {
        if (deviceID == 0)
            return false;
        AudioObjectPropertyAddress addr { defaultSelector (scope),
            kAudioObjectPropertyScopeGlobal,
            kAudioObjectPropertyElementMain };
        AudioDeviceID dev = (AudioDeviceID) deviceID;
        return AudioObjectSetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, sizeof (dev), &dev) == noErr;
    }

} // namespace dcr

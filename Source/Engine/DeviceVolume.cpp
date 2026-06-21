#include "Engine/DeviceVolume.h"

#include <AudioToolbox/AudioHardwareService.h> // kAudioHardwareServiceDeviceProperty_VirtualMainVolume
#include <CoreAudio/CoreAudio.h>

#include <vector>

namespace dcr
{

    namespace
    {

        // Selector name changed across SDKs; mirror JUCE's fallback so this builds on
        // older toolchains too.
        constexpr AudioObjectPropertySelector kVirtualMainVolume =
#if defined(MAC_OS_VERSION_12_0)
            kAudioHardwareServiceDeviceProperty_VirtualMainVolume;
#else
            kAudioHardwareServiceDeviceProperty_VirtualMasterVolume;
#endif

        AudioObjectPropertyScope caScope (DeviceVolume::Scope s) noexcept
        {
            return s == DeviceVolume::Scope::Input ? kAudioDevicePropertyScopeInput
                                                   : kAudioDevicePropertyScopeOutput;
        }

        AudioObjectPropertyAddress volumeAddr (AudioObjectPropertyScope scope) noexcept
        {
            return { kVirtualMainVolume, scope, kAudioObjectPropertyElementMain };
        }

        AudioObjectPropertyAddress muteAddr (AudioObjectPropertyScope scope) noexcept
        {
            return { kAudioDevicePropertyMute, scope, kAudioObjectPropertyElementMain };
        }

        AudioObjectPropertyAddress dbAddr (AudioObjectPropertyScope scope, UInt32 element) noexcept
        {
            return { kAudioDevicePropertyVolumeDecibels, scope, element };
        }

        // Read a device's CoreAudio name (kAudioObjectPropertyName) as a juce::String.
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

        // Does this device carry at least one stream on the given scope?  Used to pick
        // the right device when an input and an output device share a name.
        bool hasStreams (AudioObjectID dev, AudioObjectPropertyScope scope)
        {
            AudioObjectPropertyAddress addr { kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain };
            UInt32 sz = 0;
            return AudioObjectGetPropertyDataSize (dev, &addr, 0, nullptr, &sz) == noErr && sz > 0;
        }

        // Resolve a (name, scope) pair to an AudioDeviceID, or 0 if not found.  Exact
        // name match first, then case-insensitive as a fallback (JUCE's reported names
        // usually match kAudioObjectPropertyName, but don't bet the feature on it).
        AudioObjectID resolveDevice (const juce::String& name, AudioObjectPropertyScope scope)
        {
            AudioObjectPropertyAddress addr { kAudioHardwarePropertyDevices,
                kAudioObjectPropertyScopeGlobal,
                kAudioObjectPropertyElementMain };
            UInt32 sz = 0;
            if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &sz) != noErr || sz == 0)
                return 0;

            std::vector<AudioObjectID> ids (sz / sizeof (AudioObjectID));
            if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &sz, ids.data()) != noErr)
                return 0;

            for (auto id : ids)
                if (hasStreams (id, scope) && deviceNameOf (id) == name)
                    return id;
            for (auto id : ids)
                if (hasStreams (id, scope) && deviceNameOf (id).equalsIgnoreCase (name))
                    return id;
            return 0;
        }

        bool propSettable (AudioObjectID dev, const AudioObjectPropertyAddress& addr)
        {
            Boolean settable = false;
            return AudioObjectHasProperty (dev, &addr)
                   && AudioObjectIsPropertySettable (dev, &addr, &settable) == noErr
                   && settable;
        }

    } // namespace

    DeviceVolume::DeviceVolume (const juce::String& deviceName, Scope s)
        : name (deviceName), scope (s)
    {
        refresh();
    }

    void DeviceVolume::refresh()
    {
        const auto sc = caScope (scope);
        deviceID = resolveDevice (name, sc);
        volumeSettable = deviceID != 0 && propSettable ((AudioObjectID) deviceID, volumeAddr (sc));
        muteSettable = deviceID != 0 && propSettable ((AudioObjectID) deviceID, muteAddr (sc));

        // Find which element exposes the dB readout: master (0) first, then the
        // first channel (1) for devices that only publish per-channel dB.
        dbElement = -1;
        if (deviceID != 0)
            for (UInt32 el : { (UInt32) kAudioObjectPropertyElementMain, (UInt32) 1 })
            {
                const auto a = dbAddr (sc, el);
                if (AudioObjectHasProperty ((AudioObjectID) deviceID, &a))
                {
                    dbElement = (int) el;
                    break;
                }
            }
    }

    float DeviceVolume::getVolumeDb() const
    {
        if (deviceID == 0 || dbElement < 0)
            return 0.0f;
        const auto addr = dbAddr (caScope (scope), (UInt32) dbElement);
        Float32 v = 0.0f;
        UInt32 sz = sizeof (v);
        if (AudioObjectGetPropertyData ((AudioObjectID) deviceID, &addr, 0, nullptr, &sz, &v) != noErr)
            return 0.0f;
        return (float) v;
    }

    float DeviceVolume::getVolume() const
    {
        if (deviceID == 0)
            return 0.0f;
        const auto addr = volumeAddr (caScope (scope));
        if (!AudioObjectHasProperty ((AudioObjectID) deviceID, &addr))
            return 0.0f;
        Float32 v = 0.0f;
        UInt32 sz = sizeof (v);
        if (AudioObjectGetPropertyData ((AudioObjectID) deviceID, &addr, 0, nullptr, &sz, &v) != noErr)
            return 0.0f;
        return (float) v;
    }

    bool DeviceVolume::setVolume (float gain01)
    {
        if (!volumeSettable)
            return false;
        const auto addr = volumeAddr (caScope (scope));
        Float32 v = (Float32) juce::jlimit (0.0f, 1.0f, gain01);
        return AudioObjectSetPropertyData ((AudioObjectID) deviceID, &addr, 0, nullptr, sizeof (v), &v) == noErr;
    }

    bool DeviceVolume::getMute() const
    {
        if (deviceID == 0)
            return false;
        const auto addr = muteAddr (caScope (scope));
        if (!AudioObjectHasProperty ((AudioObjectID) deviceID, &addr))
            return false;
        UInt32 m = 0;
        UInt32 sz = sizeof (m);
        if (AudioObjectGetPropertyData ((AudioObjectID) deviceID, &addr, 0, nullptr, &sz, &m) != noErr)
            return false;
        return m != 0;
    }

    bool DeviceVolume::setMute (bool shouldMute)
    {
        if (!muteSettable)
            return false;
        const auto addr = muteAddr (caScope (scope));
        UInt32 m = shouldMute ? 1u : 0u;
        return AudioObjectSetPropertyData ((AudioObjectID) deviceID, &addr, 0, nullptr, sizeof (m), &m) == noErr;
    }

} // namespace dcr

#include "Engine/AppAudioWorker.h"

#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>

namespace
{
// The current system default output device: its UID (for anchoring the tap
// aggregate -- clocking + drift compensation, matches the verified AudioCap
// layout), its name (to detect when it's a device D-Router also opens), and
// whether it's the built-in output.  A tap-only aggregate is the documented
// fallback when anchoring would be brittle (see attach()).
struct DefaultOutputInfo
{
    NSString* uid = nil;
    juce::String name;
    bool builtIn = false;
};

DefaultOutputInfo defaultOutputInfo()
{
    DefaultOutputInfo out;
    AudioObjectID dev = kAudioObjectUnknown;
    AudioObjectPropertyAddress da{
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(dev);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &da, 0, nullptr, &size, &dev) != noErr || dev == kAudioObjectUnknown)
        return out;

    CFStringRef uid = nullptr;
    AudioObjectPropertyAddress ua{
        kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    size = sizeof(uid);
    if (AudioObjectGetPropertyData(dev, &ua, 0, nullptr, &size, &uid) == noErr && uid != nullptr)
        out.uid = (__bridge_transfer NSString*)uid; // +1 CFString -> ARC takes ownership

    CFStringRef name = nullptr;
    AudioObjectPropertyAddress na{
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    size = sizeof(name);
    if (AudioObjectGetPropertyData(dev, &na, 0, nullptr, &size, &name) == noErr && name != nullptr)
    {
        out.name = juce::String::fromCFString(name);
        CFRelease(name);
    }

    UInt32 transport = 0;
    AudioObjectPropertyAddress ta{
        kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    size = sizeof(transport);
    if (AudioObjectGetPropertyData(dev, &ta, 0, nullptr, &size, &transport) == noErr)
        out.builtIn = (transport == kAudioDeviceTransportTypeBuiltIn);

    return out;
}

// IOProc buffer size (frames) for a device, or 0.
int deviceBufferFrames(AudioObjectID dev)
{
    AudioObjectPropertyAddress a{
        kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 frames = 0, size = sizeof(frames);
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &frames) != noErr)
        return 0;
    return (int)frames;
}
} // namespace

namespace dcr
{
AppAudioWorker::AppAudioWorker(bool mute, int nch)
    : muteOriginalOutput(mute), numChannels(nch) {}

AppAudioWorker::~AppAudioWorker()
{
    close();
}

bool AppAudioWorker::open(const EngineSettings& s)
{
    settings = s;
    const int eb = settings.engineBlockSize;

    // Rings hold ENGINE-rate samples (post-SRC), so they size off engine
    // multipliers and are independent of the tap rate (known only at attach).
    size_t ringSize = (size_t)std::max(settings.inputRingMultEng * eb,
                                       settings.inputRingMultDev * eb);
    ringSize = std::min(ringSize, (size_t)(256 * 1024));

    inputRings.clear();
    inputRings.resize((size_t)numChannels);
    for (auto& r : inputRings)
        r.resize(ringSize);

    inputSRCs.clear();
    for (int i = 0; i < numChannels; ++i)
        inputSRCs.push_back(std::make_unique<SampleRateConverter>());

    // Provisional scratch; attach() grows these to the live IOProc block size.
    const int scratch = (int)std::ceil(1.25 * (double)std::max(eb, 4096));
    scratchEngine.assign((size_t)scratch, 0.0f);
    deinterleave.assign((size_t)scratch, 0.0f);
    return true;
}

void AppAudioWorker::close()
{
    detach();
    inputRings.clear();
    inputSRCs.clear();
}

bool AppAudioWorker::attach(AudioObjectID processObject)
{
    if (attached.load(std::memory_order_acquire))
        return true;
    if (processObject == kAudioObjectUnknown)
        return false;

    // 1) Tap description: stereo mixdown of the one process; mute per spec.
    CATapDescription* desc =
        [[CATapDescription alloc] initStereoMixdownOfProcesses:@[ @(processObject) ]];
    desc.name = @"D-Router App Tap";
    desc.UUID = [NSUUID UUID];
    desc.muteBehavior = muteOriginalOutput ? CATapMutedWhenTapped : CATapUnmuted;

    // 2) Create the process tap.
    AudioObjectID newTap = kAudioObjectUnknown;
    const OSStatus tapErr = AudioHardwareCreateProcessTap(desc, &newTap);
    if (tapErr != noErr || newTap == kAudioObjectUnknown)
    {
        juce::Logger::writeToLog("appworker.attach: AudioHardwareCreateProcessTap FAILED os=" + juce::String((int)tapErr) + " proc=" + juce::String((int)processObject) + " -- likely TCC audio-capture permission not granted");
        return false;
    }
    juce::Logger::writeToLog("appworker.attach: tap created proc=" + juce::String((int)processObject) + " tap=" + juce::String((int)newTap));

    // 3) Read the tap's stream format (sample rate; channel count informational).
    AudioStreamBasicDescription asbd{};
    {
        AudioObjectPropertyAddress fa{kAudioTapPropertyFormat,
                                      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 size = sizeof(asbd);
        if (AudioObjectGetPropertyData(newTap, &fa, 0, nullptr, &size, &asbd) != noErr)
        {
            juce::Logger::writeToLog("appworker.attach: read tap format FAILED");
            AudioHardwareDestroyProcessTap(newTap);
            return false;
        }
    }
    tapSampleRate = asbd.mSampleRate > 0 ? asbd.mSampleRate : settings.engineSampleRate;
    juce::Logger::writeToLog("appworker.attach: tap format sr=" + juce::String(asbd.mSampleRate) + " ch=" + juce::String((int)asbd.mChannelsPerFrame));

    // 4) Prepare per-channel SRC (tap rate -> engine rate) and clear rings.
    for (auto& src : inputSRCs)
    {
        src->reset();
        src->prepare(tapSampleRate, settings.engineSampleRate,
                     settings.srcQuality, settings.srcComplexity);
    }
    for (auto& r : inputRings)
        r.clear();

    // 5) Private aggregate device wrapping the tap (verified AudioCap key set).
    const DefaultOutputInfo def = defaultOutputInfo();
    NSString* outUID = def.uid;

    // Anchor-conflict guard (see header).  Anchoring the aggregate to the system
    // default output is fine UNLESS that default is a NON-built-in device D-Router
    // also opens this session: then the tap aggregate and D-Router both drive the
    // same physical device, and two HAL clients on one fragile device (notably
    // Bluetooth) corrupt the tap's capture clock -> pitch + crackle on ALL
    // app-tapped audio (confirmed on a 44.1k BT out the user added).  Drop the
    // anchor (tap-only aggregate) in that case so they never share a device.  We
    // keep the anchor for the built-in output, which tolerates multi-client and is
    // the proven-good path.
    if (outUID != nil && !def.builtIn && !routerOutputNames.empty())
    {
        const juce::String defName = def.name.trim();
        for (const auto& n : routerOutputNames)
            if (defName.isNotEmpty() && defName.equalsIgnoreCase(n.trim()))
            {
                juce::Logger::writeToLog("appworker.attach: default output '" + defName + "' is a non-built-in device D-Router also opens -- using a tap-only aggregate (no anchor) to avoid a shared-device clock conflict");
                outUID = nil; // tap-only aggregate; the `if (outUID != nil)` block below is skipped
                break;
            }
    }

    NSString* aggUID = [[NSUUID UUID] UUIDString];
    NSMutableDictionary* description = [@{
        @(kAudioAggregateDeviceNameKey) : [NSString stringWithFormat:@"DRouter-Tap-%u", (unsigned)processObject],
        @(kAudioAggregateDeviceUIDKey) : aggUID,
        @(kAudioAggregateDeviceIsPrivateKey) : @YES,
        @(kAudioAggregateDeviceIsStackedKey) : @NO,
        @(kAudioAggregateDeviceTapAutoStartKey) : @YES,
        @(kAudioAggregateDeviceTapListKey) : @[ @{
            @(kAudioSubTapDriftCompensationKey) : @YES,
            @(kAudioSubTapUIDKey) : desc.UUID.UUIDString
        } ],
    } mutableCopy];
    if (outUID != nil)
    {
        description[@(kAudioAggregateDeviceMainSubDeviceKey)] = outUID;
        description[@(kAudioAggregateDeviceSubDeviceListKey)] = @[ @{@(kAudioSubDeviceUIDKey) : outUID} ];
    }

    AudioObjectID newAgg = kAudioObjectUnknown;
    const OSStatus aggErr = AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)description, &newAgg);
    if (aggErr != noErr || newAgg == kAudioObjectUnknown)
    {
        juce::Logger::writeToLog("appworker.attach: CreateAggregateDevice FAILED os=" + juce::String((int)aggErr));
        AudioHardwareDestroyProcessTap(newTap);
        return false;
    }

    // 6) Grow scratch to the live IOProc block so ioBlock never allocates.
    const int blockFrames = std::max(deviceBufferFrames(newAgg), settings.engineBlockSize);
    const double ratio = settings.engineSampleRate / std::max(1.0, tapSampleRate);
    const int needScratch = (int)std::ceil(1.25 * (double)blockFrames * std::max(1.0, ratio));
    if ((int)deinterleave.size() < blockFrames)
        deinterleave.assign((size_t)blockFrames, 0.0f);
    if ((int)scratchEngine.size() < needScratch)
        scratchEngine.assign((size_t)needScratch, 0.0f);

    // 7) IOProc -- process taps deliver captured audio via the BLOCK + dispatch-queue
    // variant (this is what AudioCap does; the plain C AudioDeviceCreateIOProcID gives
    // a SILENT input stream for taps).  Serial queue so the SPSC ring keeps exactly one
    // producer.  Publish handles before start.
    if (captureQueue == nullptr)
        captureQueue = (void*)CFBridgingRetain(dispatch_queue_create("dcr.appaudio.tap", DISPATCH_QUEUE_SERIAL));
    AppAudioWorker* self = this;
    AudioDeviceIOProcID newProc = nullptr;
    const OSStatus procErr = AudioDeviceCreateIOProcIDWithBlock(
        &newProc, newAgg, (__bridge dispatch_queue_t)captureQueue,
        ^(const AudioTimeStamp*, const AudioBufferList* inInputData, const AudioTimeStamp*,
          AudioBufferList*, const AudioTimeStamp*) {
          AppAudioWorker::ioProcTrampoline(0, nullptr, inInputData, nullptr, nullptr, nullptr, self);
        });
    if (procErr != noErr || newProc == nullptr)
    {
        juce::Logger::writeToLog("appworker.attach: CreateIOProcIDWithBlock FAILED os=" + juce::String((int)procErr));
        AudioHardwareDestroyAggregateDevice(newAgg);
        AudioHardwareDestroyProcessTap(newTap);
        return false;
    }

    tapId = newTap;
    aggregateId = newAgg;
    procId = newProc;

    const OSStatus startErr = AudioDeviceStart(newAgg, newProc);
    if (startErr != noErr)
    {
        juce::Logger::writeToLog("appworker.attach: AudioDeviceStart FAILED os=" + juce::String((int)startErr));
        detach(); // tears down the handles we just published
        return false;
    }
    attached.store(true, std::memory_order_release); // matrix may now consume (spec §7)
    juce::Logger::writeToLog("appworker.attach: OK capturing agg=" + juce::String((int)newAgg) + " outUID=" + (outUID != nil ? juce::String(outUID.UTF8String) : juce::String("<none>")));
    return true;
}

void AppAudioWorker::detach()
{
    attached.store(false, std::memory_order_release); // matrix stops consuming first

    if (aggregateId != kAudioObjectUnknown && procId != nullptr)
    {
        AudioDeviceStop(aggregateId, procId);
        AudioDeviceDestroyIOProcID(aggregateId, procId);
    }
    if (aggregateId != kAudioObjectUnknown)
        AudioHardwareDestroyAggregateDevice(aggregateId);
    if (tapId != kAudioObjectUnknown)
        AudioHardwareDestroyProcessTap(tapId);

    procId = nullptr;
    aggregateId = kAudioObjectUnknown;
    tapId = kAudioObjectUnknown;
    tapSampleRate = 0.0;

    if (captureQueue != nullptr)
    {
        CFBridgingRelease(captureQueue); // balances the CFBridgingRetain in attach()
        captureQueue = nullptr;
    }
}

OSStatus AppAudioWorker::ioProcTrampoline(AudioObjectID, const AudioTimeStamp*, const AudioBufferList* inInputData, const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* clientData)
{
    auto* self = static_cast<AppAudioWorker*>(clientData);
    if (self == nullptr || inInputData == nullptr || inInputData->mNumberBuffers == 0)
        return noErr;

    const AudioBuffer& b = inInputData->mBuffers[0];
    const UInt32 chans = std::max<UInt32>(1, b.mNumberChannels);
    const int frames = (int)(b.mDataByteSize / sizeof(float) / chans);
    self->ioBlock(inInputData, frames);
    return noErr;
}

void AppAudioWorker::ioBlock(const AudioBufferList* input, int numFrames)
{
    if (numFrames <= 0 || input == nullptr || input->mNumberBuffers == 0)
        return;

    const AudioBuffer& buf = input->mBuffers[0];
    const auto* interleaved = static_cast<const float*>(buf.mData);
    const int srcChans = (int)buf.mNumberChannels;
    if (interleaved == nullptr || srcChans <= 0)
        return;
    if ((int)deinterleave.size() < numFrames)
        return; // pre-sized in attach(); guard rather than allocate on the RT thread

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const int useCh = ch < srcChans ? ch : srcChans - 1; // clamp if tap is mono
        for (int i = 0; i < numFrames; ++i)
            deinterleave[(size_t)i] = interleaved[(size_t)(i * srcChans + useCh)];

        auto& src = *inputSRCs[(size_t)ch];
        src.pushInput(deinterleave.data(), numFrames);
        while (true)
        {
            const int produced = src.pullOutput(scratchEngine.data(), (int)scratchEngine.size());
            if (produced <= 0)
                break;
            const size_t written = inputRings[(size_t)ch].write(scratchEngine.data(), (size_t)produced);
            if (written < (size_t)produced)
                inputOverruns.fetch_add(1, std::memory_order_relaxed);
            if (produced < (int)scratchEngine.size())
                break;
        }
    }

    framesProduced.fetch_add((uint64_t)numFrames, std::memory_order_relaxed);
    if (auto* ev = inputReadyEvent.load(std::memory_order_acquire))
        ev->signal();
}
} // namespace dcr

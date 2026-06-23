# App Audio Input Implementation Plan — Part 2: CoreAudio capture

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the two CoreAudio-linked units that capture a specific app's
audio — `AppAudioProcesses` (process registry + watcher) and `AppAudioWorker`
(process tap → private aggregate device → IOProc → SRC → rings) — mirroring the
existing `DeviceWorker` so Part 3 can wire them into the engine unchanged.

**Architecture:** Objective-C++ (`.mm`) over the macOS 14.4+ Core Audio process-tap
API, grounded in the verified [AudioCap](https://github.com/insidegui/AudioCap)
call sequence. `AppAudioWorker` reuses the codebase's `SampleRateConverter` and
`FloatRingBuffer` exactly as `DeviceWorker` does, and exposes the same surface
(`getInputRing`, `getNumInputChannels`, `setInputReadyEvent`). Capture slots are
allocated in `open()`; the live tap is bound/unbound by `attach()`/`detach()`
(spec §6 slot-vs-attach decoupling).

**Tech Stack:** Objective-C++, Core Audio (`AudioHardwareCreateProcessTap`,
`CATapDescription`, `AudioHardwareCreateAggregateDevice`,
`AudioDeviceCreateIOProcIDWithBlock`), AudioToolbox (`AudioConverter` via the
existing `SampleRateConverter`), Foundation (`NSRunningApplication`,
`NSWorkspace`), CMake explicit `target_sources`.

---

## Verification reality (read first — honesty)

- **This machine is macOS 26.3 (Darwin 25.3.0), ≥ 14.4**, so the tap API headers
  exist and **every step's `cmake --build` is a real compile-time check here** —
  it catches wrong selectors/types immediately. Lean on the build checkpoints.
- **Process enumeration (Task 2) is runtime-verifiable here** — listing audio
  processes needs no special permission.
- **Tap capture (Task 3) runtime behavior is NOT verifiable from here.** Creating a
  tap triggers the `kTCCServiceAudioCapture` permission prompt, and TCC keys on the
  *app bundle*; a CLI probe is typically denied. Real capture (audio correctness,
  mute behavior, attenuation, drift) is verified in **Part 3**, inside the signed
  `D-Router.app`, on a real listening test. Task 3 here delivers code that
  **compiles cleanly and follows the verified AudioCap sequence**; its runtime
  correctness is explicitly deferred and must not be claimed as verified.
- Several runtime-dependent details are marked **[tune on device]** — implement as
  written, then adjust against live `OSStatus`/`AudioStreamBasicDescription` in
  Part 3.

This plan refines spec §14: the permission gate is the TCC service
**`kTCCServiceAudioCapture`** (triggered by tap creation), not an
`NSAudioCaptureUsageDescription` Info.plist key — AudioCap references no such key.

## File structure (Part 2)

- **Create** `Source/Engine/AppAudioProcesses.h` / `.mm` — message-thread process
  registry: enumerate tappable processes, resolve bundle-id → process AudioObjectID,
  watch launch/quit via `NSWorkspace`. (spec §8)
- **Create** `Source/Engine/AppAudioWorker.h` / `.mm` — one app capture slot:
  tap + private aggregate device + IOProc + per-channel SRC + rings. (spec §5)
- **Modify** `CMakeLists.txt` — add the two `.mm` files to the `dcorerouter`
  `target_sources` (explicit list; no glob).

Reference patterns already in the tree (read them):
`Source/Engine/DeviceWorker.{h,cpp}` (worker shape, IOProc→SRC→ring→signal loop),
`Source/Engine/SampleRateConverter.h` (SRC API), `Source/Engine/RingBuffer.h`
(`FloatRingBuffer`), `Source/Engine/SystemAudioDevices.cpp` (raw CoreAudio C API
from a source file), `Source/Engine/EngineSettings.h` (ring/SRC sizing fields).

---

### Task 1: CMake wiring + compilable skeletons

Get both files compiling into the app as empty shells before adding logic, so every
later step has a green baseline.

**Files:**
- Create: `Source/Engine/AppAudioProcesses.h`, `Source/Engine/AppAudioProcesses.mm`
- Create: `Source/Engine/AppAudioWorker.h`, `Source/Engine/AppAudioWorker.mm`
- Modify: `CMakeLists.txt` (the `dcorerouter` `target_sources` block)

- [ ] **Step 1: Create the four skeleton files**

`Source/Engine/AppAudioProcesses.h`:

```cpp
#pragma once

#include <CoreAudio/CoreAudio.h>

#include <functional>
#include <string>
#include <vector>

namespace dcr
{
    // Message-thread-only registry of tappable audio processes (macOS 14.4+).
    // Enumerates processes that can be captured, resolves a bundle id to the
    // current CoreAudio process-object id, and notifies on launch/quit so the
    // engine can auto-reattach.  No realtime involvement.
    class AppAudioProcesses
    {
    public:
        struct Entry
        {
            AudioObjectID processObject = kAudioObjectUnknown;
            int pid = -1;
            std::string bundleId;
            std::string displayName;
            bool runningOutput = false; // currently producing output audio
        };

        AppAudioProcesses();
        ~AppAudioProcesses();

        // Snapshot of processes right now (message thread).
        std::vector<Entry> enumerate() const;

        // bundle id -> current process-object id, or kAudioObjectUnknown.
        AudioObjectID resolve (const std::string& bundleId) const;

        // Called on the message thread whenever the running-app set changes.
        std::function<void()> onProcessesChanged;

    private:
        void* observer = nullptr; // NSWorkspace notification token (opaque here)
    };
} // namespace dcr
```

`Source/Engine/AppAudioProcesses.mm`:

```cpp
#include "Engine/AppAudioProcesses.h"

namespace dcr
{
    AppAudioProcesses::AppAudioProcesses() = default;
    AppAudioProcesses::~AppAudioProcesses() = default;

    std::vector<AppAudioProcesses::Entry> AppAudioProcesses::enumerate() const
    {
        return {};
    }

    AudioObjectID AppAudioProcesses::resolve (const std::string&) const
    {
        return kAudioObjectUnknown;
    }
} // namespace dcr
```

`Source/Engine/AppAudioWorker.h`:

```cpp
#pragma once

#include <juce_core/juce_core.h>

#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <memory>
#include <vector>

#include "Engine/EngineSettings.h"
#include "Engine/RingBuffer.h"
#include "Engine/SampleRateConverter.h"

namespace dcr
{
    // One app-audio capture slot.  Mirrors DeviceWorker's outward surface so the
    // matrix can't tell them apart: getInputRing/getNumInputChannels/
    // setInputReadyEvent.  open() allocates the rings (the slot); attach()/detach()
    // bind/unbind the live process tap (spec §6).  The IOProc runs on a CoreAudio
    // HAL thread under the same RT rules as DeviceWorker's callback.
    class AppAudioWorker
    {
    public:
        AppAudioWorker (bool muteOriginalOutput, int numChannels);
        ~AppAudioWorker();

        // Allocate rings + scratch for `numChannels`.  Detached (no tap yet).
        bool open (const EngineSettings& settings);
        void close();

        // Bind a live process tap (message thread).  Reads the tap format, prepares
        // the per-channel SRC, builds the private aggregate device, starts the IOProc.
        bool attach (AudioObjectID processObject);
        void detach();
        bool isAttached() const noexcept { return attached.load (std::memory_order_acquire); }

        int getNumInputChannels() const noexcept { return numChannels; }
        FloatRingBuffer* getInputRing (int ch) noexcept
        {
            return ch >= 0 && ch < (int) inputRings.size() ? &inputRings[(size_t) ch] : nullptr;
        }
        void setInputReadyEvent (juce::WaitableEvent* e) noexcept
        {
            inputReadyEvent.store (e, std::memory_order_release);
        }
        uint64_t getInputOverruns() const noexcept { return inputOverruns.load (std::memory_order_relaxed); }

    private:
        void ioBlock (const AudioBufferList* input, int numFrames); // HAL thread

        const bool muteOriginalOutput;
        const int numChannels;
        EngineSettings settings {};

        std::vector<FloatRingBuffer> inputRings;
        std::vector<std::unique_ptr<SampleRateConverter>> inputSRCs;
        std::vector<float> scratchEngine;
        std::vector<float> deinterleave; // tap delivers interleaved stereo

        std::atomic<juce::WaitableEvent*> inputReadyEvent { nullptr };
        std::atomic<bool> attached { false };
        std::atomic<uint64_t> inputOverruns { 0 };

        // CoreAudio handles (message thread owns lifecycle; HAL thread reads).
        AudioObjectID tapId = kAudioObjectUnknown;
        AudioObjectID aggregateId = kAudioObjectUnknown;
        AudioDeviceIOProcID procId = nullptr;
        double tapSampleRate = 0.0;
    };
} // namespace dcr
```

`Source/Engine/AppAudioWorker.mm`:

```cpp
#include "Engine/AppAudioWorker.h"

namespace dcr
{
    AppAudioWorker::AppAudioWorker (bool mute, int nch)
        : muteOriginalOutput (mute), numChannels (nch) {}
    AppAudioWorker::~AppAudioWorker() { close(); }

    bool AppAudioWorker::open (const EngineSettings& s) { settings = s; return true; }
    void AppAudioWorker::close() {}
    bool AppAudioWorker::attach (AudioObjectID) { return false; }
    void AppAudioWorker::detach() {}
    void AppAudioWorker::ioBlock (const AudioBufferList*, int) {}
} // namespace dcr
```

- [ ] **Step 2: Add both `.mm` files to CMake**

In `CMakeLists.txt`, find the `dcorerouter` `target_sources(... PRIVATE ...)` block
(it already lists `Source/Engine/DeviceWorker.cpp`, `Source/DSP/PluginHost.mm`,
etc.). Add, next to the other `Source/Engine/*` entries:

```cmake
    Source/Engine/AppAudioProcesses.mm
    Source/Engine/AppAudioWorker.mm
```

- [ ] **Step 3: Build the app to verify the skeletons compile and link**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
Expected: builds to `build/dcorerouter_artefacts/Release/D-Router.app` with no errors.
(First reconfigure picks up the new sources.)

- [ ] **Step 4: Commit**

```bash
git add Source/Engine/AppAudioProcesses.h Source/Engine/AppAudioProcesses.mm \
        Source/Engine/AppAudioWorker.h Source/Engine/AppAudioWorker.mm CMakeLists.txt
git commit -m "feat(appinput): AppAudioProcesses + AppAudioWorker skeletons + CMake wiring

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: AppAudioProcesses — enumerate, resolve, watch

Message-thread CoreAudio + Foundation. Runtime-verifiable on this machine.

**Files:**
- Modify: `Source/Engine/AppAudioProcesses.mm`

- [ ] **Step 1: Implement `enumerate()` and `resolve()`**

Replace the `.mm` stubs. The process-list read mirrors the
`AudioObjectGetPropertyDataSize` + `AudioObjectGetPropertyData` pattern already in
`Source/Engine/SystemAudioDevices.cpp`. Verified selectors:
`kAudioHardwarePropertyProcessObjectList`, `kAudioProcessPropertyPID`,
`kAudioProcessPropertyBundleID`, `kAudioProcessPropertyIsRunningOutput`.

```cpp
#include "Engine/AppAudioProcesses.h"

#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h> // NSRunningApplication, NSWorkspace

namespace
{
    // Read an array of AudioObjectIDs from a global hardware property.
    std::vector<AudioObjectID> readObjectList (AudioObjectPropertySelector sel)
    {
        AudioObjectPropertyAddress addr {
            sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize (kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
            return {};
        std::vector<AudioObjectID> ids (size / sizeof (AudioObjectID));
        if (ids.empty())
            return {};
        if (AudioObjectGetPropertyData (kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
            return {};
        return ids;
    }

    int readPid (AudioObjectID proc)
    {
        AudioObjectPropertyAddress addr {
            kAudioProcessPropertyPID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        pid_t pid = -1;
        UInt32 size = sizeof (pid);
        AudioObjectGetPropertyData (proc, &addr, 0, nullptr, &size, &pid);
        return (int) pid;
    }

    std::string readBundleId (AudioObjectID proc)
    {
        AudioObjectPropertyAddress addr {
            kAudioProcessPropertyBundleID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        CFStringRef cf = nullptr;
        UInt32 size = sizeof (cf);
        if (AudioObjectGetPropertyData (proc, &addr, 0, nullptr, &size, &cf) != noErr || cf == nullptr)
            return {};
        std::string out;
        if (const char* c = CFStringGetCStringPtr (cf, kCFStringEncodingUTF8))
            out = c;
        else
        {
            char buf[512] = { 0 };
            if (CFStringGetCString (cf, buf, sizeof (buf), kCFStringEncodingUTF8))
                out = buf;
        }
        CFRelease (cf);
        return out;
    }

    bool readRunningOutput (AudioObjectID proc)
    {
        AudioObjectPropertyAddress addr {
            kAudioProcessPropertyIsRunningOutput, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain
        };
        UInt32 v = 0, size = sizeof (v);
        AudioObjectGetPropertyData (proc, &addr, 0, nullptr, &size, &v);
        return v != 0;
    }
} // namespace

namespace dcr
{
    std::vector<AppAudioProcesses::Entry> AppAudioProcesses::enumerate() const
    {
        std::vector<Entry> out;
        for (AudioObjectID proc : readObjectList (kAudioHardwarePropertyProcessObjectList))
        {
            Entry e;
            e.processObject = proc;
            e.pid = readPid (proc);
            e.bundleId = readBundleId (proc);
            e.runningOutput = readRunningOutput (proc);

            if (e.pid > 0)
                if (NSRunningApplication* app =
                        [NSRunningApplication runningApplicationWithProcessIdentifier: e.pid])
                    if (NSString* n = app.localizedName)
                        e.displayName = n.UTF8String;

            if (! e.bundleId.empty())
                out.push_back (std::move (e));
        }
        return out;
    }

    AudioObjectID AppAudioProcesses::resolve (const std::string& bundleId) const
    {
        for (const auto& e : enumerate())
            if (e.bundleId == bundleId)
                return e.processObject;
        return kAudioObjectUnknown;
    }
} // namespace dcr
```

- [ ] **Step 2: Implement the launch/quit watcher (ctor/dtor)**

Match AudioCap: observe `NSWorkspace.runningApplications` changes. Use the
`NSWorkspace` notification center for app launch/terminate and fire
`onProcessesChanged` on the main thread.

```cpp
// add near the top of the .mm, after the imports
#include <utility>

namespace dcr
{
    AppAudioProcesses::AppAudioProcesses()
    {
        NSNotificationCenter* nc = [[NSWorkspace sharedWorkspace] notificationCenter];
        auto handler = ^(NSNotification*) {
            if (onProcessesChanged)
                onProcessesChanged();
        };
        id tokenLaunch = [nc addObserverForName: NSWorkspaceDidLaunchApplicationNotification
                                         object: nil
                                          queue: [NSOperationQueue mainQueue]
                                     usingBlock: handler];
        id tokenQuit = [nc addObserverForName: NSWorkspaceDidTerminateApplicationNotification
                                       object: nil
                                        queue: [NSOperationQueue mainQueue]
                                   usingBlock: handler];
        // Retain both tokens; store as a CFArray-ish opaque holder.
        observer = (void*) CFBridgingRetain (@[ tokenLaunch, tokenQuit ]);
    }

    AppAudioProcesses::~AppAudioProcesses()
    {
        if (observer != nullptr)
        {
            NSArray* tokens = (__bridge_transfer NSArray*) observer;
            NSNotificationCenter* nc = [[NSWorkspace sharedWorkspace] notificationCenter];
            for (id t in tokens)
                [nc removeObserver: t];
            observer = nullptr;
        }
    }
} // namespace dcr
```

> Note: `handler` captures `this` implicitly via `onProcessesChanged`. Capture
> `this` explicitly in the block and ensure the owner outlives the observer (it
> does — Part 3 owns one `AppAudioProcesses` for the engine's lifetime). **[tune on
> device]** if the block-capture semantics need a weak holder.

- [ ] **Step 3: Build to verify it compiles**

Run: `cmake --build build -j`
Expected: compiles clean (real check on this macOS 26 machine).

- [ ] **Step 4: Runtime-verify enumeration on this machine**

Add a temporary `DBG` dump behind a debug flag, or a throwaway one-off: in any
message-thread init you can reach (or a scratch `main`), call `enumerate()` and
print each `displayName`/`bundleId`/`runningOutput`. Launch the app (or probe),
confirm currently-playing apps (e.g. a browser playing audio) appear with correct
bundle ids. This needs **no special permission**. Remove the temporary dump after.

Expected: a list including the bundle ids of apps currently producing audio.

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/AppAudioProcesses.mm
git commit -m "feat(appinput): process enumeration/resolve + NSWorkspace launch-quit watcher

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: AppAudioWorker — tap, aggregate, IOProc, SRC, rings

The big one. Code follows the **verified AudioCap sequence**; runtime correctness is
**deferred to Part 3** (TCC + real listening). Each step ends with a build checkpoint
(real on this machine).

**Files:**
- Modify: `Source/Engine/AppAudioWorker.mm`

- [ ] **Step 1: Implement `open()` / `close()` (slot allocation, rate-independent)**

Rings hold *engine-rate* samples (post-SRC), so they size off engine multipliers and
do not depend on the tap rate (unknown until `attach()`). The SRC is prepared in
`attach()` once the tap ASBD is read.

```cpp
bool AppAudioWorker::open (const EngineSettings& s)
{
    settings = s;
    const int eb = settings.engineBlockSize;

    // Engine-rate ring: a few engine blocks + headroom for IOProc bursts.
    // (Device-buffer term is approximated; the tap buffer size is small and the
    // ratio is ~1 since app audio is 44.1/48k against a 48k engine. [tune on device])
    size_t ringSize = (size_t) std::max (settings.inputRingMultEng * eb,
        settings.inputRingMultDev * eb);
    ringSize = std::min (ringSize, (size_t) (256 * 1024));

    inputRings.clear();
    inputRings.resize ((size_t) numChannels);
    for (auto& r : inputRings)
        r.resize (ringSize);

    inputSRCs.clear();
    for (int i = 0; i < numChannels; ++i)
        inputSRCs.push_back (std::make_unique<SampleRateConverter>());

    // Scratch: worst-case SRC output for one IOProc block + 25% headroom.
    const int scratch = (int) std::ceil (1.25 * (double) std::max (eb, 4096));
    scratchEngine.assign ((size_t) scratch, 0.0f);
    deinterleave.assign ((size_t) scratch, 0.0f);
    return true;
}

void AppAudioWorker::close()
{
    detach();
    inputRings.clear();
    inputSRCs.clear();
}
```

Run: `cmake --build build -j` → clean.

- [ ] **Step 2: Implement `attach()` — tap + format + SRC + aggregate + IOProc**

Translate the verified AudioCap sequence to Objective-C++. `CATapDescription` is an
ObjC class (hence `.mm`).

```cpp
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <Foundation/Foundation.h>

bool AppAudioWorker::attach (AudioObjectID processObject)
{
    if (attached.load (std::memory_order_acquire))
        return true;
    if (processObject == kAudioObjectUnknown)
        return false;

    // 1) Tap description: stereo mixdown of the one process; mute behavior per spec.
    CATapDescription* desc =
        [[CATapDescription alloc] initStereoMixdownOfProcesses: @[ @(processObject) ]];
    desc.UUID = [NSUUID UUID];
    desc.muteBehavior = muteOriginalOutput ? CATapMutedWhenTapped : CATapUnmuted;

    // 2) Create the process tap.
    AudioObjectID newTap = kAudioObjectUnknown;
    if (AudioHardwareCreateProcessTap (desc, &newTap) != noErr || newTap == kAudioObjectUnknown)
        return false; // [tune on device] map OSStatus; a permission denial lands here

    // 3) Read the tap's stream format (sample rate + channel count).
    AudioStreamBasicDescription asbd {};
    {
        AudioObjectPropertyAddress a { kAudioTapPropertyFormat,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 size = sizeof (asbd);
        if (AudioObjectGetPropertyData (newTap, &a, 0, nullptr, &size, &asbd) != noErr)
        {
            AudioHardwareDestroyProcessTap (newTap);
            return false;
        }
    }
    tapSampleRate = asbd.mSampleRate > 0 ? asbd.mSampleRate : settings.engineSampleRate;

    // 4) Prepare per-channel SRC: tap rate -> engine rate (mirrors DeviceWorker).
    for (auto& src : inputSRCs)
    {
        src->reset();
        src->prepare (tapSampleRate, settings.engineSampleRate,
            settings.srcQuality, settings.srcComplexity);
    }
    for (auto& r : inputRings)
        r.clear();

    // 5) Private aggregate device wrapping the tap (verified AudioCap key set).
    //    Anchor to the current default output device for clocking + drift comp.
    NSString* outputUID = /* default output device UID */ nil; // [tune on device] from SystemAudioDevices
    NSString* aggUID = [[NSUUID UUID] UUIDString];
    NSDictionary* description = @{
        @(kAudioAggregateDeviceNameKey): [NSString stringWithFormat: @"DRouter-Tap-%u", processObject],
        @(kAudioAggregateDeviceUIDKey): aggUID,
        @(kAudioAggregateDeviceMainSubDeviceKey): (outputUID ?: @""),
        @(kAudioAggregateDeviceIsPrivateKey): @YES,
        @(kAudioAggregateDeviceIsStackedKey): @NO,
        @(kAudioAggregateDeviceTapAutoStartKey): @YES,
        @(kAudioAggregateDeviceSubDeviceListKey): (outputUID ? @[ @{ @(kAudioSubDeviceUIDKey): outputUID } ] : @[]),
        @(kAudioAggregateDeviceTapListKey): @[ @{
            @(kAudioSubTapDriftCompensationKey): @YES,
            @(kAudioSubTapUIDKey): desc.UUID.UUIDString
        } ]
    };

    AudioObjectID newAgg = kAudioObjectUnknown;
    if (AudioHardwareCreateAggregateDevice ((__bridge CFDictionaryRef) description, &newAgg) != noErr)
    {
        AudioHardwareDestroyProcessTap (newTap);
        return false;
    }

    // 6) IOProc block: tap audio arrives in `inInputData`.  RT thread -- no alloc/lock.
    AudioDeviceIOProcID newProc = nullptr;
    AudioObjectID aggForBlock = newAgg;
    OSStatus e = AudioDeviceCreateIOProcIDWithBlock (&newProc, newAgg, nullptr,
        ^(const AudioTimeStamp*, const AudioBufferList* inInputData, const AudioTimeStamp*,
            AudioBufferList*, const AudioTimeStamp*) {
            // mDataByteSize / (channels * sizeof(float)) -> frames. [tune on device]
            if (inInputData != nullptr && inInputData->mNumberBuffers > 0)
            {
                const AudioBuffer& b = inInputData->mBuffers[0];
                const int frames = (int) (b.mDataByteSize / sizeof (float) / (UInt32) std::max (1u, b.mNumberChannels));
                ioBlock (inInputData, frames);
            }
        });
    if (e != noErr || newProc == nullptr)
    {
        AudioHardwareDestroyAggregateDevice (newAgg);
        AudioHardwareDestroyProcessTap (newTap);
        return false;
    }

    // 7) Publish handles, then start, then flip attached (release) last (spec §6/§7 ordering).
    tapId = newTap;
    aggregateId = newAgg;
    procId = newProc;
    if (AudioDeviceStart (newAgg, newProc) != noErr)
    {
        detach();
        return false;
    }
    attached.store (true, std::memory_order_release);
    (void) aggForBlock;
    return true;
}
```

> **[tune on device]** the `outputUID` (default output device UID via the existing
> `SystemAudioDevices` helper), the frames-per-block derivation from the
> `AudioBufferList`, and the interleaved-vs-planar layout of the tap buffer — all
> finalized against the live ASBD in Part 3. A tap-only aggregate (empty sub-device
> list) is the fallback if anchoring to an output device proves brittle.

Run: `cmake --build build -j` → clean.

- [ ] **Step 3: Implement `ioBlock()` — deinterleave → SRC → ring → signal**

Mirrors `DeviceWorker`'s callback (push → pull-until-drained → write ring →
overruns → signal), but the tap delivers one interleaved stereo buffer, so split it
into per-channel planar first.

```cpp
void AppAudioWorker::ioBlock (const AudioBufferList* input, int numFrames)
{
    if (numFrames <= 0 || input == nullptr || input->mNumberBuffers == 0)
        return;

    const AudioBuffer& buf = input->mBuffers[0];
    const auto* interleaved = static_cast<const float*> (buf.mData);
    const int srcChans = (int) buf.mNumberChannels;
    if (interleaved == nullptr || srcChans <= 0)
        return;

    if ((int) deinterleave.size() < numFrames)
        return; // pre-sized in open(); guard rather than allocate

    for (int ch = 0; ch < numChannels; ++ch)
    {
        // Pull this output channel from the interleaved tap buffer (clamp if mono).
        const int useCh = ch < srcChans ? ch : srcChans - 1;
        for (int i = 0; i < numFrames; ++i)
            deinterleave[(size_t) i] = interleaved[(size_t) (i * srcChans + useCh)];

        auto& src = *inputSRCs[(size_t) ch];
        src.pushInput (deinterleave.data(), numFrames);
        while (true)
        {
            const int produced = src.pullOutput (scratchEngine.data(), (int) scratchEngine.size());
            if (produced <= 0)
                break;
            const size_t written = inputRings[(size_t) ch].write (scratchEngine.data(), (size_t) produced);
            if (written < (size_t) produced)
                inputOverruns.fetch_add (1, std::memory_order_relaxed);
            if (produced < (int) scratchEngine.size())
                break;
        }
    }

    if (auto* ev = inputReadyEvent.load (std::memory_order_acquire))
        ev->signal();
}
```

Run: `cmake --build build -j` → clean.

- [ ] **Step 4: Implement `detach()` — verified teardown order**

```cpp
void AppAudioWorker::detach()
{
    attached.store (false, std::memory_order_release); // matrix stops consuming first (spec §7)

    if (aggregateId != kAudioObjectUnknown && procId != nullptr)
    {
        AudioDeviceStop (aggregateId, procId);
        AudioDeviceDestroyIOProcID (aggregateId, procId);
    }
    if (aggregateId != kAudioObjectUnknown)
        AudioHardwareDestroyAggregateDevice (aggregateId);
    if (tapId != kAudioObjectUnknown)
        AudioHardwareDestroyProcessTap (tapId);

    procId = nullptr;
    aggregateId = kAudioObjectUnknown;
    tapId = kAudioObjectUnknown;
    tapSampleRate = 0.0;
}
```

Run: `cmake --build build -j` → clean.

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/AppAudioWorker.mm Source/Engine/AppAudioWorker.h
git commit -m "feat(appinput): AppAudioWorker process-tap capture (tap+aggregate+IOProc+SRC)

Follows the verified AudioCap sequence. Compiles on macOS 26; runtime
capture (TCC + listening) verified in Part 3 inside the signed app.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Real-device test checklist (executed in Part 3, inside D-Router.app)

Not verifiable from this plan; record honestly when run:
- [ ] First tap creation prompts for `kTCCServiceAudioCapture`; granting it sticks.
- [ ] Capturing a stereo app (e.g. a browser) yields correct, in-sync stereo audio.
- [ ] `muteOriginalOutput=true` mutes the app's normal output; `false` passes through.
- [ ] SRC handles a 44.1 kHz app into the 48 kHz engine with no artefacts.
- [ ] Multichannel source attenuation (spec §13) — measure and note.
- [ ] Quit→relaunch the app: detach then re-attach, no glitch on other audio.
- [ ] Two apps captured at once stay independent.

## Self-review

**Spec coverage (Part 2):** §5 AppAudioWorker → Task 3; §8 process registry/watcher
→ Tasks 1–2; §14 permission detail refined (kTCCServiceAudioCapture) → noted, full
handling in Part 3. §6 slot/attach + §7 ordering honored in attach/detach. Deferred
to Part 3 (with mapping): §4 data model, §6/§7 matrix wiring, §10–§13, §15–§16.

**Placeholder scan:** no TBD/TODO. Items marked **[tune on device]** are explicit,
bounded runtime-tuning callouts (output UID, ASBD frame/layout, block-capture
semantics), not vague placeholders — each names exactly what to finalize and where.

**Type consistency:** `AppAudioWorker` surface (`open`/`close`/`attach`/`detach`/
`isAttached`/`getInputRing`/`getNumInputChannels`/`setInputReadyEvent`/
`getInputOverruns`) is consistent header↔impl and matches what Part 3's engine
wiring will call (the `DeviceWorker` shape). `AppAudioProcesses::Entry` fields
(`processObject`/`pid`/`bundleId`/`displayName`/`runningOutput`) are consistent and
feed the Part 1 `reconcile()` inputs (bundleId + process id).

## Next

Part 3 (engine wiring + Soft-In groups + UI + persistence + permission gating)
gets its own writing-plans pass; it's where these classes are exercised and the
real-device checklist above is run.

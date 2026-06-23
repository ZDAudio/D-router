# App Audio Input — Design

**Date:** 2026-06-23
**Status:** Design (approved in brainstorming, pending spec review)
**Branch:** `feat/app-audio-input`

## 1. Problem & motivation

Today the only way to feed *software* audio (a browser, a music app, a game) into
D-Router is the BlackHole dance documented in the README: install a third-party
virtual driver, point macOS's system output at BlackHole, then add BlackHole as
an input device — and set up a multi-output device if you still want to hear
anything. It works, but it is high-friction, all-or-nothing (system-wide), and
unfriendly to non-experts.

macOS 14.4+ exposes **Core Audio process taps**
(`AudioHardwareCreateProcessTap` + `CATapDescription`, wrapped in a private
aggregate device) which can capture a *specific application's* output with no
virtual driver — and can do what BlackHole fundamentally can't: capture **one
app at a time** (just Chrome, just a game). This design adds native per-app audio
capture as first-class input sources, Audio-Hijack style.

## 2. Goals / non-goals

**Goals**
- Capture a specific application's audio output as an input source (per-process tap).
- Multiple apps captured simultaneously, each an independent input source.
- Per-source "mute original output" toggle, **default muted** (avoid double audio
  in routing scenarios).
- Persist sources by **bundle identifier** and **auto-reattach** when the app is
  running — even if it launches after D-Router, or quits and relaunches.
- Each app source automatically forms a **"Soft In" input group** (stereo), so it
  inherits the group fader (VCA/Router), the 5-slot multichannel plugin chain, and
  mute/solo for free.

**Non-goals (v1)**
- Native **multichannel** app capture (5.1/7.1/Atmos). v1 captures a **stereo
  mixdown** per app. See §13. BlackHole 16ch remains the documented path for
  spatial audio.
- Support below macOS 14.4. The feature is gated; older systems keep the BlackHole
  workaround.
- Capturing non-audio or input/microphone streams. This is about app *output*.

## 3. Chosen approach (Approach A)

A new **`AppAudioWorker`** class, parallel to the existing `DeviceWorker`, owns a
raw Core Audio process-tap aggregate device. It exposes the same surface the
matrix already consumes (`getInputRing(ch)`), so `MatrixProcessor` cannot tell an
app source from a hardware device. The JUCE `AudioIODevice` path in `DeviceWorker`
stays untouched and pure; all raw CoreAudio is isolated in the new worker.

Rejected alternatives: extending `DeviceWorker` with a source-type discriminator
(turns it into a god-class mixing JUCE HAL and raw CoreAudio — violates the
"small, focused unit" principle and complicates RT reasoning); exposing the tap
aggregate as a non-private system device for JUCE to open (pollutes other apps'
device lists, and per-source mute / dynamic re-attach are tap properties JUCE's
abstraction can't reach).

## 4. Data model

A new lightweight struct, peer to `AudioEngine::DeviceSpec` (`AudioEngine.h:74`),
**not** merged into `DeviceSpec`:

```cpp
struct AppInputSpec
{
    juce::String bundleId;                    // "com.google.Chrome" — stable persistence key
    juce::String displayName;                 // cached friendly name, shown while offline
    bool         muteOriginalOutput = true;   // default: hijack
    int          numChannels = 2;             // stereo mixdown; v1 fixed at 2
    // PID / process AudioObjectID are resolved at runtime, NEVER persisted.
};
```

Each app source contributes **2 consecutive global input channels** (L/R), folded
into the global input channel space exactly like a hardware device (the mapping
built around `AudioEngine.cpp:114`).

A new **group category** is added to the shared group struct `OutputGroup`
(`OutputGroup.h:31` — the type is reused for both input and output groups):

```cpp
enum class Kind { Regular, SoftIn };
std::atomic<Kind> kind { Kind::Regular };
```

## 5. `AppAudioWorker` (new: `Source/Engine/AppAudioWorker.{h,cpp}`)

Mirrors `DeviceWorker`'s outward surface (most importantly `getInputRing(ch)`).
Internals:

- **`attach(processObjectID)`** (message thread): build a `CATapDescription`
  (stereo mixdown of the target process; mute behavior per
  `muteOriginalOutput`) → `AudioHardwareCreateProcessTap` → create a **private**
  aggregate device wrapping the tap → register an IOProc → start.
- **`detach()`** (message thread): stop the IOProc, destroy the aggregate device,
  destroy the tap. **Ring buffers are NOT freed.**
- **IOProc** (runs on a CoreAudio HAL thread — same RT rules: no alloc, no locks,
  no blocking, no I/O): captured samples → per-channel AudioToolbox SRC (tap rate
  → engine rate, reusing the converter path at `DeviceWorker.cpp:252`) → write
  `FloatRingBuffer` → signal `inputReadyEvent`.

Mute behavior maps to `CATapDescription`'s mute setting: muted ≈
`CATapMutedWhenTapped` (app silent on its original device while captured),
unmuted = passthrough. (Exact enum constants confirmed at implementation — see
§14.)

## 6. Slot vs. attach decoupling (the key idea)

Two lifetimes are deliberately separated:

- **Slot** = the worker + its ring buffers + its global channel indices.
  Allocated once, when the user **adds** the app source; lives until the user
  **removes** it.
- **Attach** = whether a tap is currently bound to a live PID. Driven dynamically
  by the process watcher (§7).

`attach()`/`detach()` only start/stop the IOProc and tap. They **do not** change
the global channel mapping, touch the matrix's `GlobalInput` list, or trigger an
engine reconfigure. Consequently an app launching/quitting causes **no** global
audio glitch — the heavyweight reconfigure happens only on explicit add/remove.

## 7. Matrix "app inputs never stall" rule

`MatrixProcessor::tryProcessOneBlock` (`MatrixProcessor.cpp:318`) currently waits
for **every** input ring to hold `blockSize` samples before processing. An offline
app source would never fill → the whole matrix would deadlock and all audio would
stop. Unacceptable.

Add `std::atomic<bool> attached` to `GlobalInput` (`MatrixProcessor.h:33`) and a
new rule for app inputs:

- **Availability check:** app inputs do **not** participate in the "wait for
  blockSize" gate.
- **Read:** if `!attached` *or* the ring holds fewer than `blockSize` samples, that
  input's block is `memset` to silence — the ring is not read and the matrix is
  not stalled.
- Hardware inputs keep their existing blocking semantics unchanged.

Memory ordering (attach/detach are both message-thread, hence serialized):
- **attach:** ring is currently unconsumed (matrix is memset-ing it) → clear ring →
  start IOProc → set `attached = true` (release) last.
- **detach:** set `attached = false` first → then stop IOProc.

This breaks no gain-staging or meter invariant: plugin chains / input groups run on
silence harmlessly, the mix adds zero, and the input meter reads 0 while offline
(correct).

## 8. Process registry & watcher (new: `Source/Engine/AppAudioProcesses.{h,cpp}`)

Message-thread-only CoreAudio helper, modeled on the existing `SystemAudioDevices`
helper (PR #21). Three jobs:

- **Enumerate** (for the picker): `kAudioHardwarePropertyProcessObjectList` → per
  process: PID (`kAudioProcessPropertyPID`), bundle ID
  (`kAudioProcessPropertyBundleID`), is-producing-output
  (`kAudioProcessPropertyIsRunningOutput`); cross-reference `NSRunningApplication`
  for display name + icon. The picker defaults to "apps currently producing
  audio."
- **Resolve:** bundle ID → current process AudioObjectID (scan the process list).
- **Watch:** `AudioObjectAddPropertyListener` on the process-object-list property
  plus `NSWorkspace` launch/terminate notifications. On any change, re-resolve every
  configured app source and drive attach/detach.

All message-thread; no RT involvement.

## 9. Auto-reattach state machine (new: JUCE-free, unit-tested)

Extract the **decision** — "given watcher events + configured specs + current
attach states, produce attach/detach commands" — into a pure-logic unit
`AppInputResolver`, the same way `ResonanceMath` / `StereoMeterMath` were pulled
out JUCE-free with tests. The CoreAudio glue (actually building taps) stays in the
worker; the decision is independently testable.

## 10. Engine reconfiguration — reuse the freeze-set

Two distinct paths:

- **Add / remove an app source (heavyweight):** changes the global channel
  mapping and the matrix input list → goes through the existing threaded
  reconfigure that `ReconfigurationController` drives. **The freeze-set invariant
  must hold** — every message-thread reader of engine state must be paused during
  the reconfigure (a missed reader, `PerfMonitor`, previously caused an
  `emitSnapshot` crash; see project memory). **The process watcher must itself
  join the freeze-set**, or it could call `attach()` on a worker mid-rebuild.
- **Auto attach / detach (lightweight):** per §6, no reconfigure. Both paths are
  message-thread (naturally serialized); additionally the watcher is gated while a
  reconfigure runs so it never touches a half-built worker.

## 11. Soft-In group integration

When the user adds an app source, the engine **auto-creates** an input group:
`InputGroupManager::createGroup(displayName, stereo())`
(`InputGroupManager.cpp:64` already supports programmatic creation), assigns the
app's two channels as members, and sets `kind = Kind::SoftIn`. The group inherits
the full group machinery: VCA/Router fader, 5-slot `MultiChannelPluginHost` chain,
mute/solo.

**Ownership:** a Soft-In group's **membership is owned by the app source** — it is
created and destroyed with the source and is **not** member-editable in
`GroupManagerDialog` (unlike regular groups). The user may still adjust its fader,
fader mode, mute, and plugins.

Group taxonomy after this change:
- Output groups (OutputGroupManager)
- Input groups (InputGroupManager, `kind = Regular`)
- **Soft-In groups** (InputGroupManager, `kind = SoftIn`) — new

## 12. UI

- **`DeviceManagerDialog`** (where inputs/outputs are already chosen) gains an
  **"App Audio Inputs"** section: a list of configured sources (name · status dot
  ● attached / ○ offline-waiting · mute toggle · remove ×) and an **"Add App…"**
  button opening a process picker (apps currently producing audio, with icons). On
  macOS < 14.4 the section is disabled with a note pointing to BlackHole.
- **`OutputGroupPanel`** (input side) and **`GroupManagerDialog`** render Soft-In
  groups in their own section / with a distinct badge, peer to regular input
  groups. Soft-In group membership is read-only there.
- **Matrix view:** app inputs appear as ordinary input columns labeled by app
  ("Chrome L / Chrome R"), greyed when offline.

## 13. Multichannel (decision + rationale)

Per-process taps are fundamentally a **mixdown**: the public per-process
initializers fold the app to mono or **stereo**, regardless of the app's native
layout. Native multichannel is not an app-level switch — it comes from tapping a
**device stream** (process + device-UID + stream), so the channel count equals
that output device's stream format; it is materially more complex and couples to
device layout. Field reports also note level attenuation that scales with the
device's channel-pair count (≈ −12 dB at 8 outs).

**v1 decision:** fix **stereo mixdown** per app → each Soft-In group is stereo.
`AppInputSpec.numChannels` leaves the door open, but v1 locks it to 2. True
multichannel app capture is deferred; Atmos/spatial users continue via the
documented **BlackHole 16ch** path.

Sources: [Apple — Capturing system audio with Core Audio taps](https://developer.apple.com/documentation/CoreAudio/capturing-system-audio-with-core-audio-taps),
[AudioCap](https://github.com/insidegui/AudioCap),
[attenuation field notes](https://dev.to/yingzhong_xu_20d6f4c5d4ce/from-core-audio-to-llms-native-macos-audio-capture-for-ai-powered-tools-dkg).

## 14. Permissions, signing & macOS gating

- **14.4 gate:** all tap code behind `if (@available(macOS 14.4, *))` + a runtime
  guard; the feature is hidden/disabled below 14.4.
- **TCC:** the first tap triggers a system audio-capture permission prompt gated on
  the TCC service **`kTCCServiceAudioCapture`** (verified against AudioCap, 2026-06-23).
  No `Info.plist` usage-description key is required (AudioCap references none). Prefer
  the **public path**: attempt tap creation and handle a permission-denied `OSStatus`
  by showing a "permission denied" state with an "Open System Settings" link. The
  private `TCC.framework` SPI (`TCCAccessPreflight`/`TCCAccessRequest`, same service
  string) can pre-check status for nicer UX but is **private API** — optional, flagged
  as a risk.
- **Ad-hoc signing:** D-Router is ad-hoc signed; TCC behavior under ad-hoc signing
  must be **verified on a real device**.
- **Fallback:** the BlackHole README section stays as-is for < 14.4 and for users
  who prefer not to grant permission.

## 15. Persistence

`Snapshot` gains `std::vector<AppInputSpec> appInputs` (parallel to `devices`),
serializing `bundleId / displayName / muteOriginalOutput / numChannels` into the
ValueTree/XML. `Snapshot::Group` (`SnapshotStore.h:34`) gains `int kind`. On load:
recreate workers **detached** and recreate their Soft-In groups → the watcher
resolves and attaches. **PIDs are never stored.** Backward compatible: old
snapshots simply have no `appInputs` node and `kind` defaults to 0 (Regular).

## 16. Invariants honored

- **RT-safety:** the IOProc obeys the same no-alloc/no-lock/no-block/no-I/O rule as
  the CoreAudio callbacks; all scratch is pre-sized at attach time. The matrix's
  new per-input branch reads one atomic and `memset`s a pre-sized buffer.
- **Freeze-set:** the new add/remove path and the watcher both respect the
  reconfigure freeze-set (§10).
- **Gain staging / meter tap:** unchanged — app inputs are ordinary inputs to the
  mix; offline = silence.

## 17. Testing

**JUCE-free unit tests** (`dcorerouter_tests`, per CLAUDE.md):
- `AppInputResolver` state machine: offline→running→attach; running→quit→detach;
  dedup; D-Router-starts-before-app; etc.
- `AppInputSpec` (and `Group.kind`) persistence round-trip.
- The matrix "app input → silence when detached/underfull, never stalls" decision
  (extracted into a testable per-input availability/read function).

**Real-device / manual only** (state honestly as unverified):
- Actual tap capture & audio correctness; mute on/off behavior; SRC quality &
  latency; the TCC prompt; ad-hoc-signing + TCC; auto-reattach timing across
  quit/relaunch; multiple apps captured at once; the multichannel-device
  attenuation note.

## 18. File inventory

**New**
- `Source/Engine/AppAudioWorker.{h,cpp}` — tap aggregate device + IOProc + SRC + rings.
- `Source/Engine/AppAudioProcesses.{h,cpp}` — message-thread process registry/watcher.
- `Source/Engine/AppInputResolver.{h,cpp}` (or header-only) — JUCE-free attach/detach decision.
- Tests in `tests/` for the resolver, persistence, and the matrix silence rule.

**Changed**
- `Source/Engine/AudioEngine.{h,cpp}` — `AppInputSpec`; build app workers; global mapping.
- `Source/Engine/MatrixProcessor.{h,cpp}` — `GlobalInput.attached`; never-stall rule.
- `Source/Routing/OutputGroup.h` — `Kind` enum + field.
- `Source/Routing/InputGroupManager.{h,cpp}` — soft-in group creation/ownership.
- `Source/UI/DeviceManagerDialog.{h,cpp}` — App Audio Inputs section + process picker.
- `Source/UI/OutputGroupPanel.*`, `Source/UI/GroupManagerDialog.*` — Soft-In sectioning.
- `Source/Persistence/SnapshotStore.{h,cpp}` — `appInputs`, `Group.kind`.
- `Source/MainComponent.cpp` — reconfigure path for add/remove; gather/restore.
- `Info.plist` / CMake — usage-description string; (entitlement if needed).

## 19. Open questions / to verify at implementation

1. Exact `CATapDescription` mute-behavior constants and the stereo-mixdown
   initializer signature.
2. **Partly resolved (2026-06-23):** TCC service is `kTCCServiceAudioCapture`, no
   `Info.plist` key needed (per AudioCap). Still to verify on a real device: whether
   tap creation under **ad-hoc signing** prompts and persists the grant correctly.
3. Whether ad-hoc signing keeps TCC grants stable across rebuilds (path/bundle-id
   keyed).
4. SRC/latency characteristics of tap capture vs. hardware input (real-device).

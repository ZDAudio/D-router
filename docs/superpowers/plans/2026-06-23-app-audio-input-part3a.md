# App Audio Input Implementation Plan — Part 3a: engine plumbing + matrix

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire `AppAudioWorker` into `AudioEngine` and `MatrixProcessor` so app-audio
capture slots take their place in the global input space and the matrix applies the
never-stall rule — the plumbing every later part builds on.

**Architecture:** Introduce a thin `InputSource` interface (the ONLY thing the matrix
needs from an input: `getInputRing` + `isAppInput`/`isAttached`). `DeviceWorker` and
`AppAudioWorker` both implement it; `MatrixProcessor::GlobalInput` holds an
`InputSource*` instead of a `DeviceWorker*`. `AudioEngine::start` builds app workers
from an `AppInputSpec` list, appends their channels after the device inputs, and the
matrix uses the Part-1 `planMatrixInput()` to decide read-vs-silence per input.

**Tech Stack:** C++20, JUCE, the existing engine/matrix; no CoreAudio changes here
(that was Part 2).

---

## Scope & verification reality

Part 3a is **pure plumbing**: it compiles and keeps the headless suite green, but it
is **not runtime-verifiable on its own** — there is no way to *add* an app source yet
(that's the minimal picker in Part 3b). Honest completion bar for 3a: **the app
builds and `ctest` stays green**. Real capture is verified in **Part 3b** (add a
source via a minimal picker, grant TCC, listen).

Decomposition of Part 3: **3a** (this — engine/matrix plumbing) · **3b** (live wiring:
`AppAudioProcesses` owner + reconcile + attach/detach + minimal "App Audio Inputs"
picker → first real-device capture verification) · **3c** (Soft-In groups + Snapshot
persistence + per-source mute UI + matrix labeling + TCC-denial UX + 14.4 gating).

Key fact driving the design (verified via codebase grep): `GlobalInput::device` is
used in **exactly 3 places, only** for `getInputRing`/`getOutputRing`
(`MatrixProcessor.cpp:326`, `:341`, `:523`). So the `DeviceWorker*` field can become a
minimal interface pointer with no other fallout.

## File structure (Part 3a)

- **Create** `Source/Engine/InputSource.h` — the interface.
- **Modify** `Source/Engine/DeviceWorker.h` — `: public InputSource`, mark
  `getInputRing` `override`, add `isAppInput()`/`isAttached()`.
- **Modify** `Source/Engine/AppAudioWorker.h` — `: public InputSource`, `override`s.
- **Modify** `Source/Engine/MatrixProcessor.h` — `GlobalInput::device` →
  `InputSource* source`.
- **Modify** `Source/Engine/MatrixProcessor.cpp` — the 2 input call sites + the
  never-stall rule (`planMatrixInput`).
- **Modify** `Source/Engine/AudioEngine.h` — `AppInputSpec`, an `appWorkers` member,
  `start()` overload taking app inputs, accessors.
- **Modify** `Source/Engine/AudioEngine.cpp` — build app workers, extend global input
  mapping, wire `setInputReadyEvent`.

---

### Task 1: `InputSource` interface; both workers implement it

**Files:**
- Create: `Source/Engine/InputSource.h`
- Modify: `Source/Engine/DeviceWorker.h`, `Source/Engine/AppAudioWorker.h`
- Modify: `Source/Engine/MatrixProcessor.h`, `Source/Engine/MatrixProcessor.cpp`

- [ ] **Step 1: Create the interface**

`Source/Engine/InputSource.h`:

```cpp
#pragma once

namespace dcr
{
    class FloatRingBuffer;

    // The minimal surface the matrix needs from a global input.  Both DeviceWorker
    // (hardware) and AppAudioWorker (process tap) implement it, so MatrixProcessor
    // consumes them uniformly.  isAppInput()/isAttached() drive the never-stall rule
    // (see MatrixInputPlan.h): app inputs contribute silence when detached/underfull
    // instead of stalling the whole matrix.
    class InputSource
    {
    public:
        virtual ~InputSource() = default;
        virtual FloatRingBuffer* getInputRing (int ch) noexcept = 0;
        virtual bool isAppInput() const noexcept = 0;
        virtual bool isAttached() const noexcept = 0;
    };
} // namespace dcr
```

- [ ] **Step 2: `DeviceWorker` implements `InputSource`**

In `Source/Engine/DeviceWorker.h`: add `#include "Engine/InputSource.h"`, change the
class declaration to derive from it, mark `getInputRing` `override`, and add the two
trait methods. DeviceWorker is hardware: always app-input=false, always attached.

```cpp
// class DeviceWorker  ->  class DeviceWorker : public InputSource
// existing: FloatRingBuffer* getInputRing (int ch) noexcept { ... }
//   add `override`:
FloatRingBuffer* getInputRing (int ch) noexcept override { /* unchanged body */ }
// add:
bool isAppInput() const noexcept override { return false; }
bool isAttached() const noexcept override { return true; }
```

- [ ] **Step 3: `AppAudioWorker` implements `InputSource`**

In `Source/Engine/AppAudioWorker.h`: `#include "Engine/InputSource.h"`, derive,
`override` `getInputRing`, and map the traits to its tap state.

```cpp
// class AppAudioWorker  ->  class AppAudioWorker : public InputSource
FloatRingBuffer* getInputRing (int ch) noexcept override { /* unchanged body */ }
bool isAppInput() const noexcept override { return true; }
bool isAttached() const noexcept override { return attached.load (std::memory_order_acquire); }
```

(`isAttached()` duplicates the existing public `isAttached()`; keep one — make the
existing one `override` and drop the separate declaration if redundant.)

- [ ] **Step 4: `GlobalInput` holds an `InputSource*`**

In `Source/Engine/MatrixProcessor.h`, change the field (and add the include):

```cpp
// #include "Engine/InputSource.h"
struct GlobalInput
{
    InputSource* source;     // was: DeviceWorker* device
    int channelIndex;
    PluginHost* plugin = nullptr;
};
```

`GlobalOutput` is unchanged (only DeviceWorkers produce output).

- [ ] **Step 5: Update the 2 input call sites in `MatrixProcessor.cpp`**

At `MatrixProcessor.cpp:326` and `:341`, replace `in.device->getInputRing(...)` /
`inputs[i].device->getInputRing(...)` with `...source->getInputRing(...)`. (The
never-stall behavior is Task 3; this step is just the rename so it compiles.)

- [ ] **Step 6: Build + tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: app builds; `0 tests failed` (matrix behavior unchanged so far —
hardware-only path is identical because `planMatrixInput` isn't wired yet).

- [ ] **Step 7: Commit**

```bash
git add Source/Engine/InputSource.h Source/Engine/DeviceWorker.h \
        Source/Engine/AppAudioWorker.h Source/Engine/MatrixProcessor.h \
        Source/Engine/MatrixProcessor.cpp
git commit -m "refactor(appinput): InputSource interface; matrix consumes it (no behavior change)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `AppInputSpec` + `AudioEngine` builds app workers

**Files:**
- Modify: `Source/Engine/AudioEngine.h`, `Source/Engine/AudioEngine.cpp`

- [ ] **Step 1: Add `AppInputSpec` and members to `AudioEngine.h`**

Next to `DeviceSpec` (`AudioEngine.h:74`):

```cpp
struct AppInputSpec
{
    juce::String bundleId;        // "com.google.Chrome" — stable key
    juce::String displayName;     // cached friendly name
    bool muteOriginalOutput = true;
    int numChannels = 2;          // stereo mixdown (v1 fixed)
};
```

Add an `#include "Engine/AppAudioWorker.h"`, a members:

```cpp
std::vector<std::unique_ptr<AppAudioWorker>> appWorkers;
std::vector<AppInputSpec> appInputSpecs; // remembered across start()
```

a `start` overload + accessors:

```cpp
bool start (const std::vector<DeviceSpec>& devices,
            const std::vector<AppInputSpec>& appInputs);
bool start (const std::vector<DeviceSpec>& devices) { return start (devices, {}); }

int getNumAppWorkers() const noexcept { return (int) appWorkers.size(); }
AppAudioWorker* getAppWorker (int i) noexcept
{
    return i >= 0 && i < (int) appWorkers.size() ? appWorkers[(size_t) i].get() : nullptr;
}
const std::vector<AppInputSpec>& getAppInputSpecs() const noexcept { return appInputSpecs; }
```

- [ ] **Step 2: Build app workers in `AudioEngine::start`**

In `Source/Engine/AudioEngine.cpp`, change the signature to the two-arg form, store
`appInputSpecs = appInputs;`, clear `appWorkers`, and AFTER the device-worker loop
(after `totalIns`/`totalOuts` are summed from devices, ~line 149) append app workers:

```cpp
// App-audio capture sources occupy global input channels AFTER all device inputs.
for (const auto& spec : appInputs)
{
    auto w = std::make_unique<AppAudioWorker> (spec.muteOriginalOutput, spec.numChannels);
    if (! w->open (settings))
        continue;
    totalIns += w->getNumInputChannels(); // contributes numChannels (2) global inputs
    appWorkers.push_back (std::move (w));
}
```

(`open()` allocates rings; the tap is bound later by Part 3b's watcher via `attach()`.)

- [ ] **Step 3: Extend the `GlobalInput` build + event wiring**

In the loop that builds `globalIns` (~`AudioEngine.cpp:222-240`), the device-worker
entries now push `InputSource*` (a `DeviceWorker*` converts implicitly). AFTER the
device loop, append the app-worker inputs and create their per-input plugin hosts so
indices line up:

```cpp
for (size_t i = 0; i < appWorkers.size(); ++i)
{
    auto* w = appWorkers[i].get();
    for (int ch = 0; ch < w->getNumInputChannels(); ++ch)
    {
        auto ph = std::make_unique<PluginHost>();
        ph->prepare (settings.engineSampleRate, settings.engineBlockSize);
        inputPluginHosts.push_back (std::move (ph));
        globalIns.push_back ({ w, ch, inputPluginHosts.back().get() });
    }
}
```

> NOTE: build the device `inputPluginHosts` first (existing code), then the app ones,
> so `inputPluginHosts` index order matches `globalIns` order. Verify the existing
> input-plugin-host loop count uses the device input total, then this appends.

Then wire the app workers' ready event alongside the device workers (near
`AudioEngine.cpp:250`):

```cpp
for (auto& w : appWorkers)
    w->setInputReadyEvent (&processor.getInputReadyEvent());
```

In `AudioEngine::stop()` (~line 264), clear app workers too:

```cpp
appWorkers.clear();
```

- [ ] **Step 4: Build**

Run: `cmake --build build -j`
Expected: builds clean. (No app sources are passed yet, so behavior is unchanged.)

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/AudioEngine.h Source/Engine/AudioEngine.cpp
git commit -m "feat(appinput): AudioEngine builds AppAudioWorkers into the global input space

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Wire the never-stall rule into the matrix

**Files:**
- Modify: `Source/Engine/MatrixProcessor.cpp`

- [ ] **Step 1: Apply `planMatrixInput` in the availability loop**

Add `#include "Engine/MatrixInputPlan.h"` to `MatrixProcessor.cpp`. Replace the
availability loop (~`MatrixProcessor.cpp:324-334`) so app inputs never stall:

```cpp
for (auto& in : inputs)
{
    auto* ring = in.source->getInputRing (in.channelIndex);
    if (ring == nullptr)
        return false;
    const auto plan = planMatrixInput (in.source->isAppInput(), in.source->isAttached(),
        (int) ring->readAvailable(), blockSize);
    if (plan.stalls) // hardware underfull -> wait; app inputs never set this
    {
        blocksStalled.fetch_add (1, std::memory_order_relaxed);
        return false;
    }
}
```

- [ ] **Step 2: Apply it in the read loop (silence when not ready)**

Replace the read loop (~`MatrixProcessor.cpp:339-348`):

```cpp
for (size_t i = 0; i < inputs.size(); ++i)
{
    auto* ring = inputs[i].source->getInputRing (inputs[i].channelIndex);
    float* dst = inBuf.data() + i * (size_t) blockSize;
    const auto plan = planMatrixInput (inputs[i].source->isAppInput(), inputs[i].source->isAttached(),
        (int) ring->readAvailable(), blockSize);
    if (plan.action == MatrixInputAction::Read)
        ring->read (dst, (size_t) blockSize);
    else
        std::fill (dst, dst + blockSize, 0.0f); // offline/underfull app input -> silence

    const auto r = juce::FloatVectorOperations::findMinAndMax (dst, blockSize);
    const float peak = juce::jmax (std::abs (r.getStart()), std::abs (r.getEnd()));
    inputPeaks[i].store (peak, std::memory_order_relaxed);
}
```

- [ ] **Step 3: Build + tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: builds; `0 tests failed`. Hardware-only behavior is unchanged (a hardware
input that's underfull still stalls via `plan.stalls`); app inputs (none yet at
runtime) would contribute silence when detached.

- [ ] **Step 4: Commit**

```bash
git add Source/Engine/MatrixProcessor.cpp
git commit -m "feat(appinput): matrix never-stalls on app inputs (planMatrixInput wired)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (3a):** §4 (AppInputSpec, partial — data model) → Task 2; §5 wiring
(workers into engine) → Task 2; §6/§7 (slot in global space; matrix never-stall) →
Tasks 1+3. Deferred to 3b/3c: live attach/detach + watcher (§8/§9 wiring), Soft-In
groups (§11), persistence (§15), UI (§12), permissions/gating (§14).

**Placeholder scan:** none. Edits to existing code reference real line numbers from
the integration map; new code is complete. The one NOTE (input-plugin-host ordering)
is an ordering caution, not a placeholder.

**Type consistency:** `InputSource` (`getInputRing`/`isAppInput`/`isAttached`) is used
identically in DeviceWorker, AppAudioWorker, `GlobalInput::source`, and the matrix
loops. `AppInputSpec` fields match Part 2's worker ctor (`muteOriginalOutput`,
`numChannels`) and the Part-1 resolver's bundleId key. `planMatrixInput` /
`MatrixInputAction::Read|Silence` match the Part-1 header.

## Next

Part 3b: `MainComponent` owns `AppAudioProcesses`, runs `reconcile()` on the
message-thread timer, drives `attach()`/`detach()` into `engine.getAppWorker(i)`
(respecting the reconfigure freeze-set), and adds a minimal "App Audio Inputs" picker
to `DeviceManagerDialog` — the first end-to-end real-device capture test.

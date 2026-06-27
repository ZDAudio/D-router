# ASRC Phase 1 (output-side) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fixed-ratio output sample-rate converter with a variable-ratio resampler (libsamplerate) that a per-device PI control loop continuously trims to track the output device's real clock — fixing pitch/crackle on drifting/loose-clock output devices (e.g. the WF-1000XM5 Bluetooth earbuds).

**Architecture:** Output-only this phase; the engine stays input-gated (no free-running clock yet — that's Phase 2). Each output channel gets an `AsyncResampler` (libsamplerate, push API, same `pushInput`/`pullOutput` interface as the old `SampleRateConverter`). One `DriftController` per device reads the output ring fill each callback and produces a clamped ±ppm ratio trim, applied to every output channel's resampler. A settings toggle (`asrcDriftComp`) freezes the trim for A/B.

**Tech Stack:** C++20, JUCE 8, CoreAudio, libsamplerate (BSD-2, via CMake FetchContent), CTest.

Reference spec: `docs/superpowers/specs/2026-06-25-asrc-design.md`.

---

### Task 1: Add libsamplerate dependency

**Files:**
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the FetchContent block**

In `CMakeLists.txt`, near the JUCE `FetchContent` setup, add:

```cmake
# libsamplerate (BSD-2) — variable-ratio resampler for async SRC / drift comp.
FetchContent_Declare(
    libsamplerate
    GIT_REPOSITORY https://github.com/libsndfile/libsamplerate.git
    GIT_TAG 0.2.2
)
set(LIBSAMPLERATE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(LIBSAMPLERATE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(libsamplerate)
```

- [ ] **Step 2: Link it to the app and the JUCE test target**

Find each `target_link_libraries(...)` for `dcorerouter` and `dcorerouter_tests_juce` and add `samplerate` to the link list. Example for the app:

```cmake
target_link_libraries(dcorerouter PRIVATE
    # ...existing...
    samplerate)
```

Do the same for `dcorerouter_tests_juce`. (Do NOT add it to `dcorerouter_tests` — that target stays JUCE-free/dependency-free; it only tests `DriftController`.)

- [ ] **Step 3: Configure + build to verify the dependency resolves**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j`
Expected: configures (fetches libsamplerate), builds with no errors. (No behavior change yet.)

- [ ] **Step 4: Commit**

```bash
git add CMakeLists.txt
git commit -m "build: add libsamplerate (FetchContent) for async SRC"
```

---

### Task 2: DriftController (JUCE-free PI control loop)

**Files:**
- Create: `Source/Engine/DriftController.h`
- Test: `tests/CoreLogicTests.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/CoreLogicTests.cpp`, add the include near the other `Engine/` includes:

```cpp
#include "Engine/DriftController.h"
```

Add this test function before the closing `} // namespace`:

```cpp
    // ---------------------------------------------------------------------------
    // DriftController -- PI loop turning output-ring fill into a ppm ratio trim.
    // Convention (OUTPUT path): ring too full -> POSITIVE ppm; caller applies
    // ratioMul = 1 - ppm*1e-6 so the device drains the ring faster.
    // ---------------------------------------------------------------------------
    void test_drift_controller()
    {
        // target 1000 samples, gentle gains, light low-pass, ±500 ppm clamp.
        dcr::DriftController c;
        c.prepare (/*target*/ 1000.0, /*maxPpm*/ 500.0,
                   /*kP*/ 0.001, /*kI*/ 0.0001, /*lpAlpha*/ 1.0);

        // At target -> ~zero trim.
        const double atTarget = c.updatePpm (1000.0);
        CHECK (std::fabs (atTarget) < 1.0e-9);

        // Too full -> positive ppm (drain faster).
        c.reset();
        double ppm = 0.0;
        for (int i = 0; i < 50; ++i)
            ppm = c.updatePpm (1100.0); // 100 over target
        CHECK (ppm > 0.0);

        // Too empty -> negative ppm.
        c.reset();
        for (int i = 0; i < 50; ++i)
            ppm = c.updatePpm (900.0);
        CHECK (ppm < 0.0);

        // Clamp: a huge error can't exceed maxPpm.
        c.reset();
        for (int i = 0; i < 1000; ++i)
            ppm = c.updatePpm (1.0e9);
        CHECK (ppm <= 500.0 + 1.0e-9);
        CHECK (ppm >= -500.0 - 1.0e-9);

        // Low-pass: with lpAlpha small, a single spike barely moves the output.
        dcr::DriftController s;
        s.prepare (1000.0, 500.0, 0.001, 0.0, /*lpAlpha*/ 0.01);
        s.updatePpm (1000.0);             // seed at target
        const double afterSpike = s.updatePpm (5000.0); // big spike, one sample
        // Proportional-only; 1% of a 4000 error -> ~40 * kP -> tiny.
        CHECK (std::fabs (afterSpike) < 0.1);
    }
```

Register it in `main()` next to the other engine tests:

```cpp
    test_drift_controller();
```

- [ ] **Step 2: Run to verify it fails**

Run: `cmake --build build --target dcorerouter_tests -j`
Expected: FAIL to compile — `DriftController.h` not found / `dcr::DriftController` undefined.

- [ ] **Step 3: Implement DriftController.h**

Create `Source/Engine/DriftController.h`:

```cpp
#pragma once

#include <algorithm>
#include <cmath>

namespace dcr
{

    // PI control loop for async-SRC drift compensation.  Turns a device's ring
    // fill (samples) into a clamped ±ppm trim that the caller applies to the
    // resampler ratio.  One controller PER DEVICE (all of a device's channels
    // share its hardware clock).  JUCE-free + deterministic so it is unit-tested
    // headless.  See DriftController test in CoreLogicTests and the ASRC spec.
    //
    // OUTPUT-path sign convention: error = lowpass(fill) - target.  Ring too full
    // (error > 0) -> positive ppm; the caller uses ratioMul = 1 - ppm*1e-6 so the
    // device consumes the ring faster.  (Phase 2 input path will flip the sign.)
    struct DriftController
    {
        // Tunables.
        double target = 0.0;    // ring set-point, samples
        double maxPpm = 500.0;  // hard clamp
        double kP = 0.0;        // proportional gain
        double kI = 0.0;        // integral gain
        double lpAlpha = 1.0;   // fill EMA coefficient (1 = no smoothing)

        // State.
        double fillLp = -1.0; // low-passed fill (-1 = uninitialised)
        double integ = 0.0;
        double ppm = 0.0;

        void prepare (double target_, double maxPpm_, double kP_, double kI_, double lpAlpha_)
        {
            target = target_;
            maxPpm = maxPpm_;
            kP = kP_;
            kI = kI_;
            lpAlpha = std::clamp (lpAlpha_, 0.0, 1.0);
            reset();
        }

        void reset()
        {
            fillLp = -1.0;
            integ = 0.0;
            ppm = 0.0;
        }

        // Call once per device callback with the current ring fill (samples).
        // Returns the clamped ppm trim.
        double updatePpm (double currentFill)
        {
            if (fillLp < 0.0)
                fillLp = currentFill; // seed on first call
            else
                fillLp += lpAlpha * (currentFill - fillLp);

            const double error = fillLp - target;

            // Integrate with anti-windup: clamp so kI*integ alone can't exceed
            // the ppm clamp.
            integ += error;
            if (kI > 0.0)
            {
                const double integLimit = maxPpm / kI;
                integ = std::clamp (integ, -integLimit, integLimit);
            }

            const double raw = kP * error + kI * integ;
            ppm = std::clamp (raw, -maxPpm, maxPpm);
            return ppm;
        }

        // Convenience: the ratio multiplier the OUTPUT path applies.
        double ratioMultiplier() const { return 1.0 - ppm * 1.0e-6; }
    };

} // namespace dcr
```

- [ ] **Step 4: Run to verify it passes**

Run: `cmake --build build --target dcorerouter_tests -j && ctest --test-dir build -R "dcorerouter_tests$" --output-on-failure`
Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/DriftController.h tests/CoreLogicTests.cpp
git commit -m "feat(engine): DriftController PI loop for async-SRC drift comp"
```

---

### Task 3: AsyncResampler (libsamplerate, variable ratio)

**Files:**
- Create: `Source/Engine/AsyncResampler.h`, `Source/Engine/AsyncResampler.cpp`
- Modify: `CMakeLists.txt` (add the new .cpp to `dcorerouter` and `dcorerouter_tests_juce` sources)
- Test: `tests/juce/AsyncResamplerTests.cpp` (new), registered in `tests/juce/TestMainJuce.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/juce/AsyncResamplerTests.cpp`:

```cpp
#include "Engine/AsyncResampler.h"
#include <juce_core/juce_core.h>
#include <cmath>
#include <vector>

struct AsyncResamplerTests : juce::UnitTest
{
    AsyncResamplerTests() : juce::UnitTest ("AsyncResampler") {}

    void runTest() override
    {
        beginTest ("48k->44.1k produces ~ratio output and stays finite");
        {
            dcr::AsyncResampler r;
            expect (r.prepare (48000.0, 44100.0, /*quality SRC_SINC_MEDIUM*/ 1));
            expectEquals (r.getInputSampleRate(), 48000.0);
            expectEquals (r.getOutputSampleRate(), 44100.0);

            // Push 4800 input frames of a 1 kHz sine; pull ~4410 out.
            std::vector<float> in (4800);
            for (int i = 0; i < 4800; ++i)
                in[(size_t) i] = std::sin (2.0 * 3.14159265 * 1000.0 * i / 48000.0);
            r.pushInput (in.data(), (int) in.size());

            std::vector<float> out (5000, 999.0f);
            int got = r.pullOutput (out.data(), 4410);
            // Should produce close to 4410 (within a small primer margin).
            expect (got > 4300 && got <= 4410);
            for (int i = 0; i < got; ++i)
                expect (std::isfinite (out[(size_t) i]) && std::fabs (out[(size_t) i]) <= 1.5f);
        }

        beginTest ("ratio trim shifts how much input is consumed");
        {
            dcr::AsyncResampler r;
            expect (r.prepare (48000.0, 48000.0, 1)); // base ratio 1.0
            std::vector<float> in (10000, 0.25f);
            r.pushInput (in.data(), (int) in.size());

            std::vector<float> out (1000);
            r.setRatioMultiplier (1.0);
            r.pullOutput (out.data(), 1000);
            const int usedNominal = r.lastInputConsumed();

            // +1000 ppm: ratio slightly >1 -> fewer input frames per output frame.
            dcr::AsyncResampler r2;
            r2.prepare (48000.0, 48000.0, 1);
            r2.pushInput (in.data(), (int) in.size());
            r2.setRatioMultiplier (1.001);
            r2.pullOutput (out.data(), 1000);
            const int usedTrim = r2.lastInputConsumed();

            expect (usedTrim < usedNominal); // higher ratio consumed less input
        }
    }
};

static AsyncResamplerTests asyncResamplerTests;
```

(`lastInputConsumed()` is a tiny test-only accessor we add for observability.)

- [ ] **Step 2: Wire the test + sources into CMake, run to verify it fails**

In `CMakeLists.txt`: add `Source/Engine/AsyncResampler.cpp` to `target_sources(dcorerouter PRIVATE ...)` and to `target_sources(dcorerouter_tests_juce PRIVATE ...)`, and add `tests/juce/AsyncResamplerTests.cpp` to the `dcorerouter_tests_juce` sources.

Run: `cmake --build build --target dcorerouter_tests_juce -j`
Expected: FAIL — `AsyncResampler.h` not found.

- [ ] **Step 3: Implement AsyncResampler.h**

Create `Source/Engine/AsyncResampler.h`:

```cpp
#pragma once

#include <vector>

struct SRC_STATE_tag;

namespace dcr
{

    // Variable-ratio resampler (libsamplerate) with the same push/pull shape as
    // the old SampleRateConverter, plus a live drift-trim multiplier on the
    // ratio.  Per channel (mono).  Allocations happen in prepare(); pushInput /
    // pullOutput are allocation-free in steady state (the pending buffer is
    // reserved up front).  See the ASRC spec.
    class AsyncResampler
    {
    public:
        AsyncResampler() = default;
        ~AsyncResampler();

        AsyncResampler (const AsyncResampler&) = delete;
        AsyncResampler& operator= (const AsyncResampler&) = delete;

        // quality: libsamplerate converter type (0=BEST,1=MEDIUM,2=FASTEST).
        // maxInputPerPull: upper bound on engine frames buffered between pulls
        //   (used to reserve the pending buffer so steady state doesn't realloc).
        bool prepare (double inRate, double outRate, int quality, int maxInputPerPull = 8192);
        void reset();

        // Drift trim: effective ratio = (outRate/inRate) * mult.  1.0 = nominal.
        void setRatioMultiplier (double mult) { ratioMult = mult; }

        void pushInput (const float* samples, int numSamples);
        // Produce up to numSamples output frames into dst; returns frames written.
        int pullOutput (float* dst, int numSamples);

        double getInputSampleRate() const { return inRate; }
        double getOutputSampleRate() const { return outRate; }
        int getOutputLatencySamples() const;

        int lastInputConsumed() const { return lastConsumed; } // test/diagnostic

    private:
        SRC_STATE_tag* state = nullptr;
        double inRate = 0.0, outRate = 0.0, baseRatio = 1.0, ratioMult = 1.0;
        std::vector<float> pending; // engine-rate input not yet consumed
        std::size_t pendingPos = 0;
        int lastConsumed = 0;
    };

} // namespace dcr
```

- [ ] **Step 4: Implement AsyncResampler.cpp**

Create `Source/Engine/AsyncResampler.cpp`:

```cpp
#include "Engine/AsyncResampler.h"

#include <samplerate.h>

#include <algorithm>

namespace dcr
{

    AsyncResampler::~AsyncResampler() { reset(); }

    bool AsyncResampler::prepare (double inRate_, double outRate_, int quality, int maxInputPerPull)
    {
        reset();
        inRate = inRate_;
        outRate = outRate_;
        baseRatio = (inRate_ > 0.0) ? (outRate_ / inRate_) : 1.0;
        ratioMult = 1.0;

        int err = 0;
        state = src_new (quality, /*channels*/ 1, &err);
        if (state == nullptr)
            return false;

        // Reserve generously so steady-state push/compact never reallocates.
        pending.reserve ((std::size_t) std::max (1024, maxInputPerPull * 2));
        pendingPos = 0;
        lastConsumed = 0;
        return true;
    }

    void AsyncResampler::reset()
    {
        if (state != nullptr)
        {
            src_delete (state);
            state = nullptr;
        }
        pending.clear();
        pendingPos = 0;
        ratioMult = 1.0;
        lastConsumed = 0;
    }

    void AsyncResampler::pushInput (const float* samples, int numSamples)
    {
        if (numSamples <= 0)
            return;
        // Compact if the consumed prefix is large (keeps the buffer bounded).
        if (pendingPos > 0 && pendingPos >= pending.size() / 2)
        {
            pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) pendingPos);
            pendingPos = 0;
        }
        pending.insert (pending.end(), samples, samples + numSamples);
    }

    int AsyncResampler::pullOutput (float* dst, int numSamples)
    {
        lastConsumed = 0;
        if (state == nullptr || numSamples <= 0)
            return 0;

        const double ratio = baseRatio * ratioMult;
        int producedTotal = 0;

        while (producedTotal < numSamples)
        {
            const long availIn = (long) (pending.size() - pendingPos);

            SRC_DATA d;
            d.data_in = (availIn > 0) ? (pending.data() + pendingPos) : nullptr;
            d.input_frames = availIn;
            d.data_out = dst + producedTotal;
            d.output_frames = numSamples - producedTotal;
            d.input_frames_used = 0;
            d.output_frames_gen = 0;
            d.end_of_input = 0;
            d.src_ratio = ratio;

            if (src_process (state, &d) != 0)
                break;

            pendingPos += (std::size_t) d.input_frames_used;
            lastConsumed += (int) d.input_frames_used;
            producedTotal += (int) d.output_frames_gen;

            // No progress (out of input) -> stop; caller treats the shortfall as
            // an underrun, same as the old SampleRateConverter.
            if (d.input_frames_used == 0 && d.output_frames_gen == 0)
                break;
        }
        return producedTotal;
    }

    int AsyncResampler::getOutputLatencySamples() const
    {
        // libsamplerate sinc converters add a fixed group delay; a small constant
        // is adequate for the latency report (the controlled ring set-point
        // dominates).  Treat as ~ half the sinc length at MEDIUM quality.
        return state != nullptr ? 0 : 0;
    }

} // namespace dcr
```

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R "dcorerouter_tests_juce$" --output-on-failure`
Expected: PASS (the round-trip produces ~4410 finite frames; the trim consumes less input).

- [ ] **Step 6: Commit**

```bash
git add Source/Engine/AsyncResampler.h Source/Engine/AsyncResampler.cpp tests/juce/AsyncResamplerTests.cpp CMakeLists.txt
git commit -m "feat(engine): AsyncResampler (libsamplerate variable-ratio) + tests"
```

---

### Task 4: ASRC settings

**Files:**
- Modify: `Source/Engine/EngineSettings.h`
- Modify: `Source/Persistence/SettingsStore.cpp`
- Test: `tests/juce/SettingsStoreTests.cpp`

- [ ] **Step 1: Add fields to EngineSettings**

In `Source/Engine/EngineSettings.h`, after the existing `srcQuality`/`srcComplexity` fields, add:

```cpp
        // --- Async SRC / drift compensation (output side, Phase 1) ---
        unsigned int asrcQuality = 1;       // libsamplerate: 0=BEST,1=MEDIUM,2=FASTEST
        bool asrcDriftComp = true;          // false = freeze trim (A/B)
        double asrcTargetMs = 10.0;         // output-ring set-point
        int asrcMaxPpm = 500;               // drift-trim clamp
        double asrcLoopTimeConstS = 2.0;    // control-loop time constant
```

In the `audioPathEquals` comparison in the same file, add the audio-affecting ones:

```cpp
                   && asrcQuality == o.asrcQuality
                   && asrcDriftComp == o.asrcDriftComp
                   && asrcTargetMs == o.asrcTargetMs
                   && asrcMaxPpm == o.asrcMaxPpm
                   && asrcLoopTimeConstS == o.asrcLoopTimeConstS
```

- [ ] **Step 2: Write the failing persistence test**

In `tests/juce/SettingsStoreTests.cpp`, inside the existing round-trip test (or a new `beginTest` block), set + assert the new fields. Add a new block:

```cpp
        beginTest ("ASRC settings round-trip");
        {
            dcr::EngineSettings s;
            s.asrcQuality = 0;
            s.asrcDriftComp = false;
            s.asrcTargetMs = 12.5;
            s.asrcMaxPpm = 300;
            s.asrcLoopTimeConstS = 3.0;

            juce::TemporaryFile scratch;
            expect (dcr::SettingsStore::save (scratch.getFile(), s));
            dcr::EngineSettings r;
            expect (dcr::SettingsStore::load (scratch.getFile(), r));
            expectEquals (r.asrcQuality, (unsigned int) 0);
            expect (!r.asrcDriftComp);
            expectEquals (r.asrcTargetMs, 12.5);
            expectEquals (r.asrcMaxPpm, 300);
            expectEquals (r.asrcLoopTimeConstS, 3.0);
        }
```

(Match the exact `save`/`load` signatures used elsewhere in this test file.)

- [ ] **Step 3: Run to verify it fails**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R "dcorerouter_tests_juce$" --output-on-failure`
Expected: FAIL — loaded values don't match (not persisted yet).

- [ ] **Step 4: Persist the fields in SettingsStore.cpp**

In `Source/Persistence/SettingsStore.cpp`, mirror the existing per-field save/load for each new field (follow the exact pattern used for `srcQuality`). Add a property key + a write in the save path and a read (with the default) in the load path for `asrcQuality`, `asrcDriftComp`, `asrcTargetMs`, `asrcMaxPpm`, `asrcLoopTimeConstS`.

- [ ] **Step 5: Run to verify it passes**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R "dcorerouter_tests_juce$" --output-on-failure`
Expected: PASS.

- [ ] **Step 6: Commit**

```bash
git add Source/Engine/EngineSettings.h Source/Persistence/SettingsStore.cpp tests/juce/SettingsStoreTests.cpp
git commit -m "feat(settings): ASRC output-side settings + persistence"
```

---

### Task 5: Wire AsyncResampler + DriftController into the output path

**Files:**
- Modify: `Source/Engine/DeviceWorker.h`
- Modify: `Source/Engine/DeviceWorker.cpp`

There is no headless unit test for the live device callback (it needs a real device); verification is the build plus the real-device check in Task 8. Keep the change a faithful swap of the existing output SRC.

- [ ] **Step 1: Swap the output SRC type + add the controller (header)**

In `Source/Engine/DeviceWorker.h`:
- Add includes: `#include "Engine/AsyncResampler.h"` and `#include "Engine/DriftController.h"`.
- Replace the output SRC member:
  - Remove: `std::vector<std::unique_ptr<SampleRateConverter>> outputSRCs;`
  - Add: `std::vector<std::unique_ptr<AsyncResampler>> outputResamplers;`
- Add: `DriftController outputDrift;` (one per device).

(Leave `inputSRCs` as `SampleRateConverter` — input is Phase 2.)

- [ ] **Step 2: Build the resamplers + controller in open()**

In `Source/Engine/DeviceWorker.cpp` `open()`, replace the `outputSRCs` build loop (around lines 142-149) with:

```cpp
        outputResamplers.clear();
        for (int i = 0; i < numOutputChannels; ++i)
        {
            auto r = std::make_unique<AsyncResampler>();
            // Output path: engine rate IN, device rate OUT.
            r->prepare (engineSampleRate, devSr, (int) settings.asrcQuality,
                /*maxInputPerPull*/ (int) scratchSize);
            outputResamplers.push_back (std::move (r));
        }

        // Per-device output drift controller.  The output ring holds ENGINE-rate
        // samples (the matrix writes engine samples; the device callback pulls
        // engine samples through the resampler), so the target is in engine-rate
        // samples.  Derive gains from the loop time constant; low-pass the fill
        // over ~1 s of device callbacks.
        {
            const double targetSamples = settings.asrcTargetMs * 1.0e-3 * engineSampleRate;
            const double callbacksPerSec = (devBuf > 0) ? (devSr / devBuf) : 100.0;
            const double tc = std::max (0.1, settings.asrcLoopTimeConstS);
            // kP small; kI smaller; lpAlpha for ~1 s EMA.
            const double kP = (double) settings.asrcMaxPpm / std::max (1.0, targetSamples) / 50.0;
            const double kI = kP / (tc * callbacksPerSec);
            const double lpAlpha = std::clamp (1.0 / std::max (1.0, callbacksPerSec), 0.0, 1.0);
            outputDrift.prepare (targetSamples, (double) settings.asrcMaxPpm, kP, kI, lpAlpha);
        }
```

(`scratchSize`, `devSr`, `devBuf` are already computed earlier in `open()`.)

- [ ] **Step 3: Apply the drift trim each callback + use the resamplers**

In `Source/Engine/DeviceWorker.cpp`, in the output half of `audioDeviceIOCallbackWithContext` (the `for (int ch = 0; ch < nOut; ++ch)` block around line 305), BEFORE the channel loop add:

```cpp
        // Per-device drift trim from the output ring fill (channel 0 is
        // representative -- all output channels are fed in lockstep by the
        // matrix).  When drift comp is off, hold the nominal ratio.
        // `asrcDriftCompActive` is a member cached in open() (RT-safe; the
        // callback must not read the EngineSettings struct directly).
        double outMult = 1.0;
        if (asrcDriftCompActive && nOut > 0)
        {
            const double fill = (double) outputRings[0].readAvailable();
            outputDrift.updatePpm (fill);
            outMult = outputDrift.ratioMultiplier();
        }
```

Then inside the channel loop, replace `auto& src = *outputSRCs[(size_t) ch];` with:

```cpp
            auto& src = *outputResamplers[(size_t) ch];
            src.setRatioMultiplier (outMult);
```

The rest of the loop is unchanged: `src.pullOutput(...)`, the `ratio = src.getInputSampleRate()/src.getOutputSampleRate()` estimate, `src.pushInput(...)`, the underrun handling — `AsyncResampler` exposes the same methods, so the body compiles as-is.

Add the cached member: in `DeviceWorker.h` add `bool asrcDriftCompActive = false;`, and in `open()` (near where the controller is built) set `asrcDriftCompActive = settings.asrcDriftComp;`. The RT callback reads only this member, never the `EngineSettings` struct.

- [ ] **Step 4: Build**

Run: `cmake --build build -j`
Expected: builds clean. Fix any reference to the removed `outputSRCs` (there should be exactly the spots changed above plus the `reset`/latency loops at lines ~187 and ~213 — change those to iterate `outputResamplers`).

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/DeviceWorker.h Source/Engine/DeviceWorker.cpp
git commit -m "feat(engine): output path uses AsyncResampler + per-device drift comp"
```

---

### Task 6: Output latency report

**Files:**
- Modify: `Source/Engine/DeviceWorker.cpp` (`getOutputSrcLatencyDeviceSamples`)
- Modify: `Source/Engine/AudioEngine.cpp` (only if it referenced the old type)

- [ ] **Step 1: Point the SRC-latency accessor at the resamplers**

In `Source/Engine/DeviceWorker.cpp`, `getOutputSrcLatencyDeviceSamples()` currently iterates `outputSRCs`. Change it to iterate `outputResamplers` and call `getOutputLatencySamples()`:

```cpp
    int DeviceWorker::getOutputSrcLatencyDeviceSamples() const
    {
        int worst = 0;
        for (auto& r : outputResamplers)
            if (r)
                worst = std::max (worst, r->getOutputLatencySamples());
        return worst;
    }
```

- [ ] **Step 2: Build to verify no remaining references to outputSRCs**

Run: `cmake --build build -j`
Expected: builds clean (no `outputSRCs` references remain).

- [ ] **Step 3: Commit**

```bash
git add Source/Engine/DeviceWorker.cpp Source/Engine/AudioEngine.cpp
git commit -m "feat(engine): report output AsyncResampler latency"
```

---

### Task 7: Settings UI (quality + drift-comp toggle)

**Files:**
- Modify: `Source/UI/SettingsDialog.cpp`

- [ ] **Step 1: Add the controls**

In `Source/UI/SettingsDialog.cpp`, in the `// ====== SRC ======` section (near the existing "SRC quality" field), add a drift-comp toggle and an ASRC quality chooser, bound to `working.asrcDriftComp` and `working.asrcQuality`, following the existing `addBoolField` / `addUIntComboField` helpers:

```cpp
        addBoolField ("Async SRC drift compensation",
            working.asrcDriftComp,
            "Continuously track each output device's real clock (fixes drift / "
            "Bluetooth pitch+crackle).  Off = fixed-ratio (A/B).");

        addUIntComboField ("Async SRC quality",
            working.asrcQuality,
            { "Best", "Medium", "Fastest" },
            { 0u, 1u, 2u },
            "libsamplerate converter quality for the variable-ratio resampler.");
```

(Use the exact helper names/signatures already in this file; mirror the "SRC quality" field added at the existing lines.)

- [ ] **Step 2: Build**

Run: `cmake --build build -j`
Expected: builds clean.

- [ ] **Step 3: Commit**

```bash
git add Source/UI/SettingsDialog.cpp
git commit -m "feat(ui): expose Async SRC drift-comp toggle + quality"
```

---

### Task 8: Full verification

**Files:** none (verification only)

- [ ] **Step 1: Format check (CI parity)**

Run, for every file touched: `clang-format --dry-run --Werror <file>`
Expected: clean. If not, `clang-format -i <file>` and amend the relevant commit.

- [ ] **Step 2: Full build + all tests**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: build clean; `100% tests passed` (both targets, including `DriftController` and `AsyncResampler`).

- [ ] **Step 3: Launch + sanity**

Run: `./run.sh`
Expected: app launches; `~/Library/Logs/D-Router/session-*.log` shows the engine starting; routing to a normal 48k output still sounds correct (no regression on the matched-rate path).

- [ ] **Step 4: Real-device verification (user-driven — document results)**

- WF-1000XM5 (out): play audio through D-Router. Expected: **correct pitch, no crackle** (the Phase-1 goal). A/B the **Async SRC drift compensation** toggle (Settings) — off should reproduce the old pitch/crackle, on should fix it.
- Two unsynced 48k devices, steady tone, 30+ min: `PERF` `drop`/`xrun` stay ~0; no audible pitch wobble.
- Confirm no regression on the existing speakers-only path.

- [ ] **Step 5: (Optional) package a test build**

Run: `./package.sh` → `dist/D-Router.zip` for sharing.

---

## Notes for the implementer

- **Do not** touch the input path or the matrix's input-gating this phase — that's Phase 2 (free-running clock). Keep `inputSRCs` as `SampleRateConverter`.
- The drift controller's gain derivation in Task 5 Step 2 is a *starting point*; the real tuning happens during real-device testing (Task 8). The toggle + A/B is the safety net.
- If `AsyncResampler` round-trip fidelity or the trim test reveals libsamplerate buffering surprises, that's the place to fix the wrapper — the device callback semantics must match the old converter exactly.

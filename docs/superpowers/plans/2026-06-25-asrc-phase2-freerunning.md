# ASRC Phase 2 (free-running clock) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development or superpowers:executing-plans. Steps use `- [ ]`.

**Goal:** Make the matrix engine **free-running** (paced by its own block clock) and regulate **every** device's ring independently with an async resampler + drift controller, so multiple devices with different/drifting clocks (e.g. a 48 k speaker + a 44.1 k Bluetooth WF-1000XM5) no longer couple through shared pacing. This is the real fix for "adding the WF corrupts the speaker."

**Architecture:** Today the matrix is **input-gated** (`tryProcessOneBlock` waits for input rings) **and output-backpressured** (`matrixOutputStalls`). Those two mechanisms couple all devices to one shared pace — fine for one clock domain, broken for several. Phase 2 replaces both with: the matrix **produces one block per engine-block-period on a monotonic host clock**, reading silence from any momentarily-underfull input and writing every output without stalling; each device's **DriftController** (Phase 1, self-calibrating) keeps that device's ring centered. A free-running matrix cannot spin, so this also **subsumes the #28 backpressure fix** (which existed only to stop the app-tap spin).

**Confirmed root cause (real-device, 2026-06-26, on shipped 0.7.3 — NOT ASRC):** with music routed to the 48k speaker, *merely adding* the 44.1k Bluetooth WF-1000XM5 as a 2nd output (nothing routed to it) corrupts the **speaker** — pitch + crackle. The `#28` output backpressure stalls the whole matrix when the bursty BT ring fills, yanking the shared pace off a steady 48k. The pitch specifically comes from the **app-tap** music source (`com.apple.Music`): app-taps don't gate the matrix, so they're *read at whatever pace the matrix runs* — pace off 48k → music read at the wrong rate → pitch. Engine SR + speaker SR both stay 48000 (no renegotiation), so the engine `xrun` counter stays ~0 while it's plainly audible. This is exactly what free-running fixes: a host-clocked 48k pace, immune to any output device's behavior.

**Builds on Phase 1** (already on branch `feat/asrc-phase1-output`, merged or rebased): reuses `AsyncResampler`, `DriftController`, the ASRC settings. **Start Phase 2 on a new branch off the Phase-1 work.**

**Tech Stack:** C++20, JUCE 8, CoreAudio, libsamplerate, CTest.

Reference: `docs/superpowers/specs/2026-06-25-asrc-design.md`. ⚠️ This is the **highest-risk** change in the project (the engine's core clocking). Phase the build; keep each step independently testable; lean hard on the real-device multi-output test.

---

### Task 1: Input-side AsyncResampler + DriftController (no clocking change yet)

Do the input side FIRST, while the matrix is still input-gated — it's lower risk and isolates the resampler swap from the clocking rewrite.

**Files:** `Source/Engine/DeviceWorker.{h,cpp}`

- [ ] **Step 1: Swap input SRC type + add input drift controller (header).** Replace `std::vector<std::unique_ptr<SampleRateConverter>> inputSRCs;` with `std::vector<std::unique_ptr<AsyncResampler>> inputResamplers;`. Add `DriftController inputDrift;` and `bool asrcDriftCompActiveIn = false;`.
- [ ] **Step 2: Build them in `open()`.** Mirror the output side: `inputResamplers[i]->prepare(devSr, engineSampleRate, (int) settings.asrcQuality, maxInputPerPull)` (input path: device rate IN, engine rate OUT). Build `inputDrift` with the same self-calibrating gain derivation as `outputDrift` (gain scale from `inputRingMult*engineBlock`, `warmupCalls ≈ 1.5*callbacksPerSec`). Set `asrcDriftCompActiveIn = settings.asrcDriftComp`.
- [ ] **Step 3: Wire the input callback.** In the input half of `audioDeviceIOCallbackWithContext`, before the per-channel loop compute `double inMult = 1.0; if (asrcDriftCompActiveIn) { inputDrift.updatePpm((double) inputRings[0].readAvailable()); inMult = inputDrift.ratioMultiplier(); }`. Per channel use `inputResamplers[ch]` and `setRatioMultiplier(inMult)`. **Sign note (verified):** the SAME `DriftController` convention works for input — "input ring too full → smaller ratio (fewer engine samples produced) → ring drains." No sign flip. Replace every `inputSRCs` reference (build/reset/latency `getInputSrcLatencyEngineSamples`) → `inputResamplers`; `grep -n inputSRCs Source/Engine/DeviceWorker.*` must be empty.
- [ ] **Step 4: Build the full app.** `cmake --build build -j` clean; `grep inputSRCs` empty. No unit test (live callback); verified by build + later real-device. clang-format clean. Commit (trailer: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`).
- [ ] **Step 5: Remove `SampleRateConverter`** if now unused (`grep -rn SampleRateConverter Source/` → only the file itself). Drop the files + the CMake source entry, or leave if still referenced. Commit.

### Task 2: Make the matrix free-running (the core change)

**Files:** `Source/Engine/MatrixProcessor.{h,cpp}`

> **Optional faster interim (lower risk, can ship before full free-running):** the confirmed root cause is purely the **output backpressure stall**, so the multi-output corruption can be cut WITHOUT the host-clock rewrite by changing output-full handling from "stall the whole matrix" to "**drop just that device's block** (count it) and keep producing for the others." ⚠️ Keep the stall-brake ONLY when **no hardware input** is present (app-tap-only case — the #28 spin), e.g. `if (anyHardwareInput) dropPerDeviceOnFull; else keepStallBrake;`. This decouples a 48k speaker from a bursty BT out immediately (the user's exact case has a hardware input, the Aural ID Bridge) while preserving the anti-spin guard. The full free-running below then supersedes it and removes the special-case. If the user wants relief sooner, do this first as its own commit + real-device check, then proceed.

- [ ] **Step 1: Read `MatrixProcessor::threadLoop()` and `tryProcessOneBlock()` in full** before editing. Note the current `inputReady.wait`, the `drainPerWake` loop, the input-stall checks (`planMatrixInput().stalls`), and the output-backpressure check (`matrixOutputStalls`).
- [ ] **Step 2: Rewrite `threadLoop()` to free-run.** Pace on a monotonic host clock with a drift-free accumulator:

```cpp
void MatrixProcessor::threadLoop()
{
    juce::Thread::setCurrentThreadName ("dcr.Matrix");
    pthread_set_qos_class_self_np (QOS_CLASS_USER_INTERACTIVE, 0);
    const double blockPeriodSec = (double) blockSize / juce::jmax (1.0, sampleRate);
    setRealtimeSchedule (blockPeriodSec);

    using clock = std::chrono::steady_clock;
    auto next = clock::now();
    const auto period = std::chrono::duration_cast<clock::duration> (
        std::chrono::duration<double> (blockPeriodSec));

    while (running.load (std::memory_order_relaxed))
    {
        processOneBlock();          // always produces exactly one block (no stall)
        next += period;             // accumulator -> no long-term drift
        std::this_thread::sleep_until (next);
        // If we fell behind (next is in the past), resync so we don't spin a burst.
        const auto now = clock::now();
        if (next < now)
            next = now;
    }
}
```

- [ ] **Step 3: Rewrite `tryProcessOneBlock()` → `processOneBlock()` (no gating).** Remove BOTH the input-stall bail and the `matrixOutputStalls` bail. Inputs: read a full block if available, else `clear()` to silence (NEVER stall) — for every input (hardware and app-tap alike; `planMatrixInput`'s `stalls` path is gone). Outputs: write the block to each output ring; if a ring can't take it (transient), count an overrun/drop and move on (do NOT stall). It returns void (always produces).
- [ ] **Step 4: Retire backpressure + input-gating.** Delete `matrixOutputStalls` (MatrixInputPlan.h) and its test, OR keep `planMatrixInput` only for the Read-vs-Silence decision (drop `stalls`). Update `CoreLogicTests` accordingly. The input-ready event can stay (harmless) or be removed since the loop no longer waits on it.
- [ ] **Step 5: Build + tests + commit.** Full build clean; `ctest` green (update/remove the backpressure + input-stall tests). clang-format clean.

### Task 3: Verify (real-device — the whole point)

- [ ] **Step 1: Build + all tests + format check** (CI parity).
- [ ] **Step 2: Launch, sanity** — speaker-only still clean (no regression).
- [ ] **Step 3: THE multi-output test (user-driven):** add WF-1000XM5 as a 2nd output with music routed to the **speaker only**. Expected: **speaker stays clean** (no pitch, no crackle) — the Phase-1 failure is gone. Watch the log: each output ring holds near its own set-point independently; `drop`/`xrun` ~0.
- [ ] **Step 4: WF routed:** route music to the WF. Expected: correct pitch, no crackle, sustained. A/B the drift-comp toggle.
- [ ] **Step 5: Multi-input / app-tap:** confirm the app-tap-only case (the #28 scenario) no longer spins (free-running can't spin) and a hardware+app-input mix is clean. Confirm no `PERF SPIKE`/runaway.

---

## Risks & notes
- **Pacing precision:** `sleep_until` on a host clock is the engine clock now; the per-device controllers absorb host-vs-device drift. If `sleep_until` jitter is audible, consider a tighter wait (busy-wait the last sub-ms) — but measure first.
- **Underrun on startup:** inputs read silence until their rings prime; expected, brief.
- **This removes the input-ready event-driven design** the engine had — that's intentional (free-running). Keep the RT scheduling.
- **app-tap spin (#28):** subsumed — a timer-paced matrix never spins. Confirm in Task 3 Step 5 before deleting `matrixOutputStalls`.
- **Keep `asrcDriftComp` as the A/B/kill switch** throughout.

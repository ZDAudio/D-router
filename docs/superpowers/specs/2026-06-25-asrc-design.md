# ASRC — Asynchronous Sample-Rate Conversion (design spec)

- **Date:** 2026-06-25
- **Status:** Approved design; ready for implementation planning.
- **Supersedes:** `ASYNC_SRC_PLAN.md` (the earlier output-only / Catmull-Rom Part-B sketch — see "Changes from the old plan" below).

## Motivation

D-Router routes between devices that are **independent clock domains**. Today each
device uses a **fixed-ratio** sample-rate converter (`SampleRateConverter`, a
CoreAudio `AudioConverter`) configured **once** at `open()` from the device's
nominal rate. That assumes the device's real clock equals its nominal rate
forever. Real hardware drifts; **Bluetooth** drifts a lot and its real rate can
sit materially off its nominal rate.

Concrete failure (WF-1000XM5 earbuds, out): engine 48 k, device opens at 44.1 k.
Through D-Router the audio is **pitch-shifted + crackly**; played by macOS
directly it is **fine**. Decisive evidence: setting the engine to 44.1 k (so the
nominal ratio matches and the SRC bypasses) **did not fix it** → the problem is
not the nominal ratio, it is the device's **actual clock** drifting from nominal.
A fixed-ratio SRC cannot track that; macOS's normal output path adapts
(continuous resampling) and ours does not.

## Problem statement

A fixed-ratio converter between two independent clocks lets the ring buffer
slowly drain or fill (underrun/overflow → clicks), and cannot correct a steady
rate offset (→ pitch error). We need **continuous, drift-tracking resampling**.

## Goals

- Replace the fixed-ratio per-device SRC with a **variable-ratio resampler**
  whose ratio is continuously trimmed by a control loop that tracks each
  device's real clock.
- Make mixed-rate and loose-clock devices (Bluetooth, 44.1↔48, two unsynced
  interfaces) play and capture cleanly through D-Router.
- Apply to **both input and output** paths.

## Non-goals

- **Not** fixing Bluetooth's inherent latency or occasional large rebuffering.
  ASRC corrects steady clock drift, not transport hiccups — BT will improve but
  may not become indistinguishable from a wired device.
- **No** "clock source" / master-device UI. The engine free-runs.
- **No** change to the user-facing engine sample-rate setting.

## Architecture

### Clock model — free-running engine

The engine **free-runs at its nominal block rate** (the matrix thread's
real-time schedule). Every device async-resamples to/from the engine rate, and a
**per-device control loop** trims that device's resampler ratio to hold its ring
at a target fill. There is **no master device** — any device (including a flaky
BT clock) can join, because each is corrected independently against the engine's
flow.

This **replaces today's input-gated matrix** (`tryProcessOneBlock` waits until
every input ring has a full block) with a free-running matrix whose inputs
async-fill and whose outputs async-drain. (This switch is the riskiest change and
is deferred to Phase 2 — see Phasing.)

### Per-device variable-ratio resampler (libsamplerate)

Each device channel gets a **libsamplerate** converter (BSD-2; pulled via CMake
`FetchContent`, like JUCE). Quality `SRC_SINC_MEDIUM` by default (configurable).
The resampler does the **nominal conversion** (e.g. 48→44.1) **and** the live
drift trim in one, via libsamplerate's variable `src_ratio`. `src_new()` (which
allocates) runs in `open()`; `src_process()` on the audio thread is
allocation-free. This replaces `SampleRateConverter` on both the input and
output SRC arrays in `DeviceWorker`.

### Per-device drift control loop (PI)

One controller **per device** (shared across that device's channels — they share
one hardware clock). Each callback: read the device's ring fill, **low-pass** it
(EMA over ~0.5–1 s), compute error = `fill − target`, run a slow **PI**
controller → a **±ppm** trim (hard-clamped, e.g. ±500 ppm), and set
`ratio = baseRatio × (1 + ppm·1e-6)`. Loop time-constant ~1–5 s so it tracks
drift without modulating pitch. Anti-windup on the integrator.

## Data flow

- **Input:** device callback → per-channel async resampler (deviceRate →
  engineRate, ratio trimmed by the input control loop holding the input ring at
  target) → input ring → matrix.
- **Output:** matrix → output ring → device callback pulls → per-channel async
  resampler (engineRate → deviceRate, ratio trimmed by the output control loop
  holding the output ring at target) → device.

## Components / files

| File | Change |
|---|---|
| `Source/Engine/AsyncResampler.{h,cpp}` | **New.** Per-channel libsamplerate wrapper: variable ratio, pre-sized scratch, RT-safe `process()`, `getLatencySamples()`. |
| `Source/Engine/DriftController.h` | **New, JUCE-free.** PI controller: low-passed ring-fill → clamped ±ppm trim. Unit-testable in `CoreLogicTests`. |
| `Source/Engine/DeviceWorker.{h,cpp}` | Replace `SampleRateConverter` arrays with `AsyncResampler`; add one `DriftController` per device; feed it ring fill each callback. |
| `Source/Engine/MatrixProcessor.cpp` | **Phase 2:** free-running loop (drop input gating); keep/adapt output backpressure (`matrixOutputStalls`). |
| `Source/Engine/EngineSettings.h` | New fields (below) + include in `audioPathEquals()`. |
| `Source/Persistence/SettingsStore.cpp` | Persist the new fields. |
| `Source/UI/SettingsDialog.cpp` | Expose quality + drift-comp toggle (advanced: target ms / max ppm). |
| `CMakeLists.txt` | `FetchContent` libsamplerate; link to engine + test targets. |
| `Source/Engine/AudioEngine.cpp` | Latency report = ring set-point + resampler latency. |
| `Source/Engine/SampleRateConverter.{h,cpp}` | Kept as the Phase-1 input-side converter; removed once Phase 2 lands. |

## Settings (EngineSettings)

- `asrcQuality` — libsamplerate quality (`SRC_SINC_FASTEST/MEDIUM/BEST`), default `MEDIUM`.
- `asrcTargetMs` — ring set-point (controlled latency); default derived from `outputPreFillBlocks`.
- `asrcMaxPpm` — drift-trim clamp; default 500.
- `asrcLoopTimeConstS` — control-loop time constant; default ~2.0.
- `asrcDriftComp` — bool, default **true**. When false, the resampler runs at the
  fixed nominal ratio (trim frozen) — an A/B / debug escape hatch. (The resampler
  itself is always the converter; this only freezes the drift correction.)

All included in `audioPathEquals()` so toggling forces a clean engine restart.

## RT-safety

- No heap alloc / locks on the audio thread. `src_new()` + all scratch sized in
  `open()`. Confirm `src_process()` is alloc-free after init.
- Control loop reads `readAvailable()` (lock-free SPSC) and **low-passes** the
  fill before the PI — without this the loop chases per-block ripple and
  modulates pitch (the main failure mode).
- Hard-clamp the ratio (`±maxPpm`) so a transient can't command a wild ratio.
- Underrun still feeds zeros and increments the existing counters.
- Resampler history persists across callbacks (it is device-stream state), reset
  only in `open()`.

## Latency

The ring target fill becomes the **real, intentional** output (and input)
latency, not just a cushion. Update `AudioEngine::LatencyReport` so the Engine
Monitor reports the set-point + resampler latency honestly.

## Phasing

- **Phase 1 — output side (fixes Bluetooth; lower risk).** Replace `outputSRCs`
  with `AsyncResampler` + an output `DriftController`. Engine stays **input-gated**
  (current clock model). A/B against current via `asrcDriftComp`. Ship and
  verify the WF-1000XM5 case before touching the core clock.
- **Phase 2 — input side + free-running clock (higher risk).** Replace
  `inputSRCs` with `AsyncResampler` + input `DriftController`; switch the matrix
  to **free-running** (drop input gating); handle multi-input drift (e.g. Aural
  ID Bridge + an app-tap drifting against each other). Verify thoroughly.

Both phases are covered by this one spec; the implementation plan sequences them.

## Testing / verification

- **Unit (JUCE-free, `CoreLogicTests`):** `DriftController` — error→ppm sign,
  clamp, anti-windup, low-pass behaviour, convergence to target; ratio math.
- **JUCE (`*_juce`):** `AsyncResampler` round-trip fidelity (THD on a steady
  tone), smoothness under a changing ratio, reported latency.
- **Real-device (user):** steady tone 30+ min across two unsynced 48 k devices
  → no drift xruns, no audible pitch wobble; WF-1000XM5 (out) → correct pitch,
  no crackle; A/B `asrcDriftComp` on/off.

## Risks & mitigations

- **Core clocking rewrite (Phase 2 free-running)** — highest risk. Mitigate:
  phase it; ship Phase 1 independently; `asrcDriftComp` A/B; heavy tone tests.
- **libsamplerate dependency** — BSD-2 (fine for the proprietary app); verify
  `src_process` alloc-free; integrate via FetchContent.
- **CPU** — sinc resampling per channel; measure vs the current AudioConverter,
  pick the quality default accordingly.
- **Bluetooth variable latency beyond drift** — ASRC won't fully fix; expectations set in Non-goals.
- **Pitch wobble from a too-fast loop** — low-pass the fill + slow time-constant;
  this is the single most important tuning detail.

## Changes from the old `ASYNC_SRC_PLAN.md`

- **Both** input and output (old plan was output-only).
- **libsamplerate polyphase sinc**, not hand-rolled Catmull-Rom. Catmull-Rom was
  acceptable only for ±ppm trims around ratio 1.0 (matched-rate drift); it
  aliases/images on a real 48→44.1 (ratio 1.088) conversion, which the unified
  resampler must do.
- **Free-running engine** clock model (old plan kept the input-gated matrix and
  noted the watchdog restart as "unaffected").

## Open questions (resolve during planning)

- Exact free-running matrix wake-up mechanism (RT timer vs output-callback pull).
- Per-channel resampler state vs one shared per device (control is per-device;
  resampling is per-channel — confirm the split).
- Whether to keep `SampleRateConverter` as a permanent fallback or delete after
  Phase 2.

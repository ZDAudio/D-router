# Stereo 3D Pan Scatter meter — design & status

**Status:** Phase A + first Phase-B slice shipped to `main` (squash PR #20, commit `e0cfd6e`, 2026-06-22).
**Scope:** A hidden, per-group 3D stereo-field visualization, embedded natively in D-Router.

---

## 1. Goal

Embed the "Stereo 3D Pan Scatter" meter (originally a Swift/SwiftUI/Metal app
in a sibling project) into D-Router **natively**, as a built-in plugin a user
drops on a **stereo output group**. It plots the group's stereo field in 3D:

- **X** = L/R pan (−1 left … +1 right)
- **Y** = log frequency (20 Hz → 20 kHz)
- **Z** = level (intensity, dB-windowed)
- **colour** = phase coherence (red anti-phase ↔ white ↔ green in-phase)

It is a *meter*: audio passes through unchanged.

### Why native (not the Swift app)

D-Router is C++20 / JUCE 8 / Metal-capable. The Swift renderer can't be linked
into it cleanly. Decision: **rewrite in ObjC++**, reuse Metal directly (no
OpenGL — deprecated on macOS), host the `MTKView` in JUCE via
`juce::NSViewComponent`. The DSP (~250 lines of FFT math) ports trivially to
C++/`juce::dsp::FFT`.

---

## 2. Architecture

It is an `InternalPluginFormat` built-in, so it reuses the entire plugin-slot
machinery (instantiate, bypass, snapshot save/restore, editor window) with
almost no glue — same rationale as `BUILTIN_PLUGINS_PLAN.md`.

```
stereo output-group slot
  └─ StereoMeterProcessor (BuiltinProcessor, pass-through)
        processBlock: tap L/R → two lock-free dcr::FloatRingBuffer  (RT-safe, no alloc/lock)
  └─ StereoMeterEditor (juce::AudioProcessorEditor)
        juce::Timer @60Hz (UI thread): drain rings → StereoFreqAnalyzer (FFT) → build vertices
        DCRScatterMTKView (Metal, via NSViewComponent): point cloud + trails + stems + room + orbit camera
        StereoControls sidebar: 11 APVTS sliders + axis legend
```

**RT safety:** the only audio-thread work added is two pre-sized `ring.write()`
calls. FFT + render run on the UI thread. The engine (`MatrixProcessor`,
workers, RT path) is **untouched** — `ctest` stays green.

### Files (all under `Source/`)

| File | Role |
|---|---|
| `DSP/Builtin/StereoMeterProcessor.h` | built-in pass-through processor; per-instance L/R rings; APVTS params |
| `DSP/Builtin/StereoFreqAnalyzer.h` | C++ FFT → per-bin pan / coherence / intensity (+ floor/ceiling, smoothing) |
| `DSP/Builtin/StereoMeterEditor.{h,mm}` | ObjC++ Metal renderer + MTKView + timer + sidebar (ARC; pimpl keeps the header plain C++) |
| `UI/ZDFace.h` | the "ZD" unlock smiley (JUCE-drawn, ported from the sibling project) |
| `DSP/Builtin/BuiltinProcessors.h` | `ids::stereo_meter` |
| `DSP/Builtin/InternalPluginFormat.{h,cpp}` | `makeById` + `createEditor` + `builtinDescriptionForId` (NOT in `allIds`) |
| `UI/OutputGroupPanel.cpp` | gated "Stereo Meter" menu item |
| `Engine/AudioEngine.h` | `std::atomic<bool> stereoMeterUnlocked` + get/set |
| `MainComponent.{h,cpp}` | `SecretTitle` (5-click) → unlock + show `ZDFace` |
| `CMakeLists.txt` | `-framework Metal/MetalKit`; the new `.mm` gets `-fobjc-arc` per-file |

### Key decisions

- **Hidden / gated.** `stereo_meter` is registered in `makeById` (so it can be
  instantiated and restored from snapshots) but **excluded from
  `getBuiltinDescriptions()`** — it never appears in the normal Built-in list.
  It surfaces only as a separate "Stereo Meter" item, shown **only when**
  `engine.isStereoMeterUnlocked()` **and** the group's `channelSet.size() == 2`.
  Unlock = click the "ZDAudio D-Router" brand title 5× (within 0.6 s steps),
  which also shows the ZD mark next to the title.
- **ObjC++ + ARC, scoped per-file.** The rest of the project's `.mm` files use
  manual retain/release; only the meter `.mm` is ARC (CMake
  `set_source_files_properties`).
- **Public-repo hygiene.** No third-party brand names in code/commit/PR text
  (per `CLAUDE.md`). "ZD"/"ZDAudio" is first-party and fine.
- **Vertex layout.** `float[3]/float[4]` structs match MSL `packed_float3/4`
  byte-for-byte; runtime-compiled shader (`newLibraryWithSource:`), embedded as
  a string (no shader-resource bundling).

---

## 3. Parameters (APVTS — saved in snapshots, sidebar sliders)

`floorDb`, `ceilDb` (dB intensity window) · `highLift` (raises high-freq points
so quiet highs are visible — `intensity × (1 + lift·freqNorm·3)`) · `pointMin`,
`pointMax` (sprite size range) · `heightScale` (Z) · `smooth` (one-pole temporal
smoothing) · `colorSat` (phase-colour saturation) · `trailDepth`, `trailDecay`
(motion trail) · `stemAmount` (drop-line stems; 0 = off).

---

## 4. Shipped vs. remaining

**Shipped (PR #20):** processor + tap, C++ analyzer, Metal point cloud, **trails**,
**stems**, orbit camera + mouse (drag = orbit, scroll = zoom), 11-param sidebar,
axis legend, ZD-smiley unlock gate.

**Remaining — Phase B (next, in rough priority):**

1. **Floor-band correlation overlay** — per-frequency-band L/R correlation
   tinting the Z=0 floor. Needs `BandCorrelationAnalyzer` ported to C++ (the
   processor would also tap/keep what the band analyzer needs).
2. **Band-centroid effects** — lateral motion trail + spectral-centroid polyline
   (the sibling app's `bandTrail` / `bandLine`).
3. **Mode A / B** — switch Z-axis encoding (level vs. coherence×level) + the
   matching colour mode.
4. **On-plot axis ticks** — real frequency/pan tick labels that track the orbit
   camera. Needs Metal text (font atlas) — the current "coordinates" are a
   static sidebar legend because a JUCE overlay can't draw over the native
   `MTKView`. This is the heaviest remaining item.

**Separate future item:** when a group is **7.1.4**, unlock a second hidden
meter — **Cloud 3D Room Space** (the sibling `PointCloud3DPanel` / spatial
point cloud). Same recipe (built-in + Metal editor), gate on the 7.1.4 layout.

---

## 5. Related work (sibling projects, not in this repo)

The same meter also exists as a standalone open-source path:
**AtmosCore** (a reusable Swift package extracted from the ATMOSMETER monolith)
+ **StereoScatter** (a thin standalone macOS app on top of it). That line is
built + verified locally but **not yet pushed to GitHub** (pending repo
name / license / handle decisions). See the workspace plan file
`~/.claude/plans/atmosmeter-stereo-3d-idempotent-steele.md` for that design.

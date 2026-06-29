# Design — Plugin editor window scaling + Stereo Meter 2D modes

**Date:** 2026-06-29
**Status:** Approved (brainstorming), pending spec review

Two independent UI features, captured in one spec. They share no code and can be
implemented / PR'd separately.

---

## Feature 1 — Plugin editor window: title-bar overlap fix + aspect-locked scaling

### Problem

`PluginEditorWindow` (`Source/UI/PluginEditorWindow.{h,mm}`) hosts a plugin's
`AudioProcessorEditor` in a `juce::DocumentWindow` with a **native macOS title
bar** (`setUsingNativeTitleBar(true)`). Two issues observed on a real device with
a fixed-size AU (Schwabe Digital "Gold Clip"):

1. **Title-bar overlap** — the macOS title bar covers the top ~few pixels of the
   plugin GUI (its top toolbar/logo row is clipped).
2. **No scaling** — fixed-size plugins are pinned to natural size
   (`setResizeLimits(natW, natH, natW, natH)` in the `else` branch of
   `installEditor`, `PluginEditorWindow.mm:173`). The user cannot enlarge/shrink
   the GUI at all.

### Current behavior (reference)

`installEditor` (`PluginEditorWindow.mm:132-176`):
- `setContentNonOwned(ed, true)` — resizes window to the editor's natural size.
- `natW/natH` = `ed->getWidth()/getHeight()`.
- If `ed->isResizable()`: honor the plugin's `getConstrainer()` min/max, else pin
  min at natural and allow up to 8× natural.
- Else (fixed-size): `setResizeLimits(natW, natH, natW, natH)` → locked.
- `centreWithSize(getWidth(), getHeight())`.

**Hard constraint (already documented in the file, must not regress):** do **not**
call `setResizable()` a second time after the plugin NSView is attached. JUCE's
`ResizableWindow::setResizable` re-invokes `addToDesktop` when already on the
desktop, which re-migrates the plugin NSView and crashes some plugins. Express
all resize policy through `setResizeLimits` / `setConstrainer`, which do **not**
touch the desktop.

### Design

**1a. Title-bar overlap.**
Ensure the editor sits fully **below** the native title bar. Rather than relying
on `setContentNonOwned(ed, true)`'s resize-to-fit (which is where the off-by-
title-bar-height creeps in), size the window explicitly to
`contentSize + getContentComponentBorder()` (the border JUCE reserves for the
native title bar) and let the document window place the content component within
that border. Verify the plugin's top row is no longer clipped.

> Device-observable. Flag for the user's real-device check — this cannot be
> confirmed headless.

**1b. Aspect-locked free resize (drag corner + lock ratio).**

- Install a `juce::ComponentBoundsConstrainer` (owned by the window) configured
  with **fixed aspect ratio = natW : natH** via
  `setFixedAspectRatio((double) natW / natH)`, plus min/max bounds
  (proposed: 0.4× … 3.0× natural, clamped to ≥ 1px). Attach with
  `setConstrainer(...)`. **No `setResizable` re-call** (see hard constraint).
- On every resize, compute `scale = currentContentWidth / natW` and apply
  `editor->setTransform(juce::AffineTransform::scale((float) scale))` so the GUI
  actually grows/shrinks. The editor's logical bounds stay `natW × natH`; the
  transform scales its rendered output. This works for fixed-size plugins (Gold
  Clip) because we scale the host-side view rather than asking the plugin to
  re-lay-out.
  - Hook point: override `resized()` on the window (or attach a lightweight
    resize listener) to recompute and apply the transform, then keep the content
    component bounds consistent with the scaled size.
- **Resizable plugins** (`ed->isResizable()` with their own constrainer) keep
  their native min/max behavior unchanged — we only layer aspect-lock + transform
  scaling onto the **fixed-size** branch. (Decision: do not interfere with
  plugins that already know how to resize themselves.)

> Caveat: `AffineTransform` scaling of a hosted NSView is GPU-scaled, so a
> magnified GUI may look mildly soft, and a plugin that ignores host transforms
> entirely would not scale. Both are acceptable and must be confirmed on a real
> device.

### Out of scope
- Persisting per-plugin window size/scale across sessions (could be a follow-up).
- Discrete zoom presets (the user chose free drag-resize instead).

---

## Feature 2 — Stereo Meter: 2D Front + RTA modes

### Problem / goal

The Stereo Meter (`Source/DSP/Builtin/StereoMeterEditor.{h,mm}`) renders a Metal
**3D point cloud** with **X = pan**, **Y = frequency (log)**, **Z = intensity**,
using a `perspective()` projection and an orbit camera (yaw/pitch/zoom). The user
wants two **2D, parallel-projection (orthographic)** views with no perspective:

- **Front** — looking at the pan×frequency face.
- **RTA** — a traditional analyzer view: **frequency horizontal, level vertical.**

### Current behavior (reference)
- `perspective(fovy, aspect, n, f)` and `lookAt(eye, centre, up)` build the MVP
  (`StereoMeterEditor.mm:60-82`, assembled `:456-458`).
- Orbit camera state `camYaw/camPitch/camDist` driven by mouse drag + scroll
  (`DCRScatterMTKView`, `:175-214`).
- `timerCallback` (`:707-832`) drains the L/R rings → `StereoFreqAnalyzer` →
  `Frame` (per-bin `freqs/pans/cohs/ints`), applies HF tilt via
  `highLiftGain(...)`, and builds `PointVertex{pos[3], color[4], size}` with
  `pos = (pan, 2*freqNorm-1, intensity*kZScale*heightScale)`.
- Frequency axis: camera-tracked billboard labels + tick lines, laid out for the
  3D scene (`:372-429`, `:472-543`). `axisOpacity` param fades them.
- 13 APVTS params + Save/Reset in `StereoControls` (`:558-644`); user defaults in
  `~/.config/dcorerouter/StereoMeterDefaults.xml`. Factory-state capture order is
  an invariant (`StereoMeterProcessor.h:20-26`).

### Design

**New parameter `viewMode`** (choice / int 0–2): `0 = 3D` (current), `1 = Front`,
`2 = RTA`. Added to the APVTS like the other params so it is saved with snapshots
and with Save / reset by Reset, **default 0 (3D)**. Respect the factory-state
capture order invariant.

**Sidebar control.** Add a **three-segment selector** (3D / Front / RTA) at the
**top of the `StereoControls` sidebar**, above the existing 13 sliders. Bind it to
`viewMode`. Adjust the sidebar layout (`:617-633`) to reserve a row for it.

**Projection.** Add an `ortho(left, right, bottom, top, near, far)` matrix builder
beside `perspective()`. Select projection by mode in the MVP assembly:
- 3D → `perspective(...) * lookAt(eye, target, up)` (unchanged).
- Front / RTA → `ortho(...) * lookAt(fixedEye, planeCentre, up)` with the camera
  locked square-on to the flat XY plane. Orthographic extent scaled by an ortho
  zoom factor.

**Camera / interaction in 2D.** Orbit drag **disabled** in Front/RTA (no
yaw/pitch). **Scroll-zoom kept**, repurposed to scale the orthographic extent.
Switching back to 3D restores the existing orbit camera state.

**Per-mode geometry** (reuse the existing per-bin `Frame` + phase-coherence point
colors — no new analysis, decision: "沿用现有按 bin 的点云"). In `timerCallback`,
compute `PointVertex.pos` based on `viewMode`:

- **3D** (unchanged): `pos = (pan, 2*freqNorm-1, it*kZScale*pHeight)`.
- **Front:** `pos = (pan, 2*freqNorm-1, ~0)`. Flat pan(X) × freq(Y) plane.
- **RTA:** `pos = (2*freqNorm-1, levelY, ~0)` where
  `levelY` maps the bin's intensity through the existing **`floorDb`/`ceilDb`**
  window to vertical screen space (decision confirmed: RTA vertical axis reuses
  floor/ceil dB). `freqNorm` drives the **horizontal** axis. Pan is dropped from
  position but still encoded in point color (phase coherence). HF-tilt and trail
  still apply; **stems hidden** in RTA.

**Axes per mode.**
- 3D: existing freq axis (vertical, billboarded) unchanged.
- Front: freq axis vertical (reuse current placement); add light left/center/
  right pan guides on the horizontal axis. Labels can be screen-aligned (camera is
  fixed) instead of billboarded.
- RTA: relocate freq ticks/labels to the **horizontal** axis (20 / 100 / 1k / 10k
  / 20k Hz across the bottom); add a **vertical dB scale** derived from
  `floorDb`/`ceilDb`.

**Controls that don't apply in 2D.** `heightScale` (Z) and `stemAmount` have no
effect in Front/RTA. Leave them visible/no-op initially (do not hide) — keeps the
first cut small; hiding/greying is a possible follow-up.

### Out of scope
- L/R as two separate RTA curves (user chose the existing per-bin point cloud).
- Hiding/disabling 3D-only sliders per mode.
- Any change to the audio thread or `StereoFreqAnalyzer`.

---

## Testing

- **Headless logic tests** (`dcorerouter_tests`, `tests/CoreLogicTests.cpp`):
  - Feature 1: pure aspect/scale math if any is factored JUCE-free (e.g. given
    natW/natH and a requested width, the locked height + scale factor). Add only
    if non-trivial deterministic logic is extracted.
  - Feature 2: extend `StereoMeterMath.h` (JUCE-free) with the RTA level→Y mapping
    (intensity + floor/ceil dB → normalized Y) and the front/RTA position mapping,
    and unit-test them. Keep the math JUCE-free so it links into the test target.
- **Real-device / visual verification** (only the user can confirm):
  - F1a: title bar no longer clips the plugin's top row.
  - F1b: drag-resizing a fixed-size AU scales the GUI with locked aspect ratio,
    no crash on open/resize/close; resizable plugins unaffected.
  - F2: Front and RTA render correctly with parallel projection; selector switches
    modes; mode persists across reopen / Save; orbit disabled but zoom works in
    2D; freq + dB axes correct in RTA.

## Invariants to respect
- No second `setResizable()` after the plugin NSView is attached (F1).
- Plugin editor lifetime: close editor before the plugin is freed (existing).
- StereoMeter factory-state capture order (`StereoMeterProcessor.h:20-26`) when
  adding `viewMode` (F2).
- RT-safety unaffected — both features are message-thread / GPU only.
- clang-format every changed file (CI fails otherwise).
- Public-repo brand hygiene: no third-party plugin brand names in committed text.

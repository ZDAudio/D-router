# Stereo Meter — HF tilt, 3D frequency axis, axis opacity, Save/Reset — design

**Status:** Design approved 2026-06-22; not yet implemented. Follow-up to the
shipped meter (`2026-06-22-stereo-meter-design.md`, PR #20).
**Scope:** Four changes to the hidden Stereo 3D Pan Scatter meter, all on the
**UI/Metal side only**. The audio tap (`processDsp`) and the engine/RT path are
**not touched**; `ctest` stays green.

---

## 1. Problems (from the user)

1. **Perceptual mismatch.** Highs *sound* present but render small/dim, because
   `intensity` is raw dB energy and real program material has a falling spectrum.
2. **"High lift" also raises the bass.** The current curve in
   `StereoMeterEditor.mm`:
   ```cpp
   const float freqNorm = (float) i / (float) (N - 1);          // 0 low … 1 high
   const float it = std::min(1.0f, intensity * (1.0f + pHighLift * freqNorm * 3.0f));
   ```
   ramps **linearly from the bottom of the spectrum**. Bins are log-spaced
   (20 Hz → ~Nyquist), so `freqNorm` ≈ 0.3 already at ~170 Hz and ≈ 0.5 at
   ~700 Hz → a 1.3×–1.5× boost across bass/low-mids, the loudest (most visible)
   content. Only the single lowest bin gets zero lift. Goal: lift **only true
   highs**, aggressively ("尽量抬高音").
3. **Y axis frequency is unreadable.** Only a static sidebar text line exists; no
   tick labels at real heights.
4. **No Save / Reset.** Want a button to persist current settings as the default
   for future meters, and a button to restore factory defaults.

---

## 2. Design

### Part A — High-frequency tilt (fixes #1 and #2)

Replace the linear ramp with a **pivot-anchored** curve keyed on true frequency.
Pure functions live in a new JUCE-free header `Source/DSP/Builtin/StereoMeterMath.h`:

```cpp
namespace dcr::builtin
{
    inline constexpr float kHighLiftMax = 6.0f; // max extra gain factor at full strength + Nyquist

    // Log position of hz within [lowestHz, nyquistHz], clamped to [0,1].
    inline float freqToNorm (float hz, float lowestHz, float nyquistHz) noexcept;

    // Display-intensity gain (>= 1) for the HF tilt. ~1.0 at/below pivot; rises
    // with frequency above it. strength in [0,1].
    //   t    = clamp( log(hz/pivot) / log(nyquist/pivot), 0, 1 )
    //   gain = 1 + strength * t * kHighLiftMax
    inline float highLiftGain (float hz, float pivotHz, float nyquistHz, float strength) noexcept;
}
```

The editor's vertex loop becomes (using `frame.freqs[i]`, which the analyzer
already fills):
```cpp
const float gain = highLiftGain (frame.freqs[i], pLiftPivot, nyquistHz, pHighLift);
const float it   = std::min (1.0f, intensity * gain);
```
Below the pivot `t = 0 → gain = 1` (bass/low-mids untouched, period). Above it the
boost climbs toward the top; with pivot 2 kHz, strength 0.5: ~2.1× at 5 kHz,
~2.9× at 10 kHz, ~3.8× at 20 kHz.

**Rejected alternative:** full equal-loudness / A-weighting — overkill, and it
*attenuates* the extremes, the opposite of what's wanted.

### Part B — 3D on-plot frequency axis (fixes #3)

A new **textured-billboard** path in the Metal renderer (`StereoMeterEditor.mm`):

- **Labels generated once** at editor init: for each tick string, draw it into a
  small `juce::Image (ARGB)` with `juce::Graphics`, then upload to an
  `MTLTexture` (BGRA8, channel-reordered, premultiplied to match the existing
  blend). Only ~5 static strings → one texture per label; no glyph-atlas/UV
  bookkeeping. Store `{ id<MTLTexture>, aspect }` per label (ARC-retained ivars).
- **Tick set:** lines at `{20, 50, 100, 200, 500, 1k, 2k, 5k, 10k, 20k}`,
  labelled subset `{20, 100, 1k, 10k, 20k}`; all filtered to `< Nyquist`.
- **Placement:** each label/tick at its true height `y = 2·freqToNorm(f) − 1` on
  the box's front-left vertical edge (x ≈ −1, z = 0), ticks extending inward.
- **Billboarding:** a new pipeline (`label_vs`/`label_fs`) offsets each quad
  corner by `camRight`/`camUp` (added to `Uniforms`) so labels stay upright and
  readable through orbit/zoom. `camRight`/`camUp` are the lookAt basis vectors
  (`side`, `u`), computed in `drawInMTKView`.
- Tick lines reuse the existing line pipeline (a new static `_freqAxisBuf` built
  once, like `_boxBuf`).

### Part C — Axis opacity (the transparency slider)

New param `axisOpacity` (0–1, default 0.85) multiplies the alpha of the
**frequency tick lines + labels only** — i.e. exactly "the frequency axis."
The room wireframe (`_boxBuf`) and the point cloud are left independent (approved
2026-06-22). Implemented as a per-draw `u.alphaMul`/`opacity` for the freq-axis
and label draws.

### Part D — Save / Reset (fixes #4)

`StereoMeterProcessor` (subclass of `BuiltinProcessor`, which owns `apvts`):

- **Constructor order (the invariant):**
  1. base ctor builds `apvts` with factory defaults,
  2. ctor body captures `factoryState = apvts.copyState();` (independent copy),
  3. `loadUserDefault();` — if the defaults file exists, parse → `apvts.replaceState(...)`.
- `void saveUserDefault()` — `if (auto xml = apvts.copyState().createXml()) writeXmlAtomically(*xml, defaultsFile());`
- `void resetToFactory()` — `apvts.replaceState (factoryState.createCopy());`
- `static juce::File defaultsFile()` — `userApplicationDataDirectory/dcorerouter/StereoMeterDefaults.xml` (mirrors `SettingsStore::getFile`, `createDirectory()` first).

**Why constructor (not editor) load:** snapshot restore calls
`setStateInformation` *after* construction, so a saved project still overrides
the user-default correctly; a fresh menu-add gets no `setStateInformation` and
keeps the user-default. (Documented as a non-negotiable load-order invariant.)

**Sidebar:** `StereoControls` is constructed with `StereoMeterProcessor&`
(instead of just the APVTS) so it can call `saveUserDefault()` /
`resetToFactory()`. Two `juce::TextButton`s ("Save", "Reset") in a reserved row;
`resized()` updated. APVTS `SliderAttachment`s auto-refresh the sliders when
`replaceState` fires, so Reset visibly updates the UI with no extra wiring.

---

## 3. Parameters (added to `StereoMeterProcessor::createLayout`)

| id | name | range | default | notes |
|---|---|---|---|---|
| `highLift` | High lift | 0…1 | **0.5** (was 0.35) | strength; reinterpreted by new curve |
| `liftPivot` | Lift pivot | 200…8000 Hz, log-skew (centre ~1.5 k) | 2000 | freq below which there is ~no lift |
| `axisOpacity` | Axis opacity | 0…1 | 0.85 | freq ticks + labels only |

Sidebar `kN` 11 → 13; two `defs[]` entries added (group `liftPivot` after
`highLift`; `axisOpacity` near the display controls). Legend text updated (Y is
now self-labelling).

---

## 4. Tests (`tests/CoreLogicTests.cpp`, JUCE-free target)

`StereoMeterMath.h` is pure `<cmath>`, so it links into the existing pure-logic
target. New cases:

- **`freqToNorm`** — endpoints map to 0 and 1; monotonic increasing; a mid
  frequency lands between.
- **`highLiftGain` (the bug-fix regression)** — `gain ≈ 1.0` for any `hz ≤
  pivot` (e.g. 100 Hz, 500 Hz, 1 kHz with pivot 2 kHz); strictly increasing above
  the pivot (`gain(5k) < gain(10k) < gain(20k)`); `strength = 0` → `gain == 1`
  everywhere; clamps hold at/above Nyquist.

Save/Reset is JUCE-`File`/APVTS I/O (not in the JUCE-free target) → verified by
build + real-app test, stated honestly.

---

## 5. Files touched

| File | Change |
|---|---|
| `Source/DSP/Builtin/StereoMeterMath.h` *(new)* | pure `freqToNorm`, `highLiftGain`, `kHighLiftMax` |
| `Source/DSP/Builtin/StereoMeterProcessor.h` | +`liftPivot`,+`axisOpacity`; `highLift` default 0.5; `factoryState`; `saveUserDefault`/`resetToFactory`/`loadUserDefault`/`defaultsFile` |
| `Source/DSP/Builtin/StereoMeterEditor.mm` | new lift curve (via math header); label-texture gen; billboard textured pipeline + `camRight/camUp/opacity` uniforms; freq tick-line buffer; draw ticks+labels w/ `axisOpacity`; `StereoControls` takes processor, `kN` 13, two defs, Save/Reset buttons, updated legend/`resized` |
| `tests/CoreLogicTests.cpp` | cases for the two math functions |

No engine/RT/persistence-schema files change; `SnapshotStore`/`SettingsStore`
are reused, not modified (`writeXmlAtomically` is the only persistence helper
called).

---

## 6. Safety / non-goals

- **RT-safety:** all new work is UI-thread + Metal. `processDsp` is unchanged
  (still just the two ring writes). `apvts.replaceState` on the message thread is
  the same path snapshot-restore already uses; the audio thread only reads raw
  atomics and this meter's `processDsp` doesn't read display params at all.
- **Snapshot compat:** the two new params are picked up automatically by APVTS
  XML; older snapshots without them load fine (params keep their defaults).
- **Public-repo hygiene:** only frequency numerals on labels; no brand text.
- **Non-goals:** named/multi presets, file-chooser import/export, equal-loudness
  weighting, controlling the room-box or point-cloud opacity from `axisOpacity`.

---

## 7. Verification plan

- `cmake --build build -j` clean.
- `ctest` green, including the new `StereoMeterMath` cases.
- Real-app (user): drop the meter on a stereo group, confirm (a) highs lift while
  bass stays put as `Lift pivot` moves, (b) frequency labels sit at correct
  heights and stay readable while orbiting, (c) `Axis opacity` fades only the
  ticks/labels, (d) Save → relaunch → new meter opens with saved settings; Reset
  → factory values restored live.

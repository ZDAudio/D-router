# Stereo Meter — HF tilt, 3D frequency axis, axis opacity, Save/Reset — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the hidden Stereo 3D Pan Scatter meter read closer to human hearing (lift only true highs), give it real on-plot frequency labels with an opacity control, and add Save/Reset of its settings.

**Architecture:** All changes are UI-thread + Metal, inside the meter's built-in plugin. A new JUCE-free math header carries the perceptual curve (unit-tested). The Metal renderer gains a textured-billboard path for frequency labels + tick lines. The processor gains a user-default file (load in constructor so snapshot-restore still wins). The audio tap (`processDsp`) and the engine/RT path are untouched; `ctest` stays green.

**Tech Stack:** C++20, JUCE 8, Objective-C++ / Metal (ARC, per-file), CMake. Design doc: `docs/superpowers/specs/2026-06-22-stereo-meter-hf-axis-design.md`.

---

## File Structure

| File | Responsibility |
|---|---|
| `Source/DSP/Builtin/StereoMeterMath.h` *(new)* | Pure, JUCE-free perceptual math: `freqToNorm`, `highLiftGain`, `kHighLiftMax`. Shared by editor + tests. |
| `tests/CoreLogicTests.cpp` | Add regression cases for the two functions (the "gain≈1 below pivot" case pins the bass-lift bug fix). |
| `Source/DSP/Builtin/StereoMeterProcessor.h` | Two new params + `highLift` default; `factoryState` capture; Save/Reset/load-default + defaults-file path. |
| `Source/DSP/Builtin/StereoMeterEditor.mm` | New lift curve (via math header); textured-billboard label pipeline + label textures + tick-line buffer + draw with `axisOpacity`; sidebar gains two sliders + Save/Reset buttons. |

**Branch:** before Task 1, create the working branch:
```bash
git checkout -b feat/stereo-meter-hf-axis
```

---

## Task 1: Perceptual math header + unit tests (TDD)

**Files:**
- Create: `Source/DSP/Builtin/StereoMeterMath.h`
- Modify: `tests/CoreLogicTests.cpp` (add include near line 10–18; add two `test_*` functions; register them in `main`)

- [ ] **Step 1: Write the failing tests**

In `tests/CoreLogicTests.cpp`, add to the include block (with the other `#include "DSP/Builtin/..."` lines):
```cpp
#include "DSP/Builtin/StereoMeterMath.h"
```
Add these two functions (place them next to the other `test_resonance_*` functions; match the file's existing `static`/namespace style — they are free functions using the `CHECK` macro):
```cpp
void test_stereometer_freq_to_norm()
{
    using dcr::builtin::freqToNorm;
    const float lo = 20.0f, nyq = 24000.0f;
    CHECK (std::abs (freqToNorm (lo, lo, nyq) - 0.0f) < 1e-5f);   // bottom -> 0
    CHECK (std::abs (freqToNorm (nyq, lo, nyq) - 1.0f) < 1e-5f);  // top -> 1
    CHECK (freqToNorm (10.0f, lo, nyq) == 0.0f);                  // below clamps
    CHECK (freqToNorm (48000.0f, lo, nyq) == 1.0f);               // above clamps
    CHECK (freqToNorm (100.0f, lo, nyq) < freqToNorm (1000.0f, lo, nyq));   // monotonic
    CHECK (freqToNorm (1000.0f, lo, nyq) < freqToNorm (10000.0f, lo, nyq));
    const float n1k = freqToNorm (1000.0f, lo, nyq);             // ln(50)/ln(1200) ~= 0.55
    CHECK (n1k > 0.4f && n1k < 0.7f);
}

void test_stereometer_high_lift_gain()
{
    using dcr::builtin::highLiftGain;
    const float nyq = 24000.0f, pivot = 2000.0f;
    // Bug-fix regression: at/below the pivot the gain is ~1 (bass/mids untouched).
    CHECK (std::abs (highLiftGain (100.0f,  pivot, nyq, 0.5f) - 1.0f) < 1e-5f);
    CHECK (std::abs (highLiftGain (500.0f,  pivot, nyq, 0.5f) - 1.0f) < 1e-5f);
    CHECK (std::abs (highLiftGain (2000.0f, pivot, nyq, 0.5f) - 1.0f) < 1e-5f);
    // Above the pivot: > 1 and strictly increasing with frequency.
    const float g5k  = highLiftGain (5000.0f,  pivot, nyq, 0.5f);
    const float g10k = highLiftGain (10000.0f, pivot, nyq, 0.5f);
    const float g20k = highLiftGain (20000.0f, pivot, nyq, 0.5f);
    CHECK (g5k > 1.0f);
    CHECK (g5k < g10k);
    CHECK (g10k < g20k);
    // strength 0 -> no lift anywhere; full strength lifts more than half.
    CHECK (std::abs (highLiftGain (20000.0f, pivot, nyq, 0.0f) - 1.0f) < 1e-5f);
    CHECK (highLiftGain (20000.0f, pivot, nyq, 1.0f) > g20k);
    // bounded by 1 + strength*kHighLiftMax.
    CHECK (g20k <= 1.0f + 0.5f * dcr::builtin::kHighLiftMax + 1e-4f);
}
```
In `main()` (near the other `test_resonance_*();` calls before the final `printf`), register:
```cpp
    test_stereometer_freq_to_norm();
    test_stereometer_high_lift_gain();
```

- [ ] **Step 2: Run the build to confirm the red (compile failure — header missing)**

Run:
```bash
cmake --build build --target dcorerouter_tests
```
Expected: FAILS to compile — `'DSP/Builtin/StereoMeterMath.h' file not found`. (This is the TDD red for a brand-new module.)

- [ ] **Step 3: Create the header (minimal implementation to satisfy the tests)**

Create `Source/DSP/Builtin/StereoMeterMath.h`:
```cpp
#pragma once

#include <algorithm>
#include <cmath>

namespace dcr::builtin
{

    // Max extra display-gain factor at full strength at/above Nyquist.
    inline constexpr float kHighLiftMax = 6.0f;

    // Log position of `hz` within [lowestHz, nyquistHz], clamped to [0, 1].
    // Mirrors the analyzer's log bin spacing so labels and points share one axis.
    inline float freqToNorm (float hz, float lowestHz, float nyquistHz) noexcept
    {
        if (nyquistHz <= lowestHz || hz <= lowestHz)
            return 0.0f;
        if (hz >= nyquistHz)
            return 1.0f;
        return std::log (hz / lowestHz) / std::log (nyquistHz / lowestHz);
    }

    // Display-intensity multiplier (>= 1) for the high-frequency tilt. ~1.0 at or
    // below the pivot (bass/low-mids untouched), rising toward Nyquist. This is a
    // *visualization* weighting, never an audio gain. strength in [0, 1].
    //   t    = clamp( log(hz/pivot) / log(nyquist/pivot), 0, 1 )
    //   gain = 1 + strength * t * kHighLiftMax
    inline float highLiftGain (float hz, float pivotHz, float nyquistHz, float strength) noexcept
    {
        if (strength <= 0.0f || pivotHz <= 0.0f || nyquistHz <= pivotHz || hz <= pivotHz)
            return 1.0f;
        const float t = std::log (hz / pivotHz) / std::log (nyquistHz / pivotHz);
        const float tc = std::min (1.0f, std::max (0.0f, t));
        return 1.0f + strength * tc * kHighLiftMax;
    }

} // namespace dcr::builtin
```

- [ ] **Step 4: Run the tests — confirm green**

Run:
```bash
cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure -R dcorerouter_tests
```
Expected: PASS — `0 failures`, and the new checks counted.

- [ ] **Step 5: Commit**

```bash
git add Source/DSP/Builtin/StereoMeterMath.h tests/CoreLogicTests.cpp
git commit -m "feat(meter): pure HF-tilt + freq-norm math with regression tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: New params + sidebar sliders + HF-tilt wiring

**Files:**
- Modify: `Source/DSP/Builtin/StereoMeterProcessor.h` (`createLayout`)
- Modify: `Source/DSP/Builtin/StereoMeterEditor.mm` (include; `Impl::nyquistHz`; timer loop lift curve; `StereoControls` defs/`kN`/legend)

- [ ] **Step 1: Add the two params and bump the High-lift default**

In `Source/DSP/Builtin/StereoMeterProcessor.h`, inside `createLayout()`: change the `highLift` default `0.35f` to `0.5f`, and add `liftPivot` (log-skewed Hz) right after it, plus `axisOpacity` after `colorSat`. The `highLift` line becomes:
```cpp
            // High-frequency tilt strength. Reshaped by StereoMeterMath::highLiftGain:
            // ~no lift at/below `liftPivot`, rising toward Nyquist (true highs only).
            l.add (std::make_unique<P> (juce::ParameterID { "highLift", 1 }, "High lift", R (0.0f, 1.0f, 0.01f), 0.5f));
            {
                auto pivotRange = R (200.0f, 8000.0f, 1.0f);
                pivotRange.setSkewForCentre (1500.0f);
                l.add (std::make_unique<P> (juce::ParameterID { "liftPivot", 1 }, "Lift pivot", pivotRange, 2000.0f, A().withLabel ("Hz")));
            }
```
And immediately after the existing `colorSat` line, add:
```cpp
            l.add (std::make_unique<P> (juce::ParameterID { "axisOpacity", 1 }, "Axis opacity", R (0.0f, 1.0f, 0.01f), 0.85f));
```

- [ ] **Step 2: Wire the new lift curve into the editor's vertex loop**

In `Source/DSP/Builtin/StereoMeterEditor.mm`, add the math include near the top includes:
```cpp
#include "DSP/Builtin/StereoMeterMath.h"
```
Add a `nyquistHz` member to `struct Impl` (next to the other members like `analyzer`):
```cpp
    float nyquistHz = 24000.0f;
```
Set it in the `Impl` constructor (right after the `analyzer (...)` member is constructed — use the SAME basis the analyzer uses for its log bins):
```cpp
        nyquistHz = (float) (juce::jmax (8000.0, p.meterSampleRate()) * 0.5);
```
In `timerCallback()`, add a read of the pivot next to the other `getRawParameterValue` reads:
```cpp
        const float pLiftPivot = s.getRawParameterValue ("liftPivot")->load();
```
Replace the existing lift line:
```cpp
            const float freqNorm = N > 1 ? (float) i / (float) (N - 1) : 0.0f; // 0 low → 1 high
            const float it = std::min (1.0f, intensity * (1.0f + pHighLift * freqNorm * 3.0f));
```
with (keep `freqNorm` — it still drives the Y position below):
```cpp
            const float freqNorm = N > 1 ? (float) i / (float) (N - 1) : 0.0f; // 0 low → 1 high (Y axis)
            // HF tilt: lift only true highs (flat below the pivot) so bass/mids stay put.
            const float gain = dcr::builtin::highLiftGain (frame.freqs[(size_t) i], pLiftPivot, nyquistHz, pHighLift);
            const float it = std::min (1.0f, intensity * gain);
```
(`axisOpacity` is read in Task 3 where the renderer consumes it — do not read it here yet, to avoid an unused-variable warning.)

- [ ] **Step 3: Show the two new sliders in the sidebar**

In `StereoControls` (same file): bump `kN` from `11` to `13`, and extend the `defs[]` array. The array becomes:
```cpp
        static const Def defs[kN] = {
            { "floorDb", "Floor (dB)" }, { "ceilDb", "Ceiling (dB)" }, { "highLift", "High lift" }, { "liftPivot", "Lift pivot" },
            { "pointMin", "Min size" }, { "pointMax", "Max size" }, { "heightScale", "Height" },
            { "smooth", "Smooth" }, { "colorSat", "Colour" }, { "axisOpacity", "Axis opacity" },
            { "trailDepth", "Trail" }, { "trailDecay", "Trail fade" }, { "stemAmount", "Stems" }
        };
```
And update `kN`:
```cpp
    static constexpr int kN = 13;
```
Update the legend text (Y is self-labelling now):
```cpp
        legend.setText ("X = Pan (L <-> R)   Y = Frequency\nZ = Level   colour = phase (red anti / green in)\nDrag = orbit   Scroll = zoom",
                        juce::dontSendNotification);
```

- [ ] **Step 4: Build the app + run tests (no regression)**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: build succeeds; ctest `0 failures`.

- [ ] **Step 5: Manual verify (real app — user)**

`./run.sh`, unlock the meter (5× brand-title click), drop **Stereo Meter** on a stereo output group, play full-range audio. Confirm: raising **High lift** brightens/raises the **highs** while the **bass/low-mid** dots stay put; moving **Lift pivot** slides where the lift begins. (The new **Axis opacity** slider exists but does nothing until Task 3.)

- [ ] **Step 6: Commit**

```bash
git add Source/DSP/Builtin/StereoMeterProcessor.h Source/DSP/Builtin/StereoMeterEditor.mm
git commit -m "feat(meter): pivot-anchored HF tilt; Lift pivot + Axis opacity params

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: 3D on-plot frequency axis (labels + ticks) + Axis opacity

**Files:**
- Modify: `Source/DSP/Builtin/StereoMeterEditor.mm` (shaders, `TexVertex`, label pipeline, `makeLabelTexture`, renderer ivars + methods, draw code, `Impl` calls)

- [ ] **Step 1: Add the textured-quad vertex struct + label shaders**

Near the `PointVertex`/`LineVertex` structs in the anonymous namespace, add:
```cpp
    struct TexVertex
    {
        float pos[3]; // packed_float3 (12 bytes)
        float uv[2];  // packed_float2 (8 bytes)
    };
    constexpr float kFreqAxisX = -1.0f;   // labels/ticks sit on the left edge
    constexpr float kLabelHalfH = 0.05f;  // billboard half-height, world units
    constexpr int kMaxLabels = 16;
```
Append to `kShaderSrc` (before the closing `)METAL";`):
```cpp
struct TLV { packed_float3 pos; packed_float2 uv; };
struct TLOut { float4 pos [[position]]; float2 uv; };
vertex TLOut label_vs(uint vid [[vertex_id]],
                      const device TLV* v [[buffer(0)]],
                      constant Uniforms& u [[buffer(1)]]) {
    TLOut o;
    o.pos = u.mvp * float4(float3(v[vid].pos), 1.0);
    o.uv = float2(v[vid].uv);
    return o;
}
fragment float4 label_fs(TLOut in [[stage_in]],
                         texture2d<float> tex [[texture(0)]],
                         constant float& opacity [[buffer(0)]]) {
    constexpr sampler s(filter::linear, address::clamp_to_edge);
    float4 c = tex.sample(s, in.uv);   // JUCE ARGB is premultiplied
    return c * opacity;                // scale premultiplied rgb + a together
}
```

- [ ] **Step 2: Add the `makeLabelTexture` helper (JUCE Image → MTLTexture)**

Ensure the graphics include is present near the top (`juce_gui_extra` pulls it transitively, but be explicit):
```cpp
#include <juce_graphics/juce_graphics.h>
```
Add this free function in the anonymous namespace (after `lookAt`):
```cpp
    // Render a short string to a small premultiplied BGRA texture (UI thread, once).
    static id<MTLTexture> makeLabelTexture (id<MTLDevice> device, const juce::String& text, float& outAspect)
    {
        const float fontH = 30.0f;
        const int pad = 6;
        juce::Font font (juce::FontOptions (fontH));
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, text, 0.0f, 0.0f);
        const int w = juce::jmax (8, (int) std::ceil (ga.getBoundingBox (0, -1, true).getWidth()) + pad * 2);
        const int h = (int) std::ceil (fontH) + pad;
        outAspect = (float) w / (float) h;

        juce::Image img (juce::Image::ARGB, w, h, true);
        {
            juce::Graphics g (img);
            g.setColour (juce::Colours::white);
            g.setFont (font);
            g.drawText (text, 0, 0, w, h, juce::Justification::centred, false);
        }
        juce::Image::BitmapData bd (img, juce::Image::BitmapData::readOnly);
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:(NSUInteger) w
                                                                                    height:(NSUInteger) h
                                                                                 mipmapped:NO];
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        [tex replaceRegion:MTLRegionMake2D (0, 0, (NSUInteger) w, (NSUInteger) h)
               mipmapLevel:0
                 withBytes:bd.data
               bytesPerRow:(NSUInteger) bd.lineStride];
        return tex;
    }
```

- [ ] **Step 3: Add renderer ivars + create the label pipeline/buffer in `init`**

In `@implementation DCRScatterRenderer { ... }` ivar block, add:
```cpp
    id<MTLRenderPipelineState> _labelPSO;
    id<MTLBuffer> _freqAxisBuf;
    NSUInteger _freqAxisCount;
    id<MTLBuffer> _labelQuadBuf;
    id<MTLTexture> _labelTex[kMaxLabels];
    float _labelY[kMaxLabels];
    float _labelAspect[kMaxLabels];
    int _labelCount;
    float _axisOpacity;
```
In `initWithDevice:view:`, after `_linePSO = makePSO (...)`, add:
```cpp
    _labelPSO = makePSO (@"label_vs", @"label_fs");
    _labelQuadBuf = [device newBufferWithLength:kMaxLabels * 4 * sizeof (TexVertex)
                                        options:MTLResourceStorageModeShared];
    _labelCount = 0;
    _freqAxisCount = 0;
    _axisOpacity = 0.85f;
```

- [ ] **Step 4: Declare + implement `buildFreqAxisWithNyquist:` and `setAxisOpacity:`**

Add to the `@interface DCRScatterRenderer` declaration:
```cpp
- (void)buildFreqAxisWithNyquist:(double)nyquist;
- (void)setAxisOpacity:(float)opacity;
```
Implement (e.g. after `setTrailDepth:decay:`), including the math header at the top of the file if not already present (`#include "DSP/Builtin/StereoMeterMath.h"` — added in Task 2):
```cpp
- (void)setAxisOpacity:(float)opacity
{
    _axisOpacity = std::max (0.0f, std::min (1.0f, opacity));
}

- (void)buildFreqAxisWithNyquist:(double)nyquist
{
    struct Tick { float hz; const char* label; }; // label == nullptr -> minor tick, no text
    static const Tick ticks[] = {
        { 20, "20" }, { 50, nullptr }, { 100, "100" }, { 200, nullptr }, { 500, nullptr },
        { 1000, "1k" }, { 2000, nullptr }, { 5000, nullptr }, { 10000, "10k" }, { 20000, "20k" }
    };
    const float col[4] = { 0.0f, 1.0f, 0.82f, 0.7f }; // theme cyan; alpha scaled by axisOpacity at draw
    std::vector<LineVertex> lv;
    _labelCount = 0;
    for (auto& t : ticks)
    {
        if (t.hz >= (float) nyquist)
            continue;
        const float yN = 2.0f * dcr::builtin::freqToNorm (t.hz, 20.0f, (float) nyquist) - 1.0f;
        const bool major = (t.label != nullptr);
        const float len = major ? 0.16f : 0.08f;
        LineVertex a, b;
        a.pos[0] = kFreqAxisX;       a.pos[1] = yN; a.pos[2] = 0.0f;
        b.pos[0] = kFreqAxisX + len; b.pos[1] = yN; b.pos[2] = 0.0f;
        for (int k = 0; k < 4; ++k) { a.color[k] = col[k]; b.color[k] = col[k]; }
        lv.push_back (a);
        lv.push_back (b);
        if (major && _labelCount < kMaxLabels)
        {
            float aspect = 1.0f;
            _labelTex[_labelCount] = makeLabelTexture (_device, juce::String (t.label), aspect);
            _labelY[_labelCount] = yN;
            _labelAspect[_labelCount] = aspect;
            ++_labelCount;
        }
    }
    _freqAxisCount = lv.size();
    _freqAxisBuf = lv.empty() ? nil
                              : [_device newBufferWithBytes:lv.data()
                                                     length:lv.size() * sizeof (LineVertex)
                                                    options:MTLResourceStorageModeShared];
}
```

- [ ] **Step 5: Draw the ticks (opacity-scaled) and the billboarded labels**

In `drawInMTKView:`, just after the existing room-wireframe draw (`drawPrimitives:MTLPrimitiveTypeLine ... _boxCount`), add the freq ticks (still on `_linePSO`):
```cpp
    // Frequency-axis tick lines — faded by Axis opacity.
    if (_freqAxisCount > 0 && _freqAxisBuf != nil)
    {
        u.alphaMul = _axisOpacity;
        [enc setVertexBuffer:_freqAxisBuf offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof (u) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypeLine vertexStart:0 vertexCount:_freqAxisCount];
        u.alphaMul = 1.0f;
    }
```
Then compute the camera basis for billboarding. After the existing `const simd_float3 eye = ...;` line, add:
```cpp
    const simd_float3 camFwd = simd_normalize (target - eye);
    const simd_float3 camRight = simd_normalize (simd_cross (camFwd, (simd_float3) { 0, 1, 0 }));
    const simd_float3 camUp = simd_cross (camRight, camFwd);
```
Finally, after the point-cloud/trail loop (just before `[enc endEncoding];`), add the label pass (single buffer write, then per-label texture + draw slice — no CPU/GPU write hazard):
```cpp
    // Frequency labels — camera-facing billboards at each major tick height.
    if (_labelCount > 0 && _labelPSO != nil)
    {
        TexVertex quads[kMaxLabels * 4];
        for (int i = 0; i < _labelCount; ++i)
        {
            const float hh = kLabelHalfH;
            const float hw = hh * _labelAspect[i];
            const simd_float3 anchor = { kFreqAxisX - 0.14f, _labelY[i], 0.0f };
            auto setc = [&] (int j, float sx, float sy, float uu, float vv) {
                const simd_float3 p = anchor + camRight * (sx * hw) + camUp * (sy * hh);
                quads[i * 4 + j].pos[0] = p.x; quads[i * 4 + j].pos[1] = p.y; quads[i * 4 + j].pos[2] = p.z;
                quads[i * 4 + j].uv[0] = uu;   quads[i * 4 + j].uv[1] = vv;
            };
            setc (0, -1, -1, 0, 1); setc (1, 1, -1, 1, 1); setc (2, -1, 1, 0, 0); setc (3, 1, 1, 1, 0);
        }
        std::memcpy (_labelQuadBuf.contents, quads, (size_t) _labelCount * 4 * sizeof (TexVertex));
        [enc setRenderPipelineState:_labelPSO];
        [enc setVertexBuffer:_labelQuadBuf offset:0 atIndex:0];
        [enc setVertexBytes:&u length:sizeof (u) atIndex:1];
        float op = _axisOpacity;
        [enc setFragmentBytes:&op length:sizeof (op) atIndex:0];
        for (int i = 0; i < _labelCount; ++i)
        {
            [enc setFragmentTexture:_labelTex[i] atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:(NSUInteger) (i * 4) vertexCount:4];
        }
    }
```

- [ ] **Step 6: Build the axis once + push opacity each tick from `Impl`**

In `Impl`'s constructor, after `view.delegate = renderer;`, add:
```cpp
        [renderer buildFreqAxisWithNyquist:(double) nyquistHz];
```
In `timerCallback()`, read the param and push it (next to the other renderer setters near the end):
```cpp
        const float pAxisOpacity = s.getRawParameterValue ("axisOpacity")->load();
        [renderer setAxisOpacity:pAxisOpacity];
```

- [ ] **Step 7: Build + tests**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: build succeeds; ctest `0 failures`.

- [ ] **Step 8: Manual verify (real app — user)**

In the open meter: frequency labels `20 / 100 / 1k / 10k / 20k` sit at the correct heights on the left edge with tick lines, stay upright and readable while you **orbit** and **zoom**, and the **Axis opacity** slider fades only the ticks + labels (the room box and points are unaffected).

- [ ] **Step 9: Commit**

```bash
git add Source/DSP/Builtin/StereoMeterEditor.mm
git commit -m "feat(meter): 3D camera-tracked frequency axis labels + ticks with Axis opacity

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Save / Reset of meter settings

**Files:**
- Modify: `Source/DSP/Builtin/StereoMeterProcessor.h` (include; `factoryState`; ctor; Save/Reset/load + path)
- Modify: `Source/DSP/Builtin/StereoMeterEditor.mm` (`StereoControls` takes the processor; Save/Reset buttons; `resized`; `Impl` constructs controls with the processor)

- [ ] **Step 1: Processor — user-default persistence + factory capture**

In `Source/DSP/Builtin/StereoMeterProcessor.h`, add the include near the top:
```cpp
#include "Persistence/AtomicXmlWrite.h"
```
Replace the constructor with one that captures the factory state and loads a saved default:
```cpp
        StereoMeterProcessor() : BuiltinProcessor (ids::stereo_meter, "Stereo Meter", createLayout())
        {
            // Capture pristine defaults BEFORE any user-default is applied, so Reset
            // can always restore them.
            factoryState = apvts.copyState();
            loadUserDefault();
        }
```
Add these public methods (e.g. just after `createLayout()`):
```cpp
        // ----- user default (Save / Reset) -------------------------------------
        // The user-default is applied in the CONSTRUCTOR only, so a project
        // snapshot's setStateInformation() still overrides it on restore, while a
        // freshly added meter opens with the saved settings.
        static juce::File defaultsFile()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("dcorerouter");
            dir.createDirectory();
            return dir.getChildFile ("StereoMeterDefaults.xml");
        }

        void saveUserDefault()
        {
            if (auto xml = apvts.copyState().createXml())
                dcr::writeXmlAtomically (*xml, defaultsFile());
        }

        void resetToFactory()
        {
            apvts.replaceState (factoryState.createCopy());
        }
```
Add the load helper (can be private) and the member:
```cpp
    private:
        void loadUserDefault()
        {
            auto f = defaultsFile();
            if (! f.existsAsFile())
                return;
            if (auto xml = juce::parseXML (f))
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
        }

        juce::ValueTree factoryState;
```
(Keep the existing private ring-buffer members; just add `factoryState` and `loadUserDefault` to the private section.)

- [ ] **Step 2: Sidebar — Save / Reset buttons wired to the processor**

In `Source/DSP/Builtin/StereoMeterEditor.mm`, change `StereoControls` to take the processor. Update the constructor signature and store a reference:
```cpp
    explicit StereoControls (StereoMeterProcessor& p) : proc (p)
    {
        auto& s = p.getValueTreeState();
```
(Use `s` for the existing `Attach` bindings — the body already uses a `juce::AudioProcessorValueTreeState& s`; this just renames the source.) After the slider/legend setup in the constructor, add the buttons:
```cpp
        saveBtn.setButtonText ("Save");
        resetBtn.setButtonText ("Reset");
        addAndMakeVisible (saveBtn);
        addAndMakeVisible (resetBtn);
        saveBtn.onClick  = [this] { proc.saveUserDefault(); };
        resetBtn.onClick = [this] { proc.resetToFactory(); };
```
Add the members to the `private:` section:
```cpp
    StereoMeterProcessor& proc;
    juce::TextButton saveBtn, resetBtn;
```
Reserve a button row at the top of `resized()` (place at the very start of the method, before the slider layout):
```cpp
        auto full = getLocalBounds().reduced (8);
        auto top = full.removeFromTop (24);
        saveBtn.setBounds (top.removeFromLeft (top.getWidth() / 2).reduced (2, 0));
        resetBtn.setBounds (top.reduced (2, 0));
        full.removeFromTop (6);
        auto r = full;
        legend.setBounds (r.removeFromBottom (84));
```
(Delete the old `auto r = getLocalBounds().reduced (8);` and `legend.setBounds (...)` lines that this replaces; the per-slider loop below stays as-is.)

- [ ] **Step 3: Construct the controls with the processor**

In `Impl`'s constructor, change:
```cpp
        controls = std::make_unique<StereoControls> (p.getValueTreeState());
```
to:
```cpp
        controls = std::make_unique<StereoControls> (p);
```

- [ ] **Step 4: Build + tests**

Run:
```bash
cmake --build build -j && ctest --test-dir build --output-on-failure
```
Expected: build succeeds; ctest `0 failures`.

- [ ] **Step 5: Manual verify (real app — user)**

Adjust several sliders → **Save**. Quit and relaunch (`./run.sh`), drop a fresh Stereo Meter → it opens with the saved settings. Move sliders, hit **Reset** → all values jump back to factory defaults live. Confirm a saved project still restores its own (snapshot) values, not the global default.

- [ ] **Step 6: Commit**

```bash
git add Source/DSP/Builtin/StereoMeterProcessor.h Source/DSP/Builtin/StereoMeterEditor.mm
git commit -m "feat(meter): Save/Reset meter settings (user-default file + factory restore)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Finalize

- [ ] **Format check (CI fails on unformatted files):**
```bash
git ls-files -m | grep -E '\.(h|cpp|mm)$' | xargs clang-format -i 2>/dev/null; git diff --stat
```
Re-commit if anything changed (`git commit -am "style: clang-format"`).
- [ ] **Open the PR** (per CLAUDE.md — branch + PR + squash). Body should note: editor/Metal + math-header only; engine/RT path untouched; new unit tests; real-device items (orbit-readable labels, opacity, save/reset) verified by the user.

---

## Self-Review (done while writing — notes for the executor)

- **Spec coverage:** Part A → Task 1 (math) + Task 2 (wiring); Part B → Task 3; Part C (axis opacity) → param in Task 2, consumed in Task 3; Part D → Task 4. Tests → Task 1. All spec sections map to a task.
- **Type consistency:** `freqToNorm`/`highLiftGain`/`kHighLiftMax` signatures match between header, tests, and both call sites. `TexVertex`, `kFreqAxisX`, `kLabelHalfH`, `kMaxLabels` defined in Task 3 Step 1 before use. `buildFreqAxisWithNyquist:`/`setAxisOpacity:` declared (Step 4) and called (Step 6). `factoryState`/`defaultsFile`/`saveUserDefault`/`resetToFactory`/`loadUserDefault` defined in Task 4 Step 1, used in Step 2.
- **Premultiplied alpha:** JUCE `Image::ARGB` is premultiplied → `label_fs` returns `tex * opacity`, matching the existing `MTLBlendFactorOne / OneMinusSourceAlpha` setup. No double-multiply.
- **GPU hazard:** labels share one `_labelQuadBuf` filled in a single `memcpy` before any draw; per-label draws use `vertexStart` offsets — no CPU-overwrites-before-GPU-read race.
- **Load-order invariant:** user-default applied only in the constructor, so `setStateInformation` (snapshot restore) still wins for saved projects.

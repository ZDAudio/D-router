# Plugin window scaling + Stereo Meter 2D modes — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** (1) Let fixed-size AU plugin windows be resized with locked aspect ratio and stop the native title bar clipping the plugin's top row; (2) add 2D Front and RTA (parallel-projection) view modes to the Stereo Meter.

**Architecture:** F1 wraps a fixed-size editor in a scaling holder component that applies an `AffineTransform`, configures the window's existing default constrainer with a fixed aspect ratio (no `setResizable` re-call — that crashes plugins), and clears `NSWindowStyleMaskFullSizeContentView` to fix the title-bar overlap. F2 adds a `viewMode` APVTS choice param, an orthographic projection + fixed camera in the Metal renderer, mode-aware point positions, and a mode-aware frequency/dB axis, driven by a 3-segment selector in the sidebar.

**Tech Stack:** C++20, JUCE 8, Objective-C++, Metal, CoreAudio.

---

## File structure

- `Source/UI/PluginEditorWindow.h` — add holder + constrainer members.
- `Source/UI/PluginEditorWindow.mm` — scaling holder, aspect constrainer, title-bar fix, teardown order.
- `Source/DSP/Builtin/StereoMeterMath.h` — add `dbToNormY` (JUCE-free).
- `tests/CoreLogicTests.cpp` — unit tests for `dbToNormY`.
- `Source/DSP/Builtin/StereoMeterProcessor.h` — add `viewMode` choice param.
- `Source/DSP/Builtin/StereoMeterEditor.mm` — `ortho()`, mode-aware camera/projection/positions/axis, 3-segment selector, legend.

---

## Feature 1 — Plugin window

### Task 1: Header members

**Files:** Modify `Source/UI/PluginEditorWindow.h`

- [ ] Add includes already present (`<memory>` via unique_ptr is fine — `<vector>`/`<functional>` already there; add nothing new besides forward use of `juce::ComponentBoundsConstrainer`/`juce::Component`).
- [ ] Add private members after `std::unique_ptr<ParameterLink> paramLink;`:

```cpp
        // Fixed-size plugins are wrapped in a holder that scales the editor via
        // an AffineTransform so the window can be resized with the plugin's
        // native aspect ratio locked.  Null for self-resizing plugins.
        std::unique_ptr<juce::Component> editorHolder;
```

(The aspect ratio is set on the window's existing default constrainer via
`getConstrainer()`, so no extra constrainer member is needed.)

### Task 2: Scaling holder + aspect-locked resize + title-bar fix

**Files:** Modify `Source/UI/PluginEditorWindow.mm`

- [ ] In the anonymous namespace (after `createEditorDefensively`), add the holder:

```cpp
// Hosts a fixed-size plugin editor and scales it with an AffineTransform so the
// window can be freely resized while the plugin's native aspect ratio is kept.
// The editor keeps its natural logical bounds; only the transform changes.  We
// do NOT own the editor (PluginEditorWindow's unique_ptr does); destroying the
// editor first is safe because Component's dtor removes it from us.
class ScaledEditorHolder : public juce::Component
{
public:
    ScaledEditorHolder (juce::AudioProcessorEditor& e, int nW, int nH)
        : editor (e), natW (juce::jmax (1, nW)), natH (juce::jmax (1, nH))
    {
        setInterceptsMouseClicks (false, true);
        addAndMakeVisible (editor);
        editor.setTopLeftPosition (0, 0);
        setSize (natW, natH);
    }

    void resized() override
    {
        // Aspect ratio is locked by the window constrainer, so width drives the
        // scale and height follows.
        const double s = (double) getWidth() / (double) natW;
        editor.setTransform (juce::AffineTransform::scale ((float) s));
        editor.setBounds (0, 0, natW, natH);
    }

private:
    juce::AudioProcessorEditor& editor;
    int natW, natH;
};
```

- [ ] Replace the body of `installEditor` (`PluginEditorWindow.mm:132-176`) with a
  branch that wraps fixed-size plugins in the holder and locks aspect ratio:

```cpp
    auto installEditor = [this](juce::AudioProcessorEditor* ed)
    {
        // CRITICAL: do NOT call setResizable a second time here (it re-invokes
        // addToDesktop, which re-migrates the plugin NSView and crashes some
        // plugins).  Express all resize policy through the window's existing
        // default constrainer (setResizeLimits / getConstrainer()) -- those do
        // not touch the desktop.
        const int natW = juce::jmax (1, ed->getWidth());
        const int natH = juce::jmax (1, ed->getHeight());

        if (ed->isResizable())
        {
            // Plugin manages its own resizing -- host it directly and honour
            // its own constrainer (unchanged behaviour).
            setContentNonOwned (ed, true);
            if (auto* c = ed->getConstrainer())
            {
                const int minW = juce::jmax (1, c->getMinimumWidth());
                const int minH = juce::jmax (1, c->getMinimumHeight());
                const int maxW = juce::jmax (minW, c->getMaximumWidth());
                const int maxH = juce::jmax (minH, c->getMaximumHeight());
                setResizeLimits (minW, minH, maxW, maxH);
            }
            else
            {
                setResizeLimits (natW, natH, natW * 8, natH * 8);
            }
        }
        else
        {
            // Fixed-size plugin: wrap in a scaling holder so the user can resize
            // the window with the native aspect ratio locked.
            editorHolder = std::make_unique<ScaledEditorHolder> (*ed, natW, natH);
            setContentNonOwned (editorHolder.get(), true);

            const int minW = juce::jmax (1, juce::roundToInt (natW * 0.4));
            const int minH = juce::jmax (1, juce::roundToInt (natH * 0.4));
            setResizeLimits (minW, minH, natW * 3, natH * 3);
            if (auto* c = getConstrainer())
                c->setFixedAspectRatio ((double) natW / (double) natH);
        }
        centreWithSize (getWidth(), getHeight());
    };
```

- [ ] After the `installEditor` try/catch block (before the ctor's closing brace,
  ~`PluginEditorWindow.mm:192`), add the title-bar overlap fix:

```cpp
#if JUCE_MAC
    // Some hosts/plugins leave the window with a full-size content view, which
    // lets the plugin's top row slide under the native title bar.  Clear that
    // style so JUCE's content sits fully below the title bar.
    if (auto* peer = getPeer())
        if (auto* nsv = (NSView*) peer->getNativeHandle())
            if (NSWindow* win = [nsv window])
            {
                win.styleMask &= ~NSWindowStyleMaskFullSizeContentView;
                win.titlebarAppearsTransparent = NO;
            }
#endif
```

- [ ] In the destructor's guarded teardown (`PluginEditorWindow.mm:215-225`),
  destroy the holder between detaching content and destroying the editor, so the
  editor (still alive) is removed from the window before it is deleted:

```cpp
    @try
    {
        clearContentComponent(); // detach the (live) editor/holder from the window
        editorHolder.reset();    // drop the holder (editor is still alive here)
        editor.reset();          // then destroy the editor itself
    }
    @catch (NSException* ex)
    {
        DBG("[plugin editor] NSException during teardown ("
            << plugin.getName() << "): " << [[ex reason] UTF8String]);
        editor.release(); // intentional leak -- safer than crashing
    }
```

  And in the non-mac branch (`:227-228`) add `editorHolder.reset();` before
  `editor.reset();`.

### Task 3: Build F1

- [ ] Run: `cmake --build build -j 2>&1 | tail -20` — Expected: builds clean (or only pre-existing warnings).
- [ ] Commit:

```bash
git add Source/UI/PluginEditorWindow.h Source/UI/PluginEditorWindow.mm
git commit -m "feat(ui): aspect-locked resize for fixed-size plugin windows + title-bar overlap fix"
```

---

## Feature 2 — Stereo Meter 2D modes

### Task 4: JUCE-free dB→Y helper + test (TDD)

**Files:** Modify `Source/DSP/Builtin/StereoMeterMath.h`, `tests/CoreLogicTests.cpp`

- [ ] **Write the failing test** in `tests/CoreLogicTests.cpp` (add `#include "DSP/Builtin/StereoMeterMath.h"` near the other includes, and a test case alongside the existing ones):

```cpp
TEST_CASE ("StereoMeterMath dbToNormY maps the dB window to [-1,1]")
{
    using dcr::builtin::dbToNormY;
    // ceiling -> +1 (top), floor -> -1 (bottom)
    REQUIRE (dbToNormY (0.0f, -60.0f, 0.0f) == Approx (1.0f));
    REQUIRE (dbToNormY (-60.0f, -60.0f, 0.0f) == Approx (-1.0f));
    REQUIRE (dbToNormY (-30.0f, -60.0f, 0.0f) == Approx (0.0f));
    // clamps outside the window
    REQUIRE (dbToNormY (10.0f, -60.0f, 0.0f) == Approx (1.0f));
    REQUIRE (dbToNormY (-90.0f, -60.0f, 0.0f) == Approx (-1.0f));
    // degenerate range stays finite
    REQUIRE (dbToNormY (-30.0f, -30.0f, -30.0f) == Approx (-1.0f));
}
```

- [ ] **Run to verify it fails:** `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure` — Expected: compile error (`dbToNormY` not declared) or test fail.

- [ ] **Implement** in `StereoMeterMath.h` before the closing namespace brace:

```cpp
    // Map a dB value within [floorDb, ceilDb] to a vertical screen coordinate in
    // [-1, 1] (floor -> -1 bottom, ceil -> +1 top), clamped.  Used by the RTA
    // (side) view's level axis.  JUCE-free so it links into the test target.
    inline float dbToNormY (float db, float floorDb, float ceilDb) noexcept
    {
        if (ceilDb <= floorDb)
            return -1.0f;
        const float t = (db - floorDb) / (ceilDb - floorDb);
        const float tc = std::min (1.0f, std::max (0.0f, t));
        return 2.0f * tc - 1.0f;
    }
```

- [ ] **Run to verify it passes:** `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure` — Expected: all pass.

- [ ] **Commit:**

```bash
git add Source/DSP/Builtin/StereoMeterMath.h tests/CoreLogicTests.cpp
git commit -m "feat(meter): add JUCE-free dbToNormY for the RTA level axis"
```

### Task 5: viewMode parameter

**Files:** Modify `Source/DSP/Builtin/StereoMeterProcessor.h`

- [ ] In `createLayout()` (after the `stemAmount` line, `:52`), add:

```cpp
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "viewMode", 1 }, "View", juce::StringArray { "3D", "Front", "RTA" }, 0));
```

(Default index 0 = 3D. Captured into `factoryState` by the existing ctor order, so
Reset restores it; saved with snapshots and Save.)

### Task 6: Renderer — ortho projection, mode-aware camera, mode-aware axis

**Files:** Modify `Source/DSP/Builtin/StereoMeterEditor.mm`

- [ ] Add an `ortho()` builder after `perspective()` (`:69`):

```cpp
matrix_float4x4 ortho(float l, float r, float b, float t, float n, float f)
{
    matrix_float4x4 m = matrix_identity_float4x4;
    m.columns[0] = (simd_float4){2.0f / (r - l), 0, 0, 0};
    m.columns[1] = (simd_float4){0, 2.0f / (t - b), 0, 0};
    m.columns[2] = (simd_float4){0, 0, 1.0f / (n - f), 0};
    m.columns[3] = (simd_float4){-(r + l) / (r - l), -(t + b) / (t - b), n / (n - f), 1};
    return m;
}
```

- [ ] Add view-mode + ortho-zoom state to `DCRScatterMTKView` (`:175-179`):

```objc
@interface DCRScatterMTKView : MTKView
@property (nonatomic) float camYaw;
@property (nonatomic) float camPitch;
@property (nonatomic) float camDist;
@property (nonatomic) int viewMode;    // 0=3D 1=Front 2=RTA
@property (nonatomic) float orthoZoom; // 2D zoom factor
@end
```

- [ ] Init them in `initWithFrame` (`:186-188`): add `_viewMode = 0; _orthoZoom = 1.0f;`.

- [ ] Make interaction mode-aware (`mouseDragged` `:200`, `scrollWheel` `:208`):

```objc
- (void)mouseDragged:(NSEvent*)e
{
    if (_viewMode != 0)
        return; // orbit only in 3D
    _camYaw -= (float)e.deltaX * 0.01f;
    _camPitch += (float)e.deltaY * 0.01f;
    const float lim = 1.5f;
    _camPitch = std::max(-lim, std::min(lim, _camPitch));
    [self setNeedsDisplay:YES];
}
- (void)scrollWheel:(NSEvent*)e
{
    if (_viewMode == 0)
    {
        _camDist *= (1.0f - (float)e.scrollingDeltaY * 0.02f);
        _camDist = std::max(1.4f, std::min(12.0f, _camDist));
    }
    else
    {
        _orthoZoom *= (1.0f + (float)e.scrollingDeltaY * 0.02f);
        _orthoZoom = std::max(0.4f, std::min(3.0f, _orthoZoom));
    }
    [self setNeedsDisplay:YES];
}
```

- [ ] Generalize label anchors in the renderer ivars (`:246-247`): replace
  `float _labelY[kMaxLabels];` with:

```objc
    float _labelAX[kMaxLabels];
    float _labelAY[kMaxLabels];
```

- [ ] Replace `buildFreqAxisWithNyquist:` declaration (`:222`) and implementation
  (`:372-429`) with a mode-aware builder. New declaration in the `@interface`
  (`:217-224`):

```objc
- (void)buildAxisForMode:(int)mode nyquist:(double)nyquist floorDb:(float)floorDb ceilDb:(float)ceilDb;
```

  New implementation (replaces the old `buildFreqAxisWithNyquist:` method body):

```objc
- (void)buildAxisForMode:(int)mode nyquist:(double)nyquist floorDb:(float)floorDb ceilDb:(float)ceilDb
{
    struct Tick { float hz; const char* label; };
    static const Tick ticks[] = {{20, "20"}, {50, nullptr}, {100, "100"}, {200, nullptr},
                                 {500, nullptr}, {1000, "1k"}, {2000, nullptr}, {5000, nullptr},
                                 {10000, "10k"}, {20000, "20k"}};
    const float col[4] = {0.0f, 1.0f, 0.82f, 0.7f};
    std::vector<LineVertex> lv;
    _labelCount = 0;

    auto addLabel = [&](const juce::String& text, float ax, float ay)
    {
        if (_labelCount >= kMaxLabels)
            return;
        float aspect = 1.0f;
        _labelTex[_labelCount] = makeLabelTexture(_device, text, aspect);
        _labelAX[_labelCount] = ax;
        _labelAY[_labelCount] = ay;
        _labelAspect[_labelCount] = aspect;
        ++_labelCount;
    };
    auto addTick = [&](float x0, float y0, float x1, float y1)
    {
        LineVertex a, b;
        a.pos[0] = x0; a.pos[1] = y0; a.pos[2] = 0.0f;
        b.pos[0] = x1; b.pos[1] = y1; b.pos[2] = 0.0f;
        for (int k = 0; k < 4; ++k) { a.color[k] = col[k]; b.color[k] = col[k]; }
        lv.push_back(a); lv.push_back(b);
    };

    if (mode == 2) // RTA: frequency on the horizontal axis, level on the vertical.
    {
        for (auto& t : ticks)
        {
            if (t.hz >= (float)nyquist)
                continue;
            const float xN = 2.0f * dcr::builtin::freqToNorm(t.hz, 20.0f, (float)nyquist) - 1.0f;
            const bool major = (t.label != nullptr);
            const float len = major ? 0.16f : 0.08f;
            addTick(xN, -1.0f, xN, -1.0f + len);
            if (major)
                addLabel(juce::String(t.label), xN, -1.08f);
        }
        // Vertical dB scale: floor (bottom), ceil (top), and the midpoint.
        const float midDb = 0.5f * (floorDb + ceilDb);
        addLabel(juce::String(juce::roundToInt(ceilDb)), -1.12f, 1.0f);
        addLabel(juce::String(juce::roundToInt(midDb)), -1.12f, 0.0f);
        addLabel(juce::String(juce::roundToInt(floorDb)), -1.12f, -1.0f);
        addTick(-1.0f, 0.0f, -0.92f, 0.0f); // mid gridline stub
    }
    else // 3D / Front: frequency on the vertical axis at the left edge.
    {
        for (auto& t : ticks)
        {
            if (t.hz >= (float)nyquist)
                continue;
            const float yN = 2.0f * dcr::builtin::freqToNorm(t.hz, 20.0f, (float)nyquist) - 1.0f;
            const bool major = (t.label != nullptr);
            const float len = major ? 0.16f : 0.08f;
            addTick(kFreqAxisX, yN, kFreqAxisX + len, yN);
            if (major)
                addLabel(juce::String(t.label), kFreqAxisX - 0.14f, yN);
        }
    }

    _freqAxisCount = lv.size();
    _freqAxisBuf = lv.empty() ? nil
                              : [_device newBufferWithBytes:lv.data()
                                                     length:lv.size() * sizeof(LineVertex)
                                                    options:MTLResourceStorageModeShared];
}
```

- [ ] Update the billboard label loop (`:506-543`) to use the generalized anchors:
  replace `const simd_float3 anchor = {kFreqAxisX - 0.14f, _labelY[i], 0.0f};`
  with `const simd_float3 anchor = {_labelAX[i], _labelAY[i], 0.0f};`.

- [ ] Make the MVP + camera basis mode-aware in `drawInMTKView` (`:446-458`).
  Replace that block with:

```cpp
    simd_float3 camRight, camUp;
    Uniforms u;
    if (sv.viewMode == 0)
    {
        const simd_float3 target = {0, 0, kZScale * 0.4f};
        const simd_float3 dir = {std::cos(sv.camPitch) * std::sin(sv.camYaw),
                                 std::sin(sv.camPitch),
                                 std::cos(sv.camPitch) * std::cos(sv.camYaw)};
        const simd_float3 eye = target + sv.camDist * dir;
        const simd_float3 camFwd = simd_normalize(target - eye);
        camRight = simd_normalize(simd_cross(camFwd, (simd_float3){0, 1, 0}));
        camUp = simd_cross(camRight, camFwd);
        u.mvp = matrix_multiply(perspective(1.0f, aspect, 0.05f, 100.0f),
                                lookAt(eye, target, (simd_float3){0, 1, 0}));
    }
    else
    {
        // 2D parallel projection, fixed front-on camera (looking down -Z).
        const float z = sv.orthoZoom > 0.0f ? sv.orthoZoom : 1.0f;
        const float halfH = 1.25f / z;
        const float halfW = halfH * aspect;
        u.mvp = ortho(-halfW, halfW, -halfH, halfH, -10.0f, 10.0f);
        camRight = (simd_float3){1, 0, 0};
        camUp = (simd_float3){0, 1, 0};
    }
```

  (The existing `const CGSize ds`/`aspect` lines just above stay; only the
  `target/dir/eye/...` and `Uniforms u; u.mvp = ...` lines are replaced.)

### Task 7: Editor Impl — pass mode through, mode-aware positions, sidebar selector

**Files:** Modify `Source/DSP/Builtin/StereoMeterEditor.mm`

- [ ] Update the `StereoControls` class to add a 3-segment selector. Add members
  (`:639-643`):

```cpp
    juce::TextButton seg[3];
    juce::AudioParameterChoice* viewParam = nullptr;
```

- [ ] In the `StereoControls` ctor (after the Save/Reset wiring, `:607`), add:

```cpp
        viewParam = dynamic_cast<juce::AudioParameterChoice*>(s.getParameter("viewMode"));
        static const char* segNames[3] = {"3D", "Front", "RTA"};
        for (int i = 0; i < 3; ++i)
        {
            seg[i].setButtonText(segNames[i]);
            seg[i].setClickingTogglesState(true);
            seg[i].setRadioGroupId(9201);
            seg[i].setConnectedEdges(((i > 0) ? juce::Button::ConnectedOnLeft : 0)
                                     | ((i < 2) ? juce::Button::ConnectedOnRight : 0));
            addAndMakeVisible(seg[i]);
            seg[i].onClick = [this, i]
            {
                if (viewParam != nullptr)
                    *viewParam = i;
            };
        }
        syncViewSeg();
```

- [ ] Add a public `syncViewSeg()` to `StereoControls` (so the timer can keep the
  buttons in sync with the parameter after a snapshot restore):

```cpp
    void syncViewSeg()
    {
        const int idx = viewParam != nullptr ? viewParam->getIndex() : 0;
        for (int i = 0; i < 3; ++i)
            seg[i].setToggleState(i == idx, juce::dontSendNotification);
    }
```

- [ ] Update `StereoControls::resized()` (`:617-633`) to reserve a row for the
  selector below the Save/Reset row:

```cpp
    void resized() override
    {
        auto full = getLocalBounds().reduced(8);
        auto top = full.removeFromTop(24);
        saveBtn.setBounds(top.removeFromLeft(top.getWidth() / 2).reduced(2, 0));
        resetBtn.setBounds(top.reduced(2, 0));
        full.removeFromTop(6);
        auto segRow = full.removeFromTop(22);
        for (int i = 0; i < 3; ++i)
            seg[i].setBounds(segRow.removeFromLeft(segRow.getWidth() / (3 - i)));
        full.removeFromTop(6);
        auto r = full;
        legend.setBounds(r.removeFromBottom(84));
        const int rh = juce::jmax(30, r.getHeight() / kN);
        for (int i = 0; i < kN; ++i)
        {
            auto row = r.removeFromTop(rh);
            labels[i].setBounds(row.removeFromTop(13));
            sliders[i].setBounds(row.removeFromTop(juce::jmin(20, row.getHeight())));
        }
    }
```

- [ ] Update the legend text (`:594`) to mention the modes:

```cpp
        legend.setText("3D: X=Pan Y=Freq Z=Level (drag orbit)\nFront: Pan x Freq   RTA: Freq x Level\ncolour = phase (red anti / green in)  scroll = zoom",
                       juce::dontSendNotification);
```

- [ ] In `Impl::timerCallback`, after reading the other params (`:750`), read the
  view mode, push it + ortho axis to the view, keep the selector synced, and
  rebuild the axis when the mode or dB window changes. Add these tracking ivars to
  `Impl` (near `float nyquistHz` `:654`):

```cpp
    int lastViewMode = -1;
    float lastFloor = 1.0f, lastCeil = 1.0f;
```

  Then in `timerCallback`, right after `const float pStemAmount = ...` (`:750`):

```cpp
        const int pView = (int)s.getRawParameterValue("viewMode")->load();
        view.viewMode = pView;
        controls->syncViewSeg();
        if (pView != lastViewMode || (pView == 2 && (pFloor != lastFloor || pCeil != lastCeil)))
        {
            [renderer buildAxisForMode:pView nyquist:(double)nyquistHz floorDb:pFloor ceilDb:pCeil];
            lastViewMode = pView;
            lastFloor = pFloor;
            lastCeil = pCeil;
        }
```

- [ ] Make the point-building loop mode-aware. Replace the position assignment
  inside the loop (`:772-774` and `:792-795`, plus the stem block `:803-823`).
  Specifically, after computing `it`, `c`, color (`r/gg/b`) but where `pan/yN/z`
  are set, branch by `pView`:

```cpp
            const float pan = frame.pans[(size_t)i];
            const float yFreq = 2.0f * freqNorm - 1.0f;
            const float zLevel = it * kZScale * pHeight;

            // ... (colour computation r/gg/b unchanged) ...

            PointVertex pv;
            if (pView == 2) // RTA: X=freq, Y=level
            {
                pv.pos[0] = yFreq;
                pv.pos[1] = 2.0f * it - 1.0f;
                pv.pos[2] = 0.0f;
            }
            else if (pView == 1) // Front: X=pan, Y=freq (flat)
            {
                pv.pos[0] = pan;
                pv.pos[1] = yFreq;
                pv.pos[2] = 0.0f;
            }
            else // 3D
            {
                pv.pos[0] = pan;
                pv.pos[1] = yFreq;
                pv.pos[2] = zLevel;
            }
            pv.color[0] = r;
            pv.color[1] = gg;
            pv.color[2] = b;
            pv.color[3] = std::min(1.0f, 0.18f + it);
            pv.size = pPointMin + it * (pPointMax - pPointMin);
            pts.push_back(pv);

            if (wantStems && pView == 0) // stems only meaningful in 3D
            {
                const float sa = pStemAmount * it;
                LineVertex lo, hi;
                lo.pos[0] = pan; lo.pos[1] = yFreq; lo.pos[2] = 0.0f;
                hi.pos[0] = pan; hi.pos[1] = yFreq; hi.pos[2] = zLevel;
                lo.color[0] = r; lo.color[1] = gg; lo.color[2] = b; lo.color[3] = sa;
                hi.color[0] = r; hi.color[1] = gg; hi.color[2] = b; hi.color[3] = sa;
                stems.push_back(lo);
                stems.push_back(hi);
            }
```

  (Remove the now-replaced old `const float yN = ...; const float z = ...;` lines
  and the old `pv.pos[...]` / stem block they fed.)

- [ ] Update the ctor's axis build call (`:680`) from
  `[renderer buildFreqAxisWithNyquist:(double)nyquistHz];` to:

```cpp
        [renderer buildAxisForMode:0 nyquist:(double)nyquistHz floorDb:-60.0f ceilDb:0.0f];
```

### Task 8: Build, test, verify F2

- [ ] Run: `cmake --build build -j 2>&1 | tail -20` — Expected: clean build.
- [ ] Run: `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure` — Expected: all pass.
- [ ] Run `clang-format -i` on every changed file (CI fails otherwise).
- [ ] Commit:

```bash
git add Source/DSP/Builtin/StereoMeterProcessor.h Source/DSP/Builtin/StereoMeterEditor.mm
git commit -m "feat(meter): add 2D Front and RTA parallel-projection view modes"
```

### Task 9: Launch for visual check

- [ ] Run `./run.sh` to build + relaunch so the user can visually verify both
  features on real devices (the only way to confirm the GPU-scaled plugin window
  and the Metal 2D modes).

---

## Verification (user, real device)
- F1a: native title bar no longer clips a fixed-size AU's top row.
- F1b: dragging a fixed-size AU window corner scales the GUI with locked aspect
  ratio; no crash on open/resize/close; resizable plugins behave as before.
- F2: 3D / Front / RTA selector switches modes; Front shows pan×freq, RTA shows
  freq(horizontal)×level(vertical) with freq + dB labels; orbit disabled but
  scroll-zoom works in 2D; mode persists across reopen and Save/Reset.

## Self-review notes
- Spec coverage: F1a (title-bar fix, Task 2), F1b (holder+aspect, Task 2),
  F2 param (Task 5), ortho+camera+axis (Task 6), selector+positions+persist
  (Task 7), tests (Task 4). All covered.
- No `setResizable` re-call (uses `getConstrainer()`/`setResizeLimits`). ✓
- Factory-state order for `viewMode` respected (added in `createLayout`). ✓
- Names consistent: `buildAxisForMode:nyquist:floorDb:ceilDb:`, `viewMode`,
  `orthoZoom`, `syncViewSeg`, `dbToNormY`, `editorHolder` used identically
  across tasks. ✓

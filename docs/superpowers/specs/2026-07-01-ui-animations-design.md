# UI animation pass — design

Status: approved 2026-07-01. Branch `feat/ui-animations`.

## Goal

Add subtle, fast UI motion to D-Router. Four areas, one PR:

1. **Tab content transition** — fade (+ small rise) the incoming panel when
   switching MATRIX / GROUPS / AUDIO SETUP / MONITOR, instead of an instant
   `setVisible` swap.
2. **Tab-button indicator** — a rounded accent bar on the rail's left gutter
   that eases its Y position to the active tab (and tracks the rail collapse).
3. **Pop-out / dock** — reuse the content fade when a panel re-docks or its
   detached placeholder appears.
4. **Micro-interactions** — rail tab-button hover-glow ease; PANIC banner
   fade-in; pop-out window fade-in; crosspoint cell toggle flash.

Style: **subtle & fast** — ~150 ms eases matching the existing rail collapse.

## Non-negotiable: this is message-thread only

Nothing here touches the matrix thread, CoreAudio callbacks, or any
`processBlock`. Pure UI. No RT-safety implications.

## Foundation — `Source/UI/Eased.h` (JUCE-free)

Extract the existing rail easing (`railWidthPx += diff * 0.30`) into a reusable
value:

```cpp
struct Eased {
    double current = 0.0, target = 0.0;
    void snap (double v)       { current = target = v; }
    void to   (double v)       { target = v; }
    bool atRest() const        { return current == target; }
    // ease current toward target; snap + return false when within eps.
    bool step (double factor = 0.30, double eps = 0.5);
};
```

`step` math: `diff = target - current`; if `|diff| <= eps` set `current =
target`, return `false`; else `current += diff * factor`, return `true`.
Deterministic → **unit-tested** in `tests/CoreLogicTests.cpp` (convergence,
epsilon snap, `snap`/`to`/`atRest`, monotonic approach).

## One driver — `uiAnim` timer in `MainComponent`

Replace the single-purpose `railAnim` with one 60 Hz `CallbackTimer`. Each frame
`stepUiAnimation()`:

- steps `railW` (Eased, replaces `railWidthPx`/`railTargetW` int pair),
- steps `indicatorY` (Eased) and repaints the rail,
- steps `contentFade` (Eased) and applies alpha + a `(1-fade)*8px` downward
  `AffineTransform::translation` to the active in-window content component(s);
  clears the transform and restores `setAlpha(1)` at rest,
- steps a per-tab `hover[4]` glow toward `button.isOver() ? 1 : 0`, writes it to
  each rail button's `"railHover"` property, repaints changed buttons,
- steps `panicFade` (Eased) for the PANIC banner,
- calls `resized()` if the rail width moved (else just `repaint()`),
- **stops the timer** when every Eased/hover is at rest.

`resized()` sets `railW.to(...)` and `indicatorY.to(activeTabButtonCentreY)` and
wakes `uiAnim` if anything is off-target — same start-on-demand logic as today.
`mouseEnter`/`mouseExit` on the four rail buttons wake `uiAnim` so hover eases.

## Rendering touch-points

- `MainComponent::paint()` — draw the eased indicator bar in the rail gutter
  (x just left of the tab buttons, height ≈ tab height, rounded, accent colour).
- `LookAndFeel::drawButtonBackground` — for `"railIcon"` buttons, lerp the
  background between rest `(28,28,34)` and highlight `(42,42,48)` by the eased
  `"railHover"` value instead of the instant highlight flag.
- `CrosspointGrid` — on a click-toggle, flash the toggled cell: a grid-local
  `Eased` + short timer eases a white overlay `1→0` over the single cell
  (bounded repaint, no RT impact).

## Testing

- `Eased::step` → deterministic unit tests (`ctest`, ~0.5 s, JUCE-free).
- All visual behaviour (timing feel, no flicker on fast tab-mashing, indicator
  ↔ rail-collapse interplay, fade cleanliness) is **user / real-device
  verification** — reported honestly as unverified until the user confirms.

## Explicitly out of scope

- Per-button hover ease on the toolbar buttons (keep JUCE's instant highlight)
  — bounds the diff.
- True cross-fade of outgoing↔incoming panels (fade-in of the incoming panel
  only — cheaper and reads cleaner).

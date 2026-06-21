# Phase C1 — PanicController Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract D-Router's PANIC feature from MainComponent into an explicit, single-owner, unit-tested `dcr::PanicController` state machine, so the panic state and its saved mute vectors have exactly one owner and the UI derives from it.

**Architecture:** A new JUCE-free `dcr::PanicController` (in `Source/Routing/`) owns `enum class State { Inactive, Active }` plus the saved input/output mute vectors. It exposes four transitions — `engage`, `release`, `noteUserMuteChanged`, `reset` — operating on a `RoutingMatrix&`. MainComponent drops its `inPanic` bool and the two `savedInputMutes`/`savedOutputMutes` vectors, holds a `PanicController` instead, and every former `inPanic` read becomes `panic.isActive()`. Because it depends only on the JUCE-free `RoutingMatrix`, it is tested in the fast pure-logic `dcorerouter_tests` target.

**Tech Stack:** C++20, JUCE 8 (app only — the controller is JUCE-free), the existing `tests/CoreLogicTests.cpp` harness (`CHECK` macro, manual `test_*` registration in `main`).

## Global Constraints

- **DSP is out of scope** — this touches only message-thread UI coordination + routing-state reads. No audio-path code.
- **Real-time safety:** `RoutingMatrix::setInputMute/setOutputMute` are already UI-thread setters; PanicController runs on the message thread only. No allocation in `noteUserMuteChanged`/`reset` beyond `std::vector::clear`.
- **`dcr::` namespace** for the new class.
- **Match house style:** Allman braces, space-before-paren in calls, comment the *why* (RT-safety / lifetime / ordering). New file must pass `clang-format --dry-run --Werror` against the adopted `.clang-format`.
- **No behavior change** visible to the user: panic engage/release/forget/reset semantics must be byte-for-byte preserved (verified by tests mirroring the current logic at MainComponent.cpp:1026–1132).
- Branch: `overhaul/phase-c-statemachines`. Small commits, `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>` trailer.

---

## File Structure

- **Create** `Source/Routing/PanicController.h` — the state machine (declaration + doc comments).
- **Create** `Source/Routing/PanicController.cpp` — the four transitions.
- **Modify** `CMakeLists.txt` — add `PanicController.cpp` to both the `dcorerouter` app target (line ~31 `target_sources`) and the `dcorerouter_tests` pure-logic target (line ~108).
- **Modify** `tests/CoreLogicTests.cpp` — add `#include "Routing/PanicController.h"`, the `test_panic_*` functions, and register them in `main`.
- **Modify** `Source/MainComponent.h` — remove `inPanic`, `savedInputMutes`, `savedOutputMutes`; add `#include "Routing/PanicController.h"` and a `PanicController panic;` member.
- **Modify** `Source/MainComponent.cpp` — rewire the 8 sites (onClick:100, onUserMuteChanged:240, menu:545+617, panicActivate:1026, panicRelease:1051, updatePanicButtonAppearance:1067, panicResetRestart:1088, applyDeviceSelection:1126) to the controller.

---

## Authoritative current behavior (the contract to preserve)

From `MainComponent.cpp` as it stands on this branch:

- **engage** (`panicActivate` 1026): if `nIn==0 && nOut==0` → no-op. Else save each input/output's current mute into `savedInputMutes`/`savedOutputMutes` (as `0/1`), then `setInputMute(n,true)`/`setOutputMute(o,true)` for all; set active.
- **release** (`panicRelease` 1051): restore `setInputMute(n, saved[n]!=0)` for `min(saved.size, numInputs)` (and outputs), then `clear()` both vectors, set inactive.
- **forget** (`onUserMuteChanged` 240): if not active → return; else set inactive + `clear()` both vectors. **Matrix is NOT touched** (the user's manual mute stands).
- **reset** (`applyDeviceSelection` 1126): if active OR either saved vector non-empty → set inactive + `clear()` both vectors + refresh button. **Matrix is NOT touched** (it's about to be rebuilt).
- **reset-restart** (`panicResetRestart` 1088): if active → `release` first (so the restart harvests real routing), then restart.

---

### Task 1: PanicController state machine + pure-logic tests

**Files:**
- Create: `Source/Routing/PanicController.h`
- Create: `Source/Routing/PanicController.cpp`
- Modify: `CMakeLists.txt` (add `Source/Routing/PanicController.cpp` to `dcorerouter_tests` at line ~110, and to `dcorerouter` app `target_sources` at line ~39 near `RoutingMatrix.cpp`)
- Test: `tests/CoreLogicTests.cpp`

**Interfaces:**
- Consumes: `dcr::RoutingMatrix` (`getNumInputs/getNumOutputs`, `getInputMute/getOutputMute`, `setInputMute/setOutputMute`).
- Produces:
  - `dcr::PanicController` with `enum class State { Inactive, Active };`
  - `State state() const noexcept;`
  - `bool isActive() const noexcept;`
  - `bool engage (RoutingMatrix& m);` — returns true iff it transitioned to Active.
  - `void release (RoutingMatrix& m);`
  - `void noteUserMuteChanged() noexcept;`
  - `bool reset() noexcept;` — returns true iff any state was cleared (so callers refresh UI only when needed).

- [ ] **Step 1: Write the failing tests** (append inside the anonymous namespace in `tests/CoreLogicTests.cpp`, after the matrix tests)

```cpp
    // ---------------------------------------------------------------------------
    // PanicController (Phase C1) -- mirrors the pre-extraction MainComponent logic
    // ---------------------------------------------------------------------------
    void test_panic_engage_saves_and_mutes_all()
    {
        dcr::RoutingMatrix m;
        m.resize (3, 2);
        m.setInputMute (1, true); // one input already muted before panic

        dcr::PanicController p;
        CHECK (!p.isActive());
        CHECK (p.engage (m) == true);
        CHECK (p.isActive());
        CHECK (p.state() == dcr::PanicController::State::Active);
        // every channel now muted
        for (int n = 0; n < 3; ++n)
            CHECK (m.getInputMute (n));
        for (int o = 0; o < 2; ++o)
            CHECK (m.getOutputMute (o));
    }

    void test_panic_engage_empty_matrix_is_noop()
    {
        dcr::RoutingMatrix m;
        m.resize (0, 0);
        dcr::PanicController p;
        CHECK (p.engage (m) == false);
        CHECK (!p.isActive());
    }

    void test_panic_release_restores_prior_state()
    {
        dcr::RoutingMatrix m;
        m.resize (3, 2);
        m.setInputMute (1, true);
        m.setOutputMute (0, true);

        dcr::PanicController p;
        p.engage (m);
        p.release (m);
        CHECK (!p.isActive());
        // exactly the pre-panic mute pattern is back
        CHECK (!m.getInputMute (0));
        CHECK (m.getInputMute (1));
        CHECK (!m.getInputMute (2));
        CHECK (m.getOutputMute (0));
        CHECK (!m.getOutputMute (1));
    }

    void test_panic_release_when_inactive_is_noop()
    {
        dcr::RoutingMatrix m;
        m.resize (2, 2);
        m.setInputMute (0, true);
        dcr::PanicController p;
        p.release (m); // must not throw / must not change anything
        CHECK (!p.isActive());
        CHECK (m.getInputMute (0));
        CHECK (!m.getInputMute (1));
    }

    void test_panic_forget_drops_saved_state_without_touching_matrix()
    {
        dcr::RoutingMatrix m;
        m.resize (2, 1);
        dcr::PanicController p;
        p.engage (m); // everything muted, prior state (all-unmuted) saved
        // user manually unmutes input 0 while panic active
        m.setInputMute (0, false);
        p.noteUserMuteChanged();
        CHECK (!p.isActive());
        // matrix unchanged by the forget -- input 0 stays unmuted, rest muted
        CHECK (!m.getInputMute (0));
        CHECK (m.getInputMute (1));
        // re-engaging now saves the CURRENT pattern (fresh), then releasing
        // returns to it -- proves the stale saved state was truly dropped
        p.engage (m);
        p.release (m);
        CHECK (!m.getInputMute (0));
        CHECK (m.getInputMute (1));
    }

    void test_panic_forget_when_inactive_is_noop()
    {
        dcr::PanicController p;
        p.noteUserMuteChanged();
        CHECK (!p.isActive());
    }

    void test_panic_reset_clears_state_and_reports()
    {
        dcr::RoutingMatrix m;
        m.resize (2, 2);
        dcr::PanicController p;
        p.engage (m);
        CHECK (p.reset() == true); // had active state to clear
        CHECK (!p.isActive());
        CHECK (p.reset() == false); // nothing left to clear
    }
```

- [ ] **Step 2: Register the tests in `main`** (in `tests/CoreLogicTests.cpp`, after the `test_matrix_*` block)

```cpp
    test_panic_engage_saves_and_mutes_all();
    test_panic_engage_empty_matrix_is_noop();
    test_panic_release_restores_prior_state();
    test_panic_release_when_inactive_is_noop();
    test_panic_forget_drops_saved_state_without_touching_matrix();
    test_panic_forget_when_inactive_is_noop();
    test_panic_reset_clears_state_and_reports();
```

- [ ] **Step 3: Add the `#include`** at the top of `tests/CoreLogicTests.cpp` (with the other `Routing/` includes)

```cpp
#include "Routing/PanicController.h"
```

- [ ] **Step 4: Run the tests to verify they fail to compile** (the class does not exist yet)

Run: `cmake --build build --target dcorerouter_tests`
Expected: FAIL — `'PanicController.h' file not found` / `no member named 'PanicController' in namespace 'dcr'`.

- [ ] **Step 5: Write `Source/Routing/PanicController.h`**

```cpp
#pragma once

#include <vector>

namespace dcr
{

    class RoutingMatrix;

    // Explicit owner of the PANIC feature's state (Phase C1).  Before this, the
    // panic flag and the saved pre-panic mute vectors lived loose in
    // MainComponent and were written from five sites; concentrating them here
    // gives the feature one writer and makes its transitions testable headless.
    //
    // Message-thread only (mirrors the RoutingMatrix UI-setter contract); no
    // locks, no allocation beyond the saved-state vectors.
    class PanicController
    {
    public:
        enum class State
        {
            Inactive,
            Active
        };

        State state() const noexcept { return state_; }
        bool isActive() const noexcept { return state_ == State::Active; }

        // First press: snapshot every channel's current mute, then mute all of
        // them.  No-op (stays Inactive, returns false) on an empty matrix.
        // Returns true iff it transitioned to Active.
        bool engage (RoutingMatrix& m);

        // Second press: restore the snapshot, drop it, return to Inactive.
        // Safe no-op when already Inactive.
        void release (RoutingMatrix& m);

        // The user manually toggled a mute while panic was active, so the saved
        // pre-panic snapshot is now stale: forget it and return to Inactive.
        // The matrix is intentionally left as the user just set it.  No-op when
        // Inactive.
        void noteUserMuteChanged() noexcept;

        // The matrix is about to be rebuilt (device reconfigure): the saved
        // indices would point at the old layout, so drop all panic state
        // WITHOUT touching the matrix.  Returns true iff anything was cleared
        // (so the caller can refresh the button only when needed).
        bool reset() noexcept;

    private:
        State state_ = State::Inactive;
        std::vector<unsigned char> savedInputMutes;
        std::vector<unsigned char> savedOutputMutes;
    };

}
```

- [ ] **Step 6: Write `Source/Routing/PanicController.cpp`**

```cpp
#include "PanicController.h"

#include "RoutingMatrix.h"

#include <algorithm>

namespace dcr
{

    bool PanicController::engage (RoutingMatrix& m)
    {
        const int nIn = m.getNumInputs();
        const int nOut = m.getNumOutputs();
        if (nIn == 0 && nOut == 0)
            return false;

        savedInputMutes.assign ((size_t) nIn, 0);
        savedOutputMutes.assign ((size_t) nOut, 0);
        for (int n = 0; n < nIn; ++n)
        {
            savedInputMutes[(size_t) n] = m.getInputMute (n) ? 1 : 0;
            m.setInputMute (n, true);
        }
        for (int o = 0; o < nOut; ++o)
        {
            savedOutputMutes[(size_t) o] = m.getOutputMute (o) ? 1 : 0;
            m.setOutputMute (o, true);
        }
        state_ = State::Active;
        return true;
    }

    void PanicController::release (RoutingMatrix& m)
    {
        // min() guards against the channel count having shrunk since engage.
        const int nIn = std::min ((int) savedInputMutes.size(), m.getNumInputs());
        const int nOut = std::min ((int) savedOutputMutes.size(), m.getNumOutputs());
        for (int n = 0; n < nIn; ++n)
            m.setInputMute (n, savedInputMutes[(size_t) n] != 0);
        for (int o = 0; o < nOut; ++o)
            m.setOutputMute (o, savedOutputMutes[(size_t) o] != 0);
        savedInputMutes.clear();
        savedOutputMutes.clear();
        state_ = State::Inactive;
    }

    void PanicController::noteUserMuteChanged() noexcept
    {
        if (state_ != State::Active)
            return;
        state_ = State::Inactive;
        savedInputMutes.clear();
        savedOutputMutes.clear();
    }

    bool PanicController::reset() noexcept
    {
        if (state_ == State::Inactive && savedInputMutes.empty() && savedOutputMutes.empty())
            return false;
        state_ = State::Inactive;
        savedInputMutes.clear();
        savedOutputMutes.clear();
        return true;
    }

}
```

- [ ] **Step 7: Add `PanicController.cpp` to both CMake targets**

In `CMakeLists.txt`, add `Source/Routing/PanicController.cpp` to the `dcorerouter` app `target_sources` list (beside `Source/Routing/RoutingMatrix.cpp`, ~line 39) AND to the `dcorerouter_tests` executable sources (beside `Source/Routing/RoutingMatrix.cpp`, ~line 110).

- [ ] **Step 8: Build + run the pure-logic tests**

Run: `cmake --build build --target dcorerouter_tests && ctest --test-dir build -R dcorerouter_tests --output-on-failure`
Expected: PASS — all `test_panic_*` checks green, `0 failures`.

- [ ] **Step 9: Format the two new files**

Run (per-file due to clang-format v22 batch bug):
```bash
clang-format Source/Routing/PanicController.h > /tmp/x && mv /tmp/x Source/Routing/PanicController.h
clang-format Source/Routing/PanicController.cpp > /tmp/x && mv /tmp/x Source/Routing/PanicController.cpp
clang-format --dry-run --Werror Source/Routing/PanicController.h Source/Routing/PanicController.cpp
```
Expected: no output (clean).

- [ ] **Step 10: Commit**

```bash
git add Source/Routing/PanicController.h Source/Routing/PanicController.cpp CMakeLists.txt tests/CoreLogicTests.cpp
git commit -m "feat(state): add PanicController state machine (Phase C1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: Rewire MainComponent onto PanicController

**Files:**
- Modify: `Source/MainComponent.h:60-70` (remove `inPanic`/saved vectors, add member + include)
- Modify: `Source/MainComponent.cpp` (8 sites listed below)

**Interfaces:**
- Consumes: `dcr::PanicController` (from Task 1).
- Produces: no new public surface; MainComponent's private panic methods become thin coordinators (controller call + UI refresh).

- [ ] **Step 1: Edit `Source/MainComponent.h`** — replace the three data members with the controller. Add near the other `Routing/` includes:

```cpp
#include "Routing/PanicController.h"
```

Replace lines 68-70:
```cpp
        bool inPanic = false;
        std::vector<unsigned char> savedInputMutes;
        std::vector<unsigned char> savedOutputMutes;
```
with:
```cpp
        PanicController panic;
```
(Keep the `panicActivate/panicRelease/panicResetRestart/updatePanicButtonAppearance` declarations — they remain the UI coordinators.)

- [ ] **Step 2: Rewire `panicActivate` (`MainComponent.cpp:1026`)**

```cpp
    void MainComponent::panicActivate()
    {
        if (! panic.engage (engine.getRoutingMatrix()))
            return; // empty matrix -- nothing to mute
        matrixView.refreshMuteButtonStates();
        updatePanicButtonAppearance();
    }
```

- [ ] **Step 3: Rewire `panicRelease` (`MainComponent.cpp:1051`)**

```cpp
    void MainComponent::panicRelease()
    {
        panic.release (engine.getRoutingMatrix());
        matrixView.refreshMuteButtonStates();
        updatePanicButtonAppearance();
    }
```

- [ ] **Step 4: Rewire the forget callback (`MainComponent.cpp:240`)**

```cpp
        matrixView.onUserMuteChanged = [this] {
            panic.noteUserMuteChanged();
            updatePanicButtonAppearance();
        };
```

- [ ] **Step 5: Rewire `updatePanicButtonAppearance` (`MainComponent.cpp:1067`)** — replace every `inPanic` read with `panic.isActive()`:

```cpp
    void MainComponent::updatePanicButtonAppearance()
    {
        const bool active = panic.isActive();
        stopButton.setButtonText (active ? "PANIC*" : "PANIC");
        stopButton.setColour (juce::TextButton::buttonColourId,
            active ? juce::Colour::fromRGB (180, 30, 30)
                   : juce::Colour::fromRGB (50, 50, 56));
        stopButton.setColour (juce::TextButton::buttonOnColourId,
            active ? juce::Colour::fromRGB (180, 30, 30)
                   : juce::Colour::fromRGB (50, 50, 56));
        stopButton.repaint();

        if (resetButton.isVisible() != active)
        {
            resetButton.setVisible (active);
            resized();
        }
    }
```

- [ ] **Step 6: Rewire `panicResetRestart` (`MainComponent.cpp:1088`)** — change `if (inPanic)` to `if (panic.isActive())`:

```cpp
        if (panic.isActive())
            panicRelease();
```

- [ ] **Step 7: Rewire `applyDeviceSelection` reset (`MainComponent.cpp:1124-1132`)**

```cpp
        // Matrix is about to be rebuilt - any saved-panic indices would point at
        // the old channel layout, so drop the panic state cleanly.
        if (panic.reset())
            updatePanicButtonAppearance();
```

- [ ] **Step 8: Rewire the stopButton onClick (`MainComponent.cpp:100`)** and the menu (`545`, `616-620`) — replace `inPanic` with `panic.isActive()`:

`100`:
```cpp
        stopButton.onClick = [this] { if (panic.isActive()) panicRelease(); else panicActivate(); };
```
`545` (menu item label/tick):
```cpp
                m.addItem (miPanic, panic.isActive() ? "Release PANIC (restore mutes)" : "PANIC (mute everything)", true, panic.isActive());
```
`617`:
```cpp
                if (panic.isActive())
                    panicRelease();
                else
                    panicActivate();
```

- [ ] **Step 9: Grep to prove no stray references remain**

Run: `grep -rn "inPanic\|savedInputMutes\|savedOutputMutes" Source/`
Expected: no matches.

- [ ] **Step 10: Build the app + run all tests**

Run:
```bash
cmake --build build -j
cmake --build build --target dcorerouter_tests dcorerouter_tests_juce
ctest --test-dir build --output-on-failure
```
Expected: app links, both suites PASS.

- [ ] **Step 11: Format changed files + tree-wide check**

```bash
clang-format Source/MainComponent.cpp > /tmp/x && mv /tmp/x Source/MainComponent.cpp
clang-format Source/MainComponent.h   > /tmp/x && mv /tmp/x Source/MainComponent.h
clang-format --dry-run --Werror Source/MainComponent.cpp Source/MainComponent.h
```
Expected: clean.

- [ ] **Step 12: Commit**

```bash
git add Source/MainComponent.h Source/MainComponent.cpp
git commit -m "refactor(ui): derive PANIC state from PanicController (Phase C1)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Self-Review

- **Spec coverage (C1):** ✅ `enum class PanicState`→`PanicController::State`; saved vectors owned internally (single owner); engage/release pair; the "user manually changed a mute" is the explicit `noteUserMuteChanged` transition, not a side write; button + menu derive from `panic.isActive()` (no scattered `inPanic` reads — Task 2 Step 9 grep proves it).
- **Placeholder scan:** none — every step shows the exact code/command.
- **Type consistency:** `engage` returns `bool` (used in Task 2 Step 2); `reset` returns `bool` (used in Step 7); `isActive()` used consistently; `State::Active/Inactive` consistent.
- **Behavior preservation:** the `engage`/`release` bodies are lifted verbatim from `panicActivate`/`panicRelease` (only `inPanic=...` → `state_=...`); `reset`'s guard reproduces the `inPanic || !saved.empty()` condition; tests pin each transition.

**Phase C1 done when:** PanicController is an explicit single-owner unit-tested state machine; MainComponent holds no parallel panic state; app + both test suites build and pass; format clean tree-wide.

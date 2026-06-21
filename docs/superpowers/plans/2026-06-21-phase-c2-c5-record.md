# Phase C2–C5 — Implementation Record

**Date:** 2026-06-21
**Branch:** `overhaul/phase-c-statemachines` (rebased onto `origin/main`, incl. Group-faders #7)
**Design:** `docs/superpowers/specs/2026-06-20-drouter-overhaul-design.md` §Phase C
**C1 plan (separate):** `docs/superpowers/plans/2026-06-21-phase-c1-panic-controller.md`

C1 (PanicController) shipped under its own bite-sized plan + review. C2–C5 were
executed in a velocity pass (user: "keep doing C2–C5, will verify later"); this
file is the design/decision record in lieu of per-increment plan files. Each
increment is its own commit, build-verified, with pure logic unit-tested where
applicable.

---

## C2 — PanelHost ✅ done (commit `c68f816`)

**What:** Collapsed the three near-identical `*PanelDetached` bool + `DocumentWindow`
+ `toggle*PanelDetach()` trios (status / input-group / output-group) into a single
parameterized `dcr::PanelHost` slot (`Source/UI/PanelHost.{h,cpp}`) with an explicit
`enum class State { Attached, Detached }`.

**Design:** `PanelHost` holds non-owning refs to the host (embed target) + panel,
owns the floating window while Detached, and exposes three hooks the owner wires:
`windowSize` (lazy size at detach time), `setPanelDetached` (forwards to the panel's
typed `setDetached(bool)`), `onChanged` (re-run tab layout). The floating-window
class moved out of MainComponent into PanelHost. MainComponent's `resized()`/
`switchTab()` now read `host.isDetached()` instead of three parallel bools; a literal
duplicate visibility block in `switchTab` was dropped.

**Not unit-tested** — pure JUCE Component/DocumentWindow plumbing, not headless-
testable. Build-verified only. **Needs visual verification:** detach/reattach each
panel (pop-out button + window close), tab switching with panels detached.

---

## C3 — ReconfigurationController ✅ phase machine done (commit `ae1ff97`); relocation deferred

**What (done):** Replaced MainComponent's bare `std::atomic<bool> isReconfiguring`
with `dcr::ReconfigurationController` (`Source/Engine/ReconfigurationController.{h,cpp}`):
an explicit, single-owner, forward-only phase machine
`Idle → Draining → Rebuilding → RestoringMatrix → RestoringPlugins → Running`.
- `tryBegin()` — the re-entry guard (CAS Idle→Draining), replaces `exchange(true)`.
- `advance(next)` — moves only to the immediate successor; rejects out-of-order.
- `finish()` — any phase → Idle.
- `active()` / `phase()` — atomic reads (the device-format watchdog reads `active()`).

Phase transitions are set at the existing worker/message sync points as **additive
atomic stores** — no reordering of the delicate fade → `stopProcessor` → harvest →
`engine.stop()/start()` → callAsync-restore sequence. Pure transition logic
(re-entry rejection, forward-only ordering, terminal `Running`, reuse after
`finish`) is unit-tested in `dcorerouter_tests` (4 cases).

**Payload relocation — NOW DONE** (commit `95faeee`, after the phase machine):
`PendingSnapshotApply` + the plugin-load queue/cursor/startMs moved out of
MainComponent into `ReconfigurationController` (its single owner). MainComponent
accesses them via `reconfig.snapshot()`/`pluginQueue()`/`pluginCursor()`/
`pluginStartMs()`; each using-scope binds a **local reference alias** so the delicate
worker/message bodies stay byte-identical — no change to thread timing or the
single-thread `pendingSnap` discipline (still touched only on the message thread; the
worker harvests into a local `harvestedChains` and hands over via callAsync). The
payload carries JUCE/Snapshot types, so the controller is now JUCE-linked: its 5
phase-machine + payload tests moved to `dcorerouter_tests_juce`
(`ReconfigurationControllerTests`, `juce::UnitTest`). **Independent code-review of the
relocation: behavior-preserving, no bugs, thread discipline intact** (all 7 alias
sites are references where the original mutated, copies only where read-only; worker
thread never touches the payload; no dangling refs). MainComponent.h −42 lines.

**Needs real-device verification:** Settings change (preserve-state restart), Load
snapshot (cold-start matrix + plugin restore), Reset button, and the auto-recover
device-format watchdog — confirm none stack/stick and audio fades without clicks.

---

## C4 — single-source-of-truth routing UI ✅ already satisfied (no code change)

**Finding:** C4's goal is already met, by the combination of upstream Group-faders #7
and pre-existing design — verified, not assumed:
- **Strip widgets:** user edits write **directly** to `RoutingMatrix`
  (`slider.onValueChange → matrix.setInputTrim`, `mute.onClick → matrix.setInputMute`,
  `MatrixView.cpp:820/857/998/1033`). The widgets hold no authoritative copy.
- **Pull-back:** `refreshTrimWidgetsFromEngine()` resyncs widget values from the
  matrix on the meter tick, gated on `RoutingMatrix::getDirtyGeneration()`
  (`MatrixView.cpp:1363`) — added by #7. Static matrix costs nothing.
- **Crosspoints:** `CrosspointGrid` reads/writes gains live on the matrix
  (`getCrosspoint`/`setCrosspoint`, `CrosspointGrid.cpp`) — never a cached copy.
- **`MatrixStateByName`:** used *only* in the reconfigure capture→restore path
  (`MainComponent.cpp:1016/1100`); it is already a transient (de)serialization
  mapping across device add/remove, not a third live representation.

Fabricating a refactor here would be drive-by churn (risk, no benefit) and the spec
forbids it. **C4 = verified-done.**

---

## C5 — god-file decomposition ✅ emergent shrink realized

Per the spec, C5 is emergent — "files shrink as logic leaves," no big-bang split.
Realized by C1–C3:
- `MainComponent.cpp`: **1972 → 1828 lines** (−144 net; 215 deletions / 70 insertions
  across the three C commits). The extracted logic now lives in three focused files:
  `PanicController` (121), `PanelHost` (151), `ReconfigurationController` (109) =
  381 lines, each with one clear responsibility.
- `MainComponent.h`: 240 → 239 (the three detach bools + windows + the panic flag +
  saved vectors collapsed into typed members).
- `MatrixView.cpp`: unchanged (C4 needed no edits).

Further god-file shrink is tied to the **deferred C3 follow-up** (moving the
plugin-load machinery + `pendingSnap` into `ReconfigurationController`), which would
pull a large block out of `MainComponent.cpp` — to be done with device verification.

---

## Phase C status

| Increment | State | Verified |
|---|---|---|
| C1 PanicController | done | unit tests + review |
| C2 PanelHost | done | build; **needs visual** |
| C3 ReconfigurationController (phase machine) | done | unit tests + build |
| C3 pendingSnap/plugin-queue relocation | done | review + unit tests + build; **needs device** |
| C4 single-source-of-truth | already satisfied | verified by inspection |
| C5 god-file shrink | emergent (−144 lines) | build |

Branch not pushed to fork/origin. App + both test suites build & pass; format clean
tree-wide. DSP untouched throughout.

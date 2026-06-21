# D-Router — Architecture + Industrialization Overhaul

**Date:** 2026-06-20
**Status:** Approved (design); Phase A planned next
**Scope:** Engine, Routing, UI, Persistence, Diagnostics. **DSP is explicitly out of scope.**

## Goal

Take an ambitious, well-engineered real-time audio router that has *no* engineering
infrastructure and a state-duplication problem at the UI↔engine boundary, and:

1. Industrialize it to C++/JUCE best practice (the missing scaffolding).
2. Fix the concrete fragile spots.
3. Improve the architecture by replacing implicit booleans/scattered state with
   explicit state machines and single owners.

Driving principle (from `developing-with-claude-advice.md`): **remove ambiguity** —
one source of truth, one writer, explicit states, explicit contracts, then verify
against reality. The scaffolding exists so we *can* verify.

## Constraints / working agreement

- **DSP excluded** for now (Source/DSP/Builtin/ and the plugin hosts' DSP internals).
- Every code change on a branch; small, reviewable commits (advice #11).
- Test-first for anything with edge cases (advice #13).
- Match existing JUCE house style; do **not** reformat the tree.
- The real-time engine is sound — preserve its behavior. Redesign is incremental,
  not a rewrite. The lock-free SPSC ring, the atomic `RoutingMatrix` + `dirtyGen`
  snapshot model, and the dedicated matrix thread stay as-is.
- Each phase ships behind its own implementation plan (writing-plans) and branch.
  This document is the shared design; plans are per-phase to stay reviewable.

## Lowest-risk-first ordering

`Phase A (scaffold) → Phase B (fragiles) → Phase C (architecture)`. The scaffolding
gives us tests + CI before we touch fragile real-time code; the fragile fixes give
us hardened, tested persistence/diagnostics before we restructure the state model.

---

## Phase A — Industrialization scaffolding

Near-zero behavioral risk. Establishes the safety net.

> **Reconciliation (2026-06-20):** after rebasing onto current `origin/main`, much
> of A was already merged upstream — `run.sh` fix, `CLAUDE.md`, and a *pure-C++*
> (no-JUCE) test harness with RingBuffer + RoutingMatrix tests (PR #3); some Phase
> B fragiles too (`587b176`). The remaining A gaps — `.clang-format`, `.clang-tidy`,
> `.editorconfig`, CI, and **persistence tests** — are in the revised plan. Because
> upstream's test target is deliberately JUCE-free, the JUCE-dependent persistence
> tests live in a SEPARATE `dcorerouter_tests_juce` target (JUCE `UnitTest`), leaving
> the fast pure-logic target untouched. See
> `docs/superpowers/plans/2026-06-20-phase-a-scaffolding.md`.

### A1. `CLAUDE.md`
Codify the conventions the source comments already encode, so every session stops
re-learning (and re-violating) them:
- `dcr::` namespace everywhere.
- **AU plugins create/destroy on the message thread only** (the async plugin-load
  queue exists for this reason).
- **Never clobber per-channel output trims** for restart fades — use
  `MatrixProcessor::setMasterGainTarget` (a prior bug leaked a stuck −60 dB state).
- `MatrixProcessor`: `pool` MUST be declared before `thread` (destruction order).
- Ring buffers are strict SPSC — one producer (device callback), one consumer
  (matrix thread); never share an end across threads.
- `pendingSnap` is single-threaded by discipline (drained on the message thread).
- Build/run/test commands; "DSP is currently out of scope for refactors."

### A2. `.clang-format`
Authored to reproduce the current JUCE house style (Allman braces, space-before-
paren in calls, pointer alignment, column alignment) so `clang-format --dry-run`
on the existing tree yields ~zero diff. Verified before commit. Formatting stays
opt-in per file; CI only *checks*, never rewrites.

### A3. `.clang-tidy`
A curated set (bugprone-*, a focused slice of cppcoreguidelines-*, performance-*),
NOT the firehose. Warnings, not errors, initially. Tuned so the existing code is
mostly clean and the signal is real.

### A4. Test target `dcorerouter_tests`
- JUCE built-in `juce::UnitTest` + a small console runner; separate CMake target
  linking the same Source/ objects it needs (no app bundle).
- First tests cover pure, high-value logic the redesign will lean on:
  - `FloatRingBuffer`: wrap-around, fill/drain accounting, overflow/underflow returns.
  - `RoutingMatrix`: `takeSnapshot` correctness, `dirtyGen` bumps, solo-aware
    effective gain, blocked-crosspoint forcing to 0.
  - `OutputGroupManager` / `InputGroupManager`: linked-fader math.
  - `SnapshotStore` / `SettingsStore`: round-trip (serialize → deserialize → equal).
- These round-trip tests double as the regression guard for Phase B.

### A5. CI — GitHub Actions, macOS runner
Steps: configure (FetchContent JUCE, cached) → build app → build + run
`dcorerouter_tests` → `clang-format --dry-run --Werror` check. Lint (clang-tidy)
as a non-blocking job initially. **CI builds + tests + lints only** — no packaged
artifact (kept out per decision; `package.sh` remains a manual release step).

### A6. Housekeeping
- Fix `run.sh`: the bundle is `D-Router.app` (CMake `PRODUCT_NAME`), not
  `dcorerouter.app`; `killall` target is `D-Router`, not `dcorerouter`
  (run.sh:29, 44, 47).
- Add `.editorconfig`.

**Phase A done when:** `cmake -B build && cmake --build build` builds the app and
the test target, `ctest`/the runner passes, the format check passes on the tree,
and CI is green.

---

## Phase B — Fix the fragiles (test-first)

With A's tests in place, each fix starts with a failing test that reproduces the gap.

### B1. Persistence robustness
- **Atomic writes** for both stores: write to a `TemporaryFile`, then `moveFileTo`
  (replaces `xml->writeTo(file)` at SnapshotStore.cpp:379, SettingsStore.cpp:110).
  Prevents corruption on crash mid-write — which currently destroys the very file
  crash-recovery wants.
- **Snapshot version honored on load**: the writer sets `version=1`
  (SnapshotStore.cpp:79) but the loader never reads it. Read it; reject/branch on
  unknown versions explicitly.
- **Settings version field**: add one (SettingsStore has none).
- **Persist the dropped fields**: the warn/crit threshold settings are defined and
  shown in the UI but never serialized — user customization silently reverts each
  launch. Save + load them.
- **Typed load result**: replace bare `bool` with
  `enum class LoadResult { Ok, NoFile, ParseError, UnsupportedVersion, Corrupt }`
  so crash-recovery distinguishes "no snapshot yet" from "file corrupt" and can
  tell the user, instead of silently showing blank state.
- Tests first: round-trip, truncated/corrupt file, version-mismatch, missing-field.

### B2. Crash handler async-signal-safety
Current handler is acknowledged best-effort ("racy but in practice OK") and still
calls `Logger::getCurrentLogFile()` and `::backtrace` inside the signal handler
(CrashHandler.mm:45–47, 61–62) — neither is async-signal-safe; a crash inside
malloc can deadlock the handler.
- Pre-open the log fd at `install()` time, store it in an `atomic<int>`.
- In the handler: only `write(2)` + `backtrace_symbols_fd` on that pre-opened fd;
  no Logger access, no allocation.

### B3. Async lifetime-guard consistency
Audit every async callback — plugin-restore queue, `CallOutBox` FX-chain popups,
loading-overlay progress/finished — and ensure each uniformly honors `aliveToken`
before dereferencing `this`/`engine`/`matrixView`. The "add the missing guard"
fixes land here; the structural cleanup is C. (No DSP internals touched — only the
host-side callback lifetime.)

**Phase B done when:** new tests pass; manual kill-during-save leaves a loadable
file; an induced crash writes a backtrace without hanging.

---

## Phase C — Architecture: explicit state machines (incremental)

Each extraction moves logic *out* of a god file, gets its own unit tests, and
replaces implicit state with an explicit machine + single writer. Engine untouched.

### C1. `PanicController`
- `enum class PanicState { Inactive, Active }`. Owns the saved input/output mute
  vectors internally (single owner). One transition function pair (engage/release);
  the "user manually changed a mute" event is an explicit transition, not a side
  write to `inPanic` + cleared vectors.
- UI button + menu item **derive** their label/toggle from the controller — no
  parallel `inPanic` reads scattered across MainComponent (currently written from
  3 sites: panicActivate:945, panicRelease, the mute-changed callback:250).

### C2. `PanelHost`
- Collapse the 3× duplicated `groupPanelDetached` / `inputGroupPanelDetached` /
  `statusPanelDetached` + 3 windows + 3 near-identical toggle methods into one
  parameterized slot with explicit `Attached | Detached` state.

### C3. `ReconfigurationController` (centerpiece)
- Model the device-reconfigure + snapshot-apply lifecycle as one explicit machine:
  `Idle → Draining → Rebuilding → RestoringMatrix → RestoringPlugins → Running`,
  owning `isReconfiguring`, `pendingSnap`, and the plugin-load queue/cursor that
  are currently spread across MainComponent (1011, 1046–1160, plugin queue).
- Preserves the existing thread discipline (worker thread + message-thread drain);
  makes the phases nameable, testable, and impossible to enter out of order.

### C4. Single-source-of-truth for routing UI
- `MatrixView` derives its widget values from the authoritative `RoutingMatrix`
  rather than holding a parallel live copy; the by-name `MatrixStateByName` shrinks
  to a pure (de)serialization mapping used only across device add/remove, not a
  third live representation.

### C5. God-file decomposition (emergent)
- As C1–C4 land, `MainComponent.cpp` (1820) and `MatrixView.cpp` (1931) shrink
  toward coordinator-sized files. No big-bang split — files shrink as logic leaves.

**Phase C done when:** panic, panel-detach, and reconfiguration are each an
explicit, single-owner, unit-tested state machine; the UI no longer keeps a live
parallel copy of routing state; the two god files are materially smaller.

---

## Risks & mitigations

- **Real-time regressions from refactors** → Phase C never touches the audio path;
  it restructures message-thread coordination only. Engine/matrix code is frozen
  except where a fragile fix (B) requires it, guarded by tests.
- **clang-format churn** → A2 verified to ~zero diff before commit; check-only CI.
- **Scope creep into DSP** → explicit exclusion in CLAUDE.md and every plan.
- **One spec too big to execute** → mitigated by per-phase implementation plans;
  this doc is design only.

## Out of scope (this effort)

- All DSP/Builtin processor internals and DSP correctness.
- Notarization / distribution pipeline beyond the existing `package.sh`.
- Cross-platform (macOS only).
- Feature work (no new routing/plugin features).

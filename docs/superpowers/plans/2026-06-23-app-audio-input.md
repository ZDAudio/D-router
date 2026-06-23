# App Audio Input Implementation Plan — Part 1: JUCE-free tested logic

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Land the two JUCE-free, unit-tested decision units that the rest of the
app-audio-input feature builds on — the auto-reattach **resolver** and the matrix
**never-stall** rule — guarded by the headless `dcorerouter_tests` suite.

**Architecture:** Both units are pure functions in header-only files under
`Source/Engine/`, in the same spirit as the existing extracted logic
(`ResonanceMath`, `StereoMeterMath`, `PdcPlan`). They take plain
(`std::string`/`int`/`bool`) inputs and return plain results, so they compile into
the JUCE-free test target and are exercised by the existing `CHECK`-macro harness
in `tests/CoreLogicTests.cpp`. No JUCE, no CoreAudio, no engine wiring in this part.

**Tech Stack:** C++20, header-only inline functions, CMake + CTest
(`dcorerouter_tests` target), the existing hand-rolled `CHECK` test harness.

---

## Scope & decomposition

The full feature (see `docs/superpowers/specs/2026-06-23-app-audio-input-design.md`)
spans subsystems with very different verification stories. The `dcorerouter_tests`
target is **JUCE-free**, so only deterministic, JUCE-free logic can be unit-tested
headless. This Part 1 plan implements exactly that logic. The device-linked work is
explicitly deferred to follow-up plans because it is not headless-testable and its
CoreAudio API must be written against a reference implementation and verified on a
macOS 14.4+ device:

- **Part 2 — CoreAudio capture (real-device):** `AppAudioProcesses` (process
  enumerate/resolve/watch) and `AppAudioWorker` (process tap + private aggregate
  device + IOProc + SRC + rings). Covers spec §5, §8. Written against
  [AudioCap](https://github.com/insidegui/AudioCap) on a 14.4+ device.
- **Part 3 — Integration:** `AppInputSpec` data model + global-channel wiring in
  `AudioEngine`, the `GlobalInput.attached` flag + calling `planMatrixInput` from
  `MatrixProcessor`, Soft-In group auto-creation, `DeviceManagerDialog` /
  `OutputGroupPanel` / `GroupManagerDialog` UI, `Snapshot` persistence
  (`appInputs` + `Group.kind`), and permissions/14.4 gating. Covers spec §4, §6,
  §7(wiring), §10–§16. Each gets task detail after Part 2 lands.

Each part produces software that stands on its own: Part 1 lands tested library
functions; Parts 2/3 build on them.

## File structure (Part 1)

- **Create** `Source/Engine/AppInputResolver.h` — JUCE-free `reconcile()`: given the
  configured sources and the running-process set, emit attach/detach commands.
  Single responsibility: the auto-reattach decision (spec §9).
- **Create** `Source/Engine/MatrixInputPlan.h` — JUCE-free `planMatrixInput()`: how
  the matrix treats one input each block (hardware stalls vs. app never-stalls,
  spec §7).
- **Modify** `tests/CoreLogicTests.cpp` — add the two includes, the test functions,
  and their calls in `main()`.

Both headers are header-only `inline` functions, so no CMake source-list edit is
needed — `CoreLogicTests.cpp` already resolves `"Engine/..."` includes and the app
picks them up through normal includes.

**Pre-req:** a configured build dir. If `build/` is absent, run once:
`cmake -B build -DCMAKE_BUILD_TYPE=Release`

---

### Task 1: AppInputResolver — auto-reattach decision

**Files:**
- Create: `Source/Engine/AppInputResolver.h`
- Test: `tests/CoreLogicTests.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/CoreLogicTests.cpp`, add the include alongside the existing engine
includes (near line 13–15):

```cpp
#include "Engine/AppInputResolver.h"
```

Add these test functions inside the anonymous `namespace` (e.g. after
`test_update_version_compare`, before the closing `} // namespace` at line 804):

```cpp
    // ---------------------------------------------------------------------------
    // AppInputResolver (auto-reattach: reconcile configured app sources with the
    // set of processes currently running, keyed by bundle id).
    // ---------------------------------------------------------------------------
    void test_appinput_reconcile_attach_when_running()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 0 } }; // offline
        std::vector<RunningProcess> running { { "com.google.Chrome", 42 } };
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 1);
        CHECK (cmds[0].type == CommandType::Attach);
        CHECK (cmds[0].sourceIndex == 0);
        CHECK (cmds[0].processId == 42);
    }

    void test_appinput_reconcile_noop_when_offline_and_not_running()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.apple.Music", 0 } };
        std::vector<RunningProcess> running {}; // nothing running
        auto cmds = reconcile (sources, running);
        CHECK (cmds.empty());
    }

    void test_appinput_reconcile_steady_state_no_command()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 42 } }; // attached to 42
        std::vector<RunningProcess> running { { "com.google.Chrome", 42 } };
        auto cmds = reconcile (sources, running);
        CHECK (cmds.empty());
    }

    void test_appinput_reconcile_detach_when_quit()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 42 } }; // attached
        std::vector<RunningProcess> running {}; // Chrome quit
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 1);
        CHECK (cmds[0].type == CommandType::Detach);
        CHECK (cmds[0].sourceIndex == 0);
    }

    void test_appinput_reconcile_relaunch_detach_then_attach()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources { { "com.google.Chrome", 42 } }; // attached to old pid
        std::vector<RunningProcess> running { { "com.google.Chrome", 99 } }; // relaunched
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 2);
        CHECK (cmds[0].type == CommandType::Detach);
        CHECK (cmds[0].sourceIndex == 0);
        CHECK (cmds[1].type == CommandType::Attach);
        CHECK (cmds[1].sourceIndex == 0);
        CHECK (cmds[1].processId == 99);
    }

    void test_appinput_reconcile_multiple_sources_mixed()
    {
        using namespace dcr::appinput;
        std::vector<Source> sources {
            { "com.google.Chrome", 0 }, // offline -> attach
            { "com.apple.Music", 7 }, // attached, still running -> noop
            { "com.foo.Game", 12 }, // attached, quit -> detach
        };
        std::vector<RunningProcess> running {
            { "com.google.Chrome", 42 },
            { "com.apple.Music", 7 },
        };
        auto cmds = reconcile (sources, running);
        CHECK (cmds.size() == 2);
        CHECK (cmds[0].type == CommandType::Attach);
        CHECK (cmds[0].sourceIndex == 0);
        CHECK (cmds[0].processId == 42);
        CHECK (cmds[1].type == CommandType::Detach);
        CHECK (cmds[1].sourceIndex == 2);
    }
```

Add their calls in `main()` (after the `test_update_version_compare();` call near
line 859):

```cpp
    test_appinput_reconcile_attach_when_running();
    test_appinput_reconcile_noop_when_offline_and_not_running();
    test_appinput_reconcile_steady_state_no_command();
    test_appinput_reconcile_detach_when_quit();
    test_appinput_reconcile_relaunch_detach_then_attach();
    test_appinput_reconcile_multiple_sources_mixed();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target dcorerouter_tests`
Expected: **compile failure** — `fatal error: 'Engine/AppInputResolver.h' file not found`.

- [ ] **Step 3: Create the header (implementation)**

Create `Source/Engine/AppInputResolver.h`:

```cpp
#pragma once

// JUCE-free decision logic for auto-reattaching app-audio capture sources to
// running processes.  The CoreAudio glue (process enumeration, tap creation)
// lives elsewhere and is not headless-testable; THIS file is the deterministic
// core -- given the configured sources and the processes currently running, it
// emits the attach/detach commands needed to reconcile them.  Pure, no side
// effects.  Mirrors the codebase pattern of extracting JUCE-free logic
// (ResonanceMath, PdcPlan) for the headless suite.

#include <string>
#include <vector>

namespace dcr::appinput
{
    // A configured app-capture source as the resolver sees it.  bundleId is the
    // stable persistence key (e.g. "com.google.Chrome").  attachedProcessId is the
    // CoreAudio process-object id this source is currently bound to, or 0 when the
    // source is offline (attached to no process).
    struct Source
    {
        std::string bundleId;
        int attachedProcessId = 0; // 0 == not attached
    };

    // One process the watcher observed, mapped from bundle id to its CoreAudio
    // process-object id (always > 0 for a real process).
    struct RunningProcess
    {
        std::string bundleId;
        int processId = 0;
    };

    enum class CommandType
    {
        Attach,
        Detach
    };

    struct Command
    {
        CommandType type;
        int sourceIndex; // index into the sources vector passed to reconcile()
        int processId; // Attach: the process to bind; Detach: 0
    };

    // Reconcile configured sources against the running-process set.  Per source:
    //   running, source offline               -> Attach
    //   running, attached to the SAME pid      -> no-op (steady state)
    //   running, attached to a DIFFERENT pid   -> Detach + Attach (app relaunched)
    //   not running, source attached           -> Detach
    //   not running, source offline            -> no-op (stays offline/waiting)
    // Commands are emitted in source order; a relaunch emits Detach before Attach.
    inline std::vector<Command> reconcile (const std::vector<Source>& sources,
        const std::vector<RunningProcess>& running)
    {
        std::vector<Command> cmds;
        for (int i = 0; i < (int) sources.size(); ++i)
        {
            const auto& s = sources[(size_t) i];

            int foundPid = 0;
            for (const auto& rp : running)
                if (rp.bundleId == s.bundleId)
                {
                    foundPid = rp.processId;
                    break;
                }

            const bool sourceAttached = (s.attachedProcessId != 0);

            if (foundPid != 0)
            {
                if (! sourceAttached)
                {
                    cmds.push_back ({ CommandType::Attach, i, foundPid });
                }
                else if (s.attachedProcessId != foundPid)
                {
                    // App relaunched with a new pid: drop the stale tap, bind the new one.
                    cmds.push_back ({ CommandType::Detach, i, 0 });
                    cmds.push_back ({ CommandType::Attach, i, foundPid });
                }
                // else attached to the same pid -> steady state, nothing to do.
            }
            else if (sourceAttached)
            {
                cmds.push_back ({ CommandType::Detach, i, 0 });
            }
        }
        return cmds;
    }
} // namespace dcr::appinput
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure`
Expected: build succeeds; CTest reports the suite passing with `0 failures` (the
summary line `N checks, 0 failures`).

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/AppInputResolver.h tests/CoreLogicTests.cpp
git commit -m "feat(appinput): JUCE-free auto-reattach resolver + tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: MatrixInputPlan — the matrix never-stall rule

**Files:**
- Create: `Source/Engine/MatrixInputPlan.h`
- Test: `tests/CoreLogicTests.cpp`

- [ ] **Step 1: Write the failing tests**

In `tests/CoreLogicTests.cpp`, add the include next to the Task 1 include:

```cpp
#include "Engine/MatrixInputPlan.h"
```

Add these test functions in the anonymous `namespace` (after the Task 1 tests):

```cpp
    // ---------------------------------------------------------------------------
    // MatrixInputPlan (per-input read/silence/stall decision).  Hardware inputs
    // gate the matrix (stall until a full block is ready); app inputs NEVER stall
    // -- an offline or warming-up app contributes silence instead of freezing all
    // audio.
    // ---------------------------------------------------------------------------
    void test_matrixinput_hardware_reads_when_full()
    {
        auto p = dcr::planMatrixInput (/*isAppInput*/ false, /*attached*/ false, /*avail*/ 128, /*block*/ 128);
        CHECK (p.action == dcr::MatrixInputAction::Read);
        CHECK (! p.stalls);
    }

    void test_matrixinput_hardware_stalls_when_underfull()
    {
        auto p = dcr::planMatrixInput (false, false, 64, 128);
        CHECK (p.stalls); // hardware underfull -> the matrix waits
    }

    void test_matrixinput_app_reads_when_attached_and_full()
    {
        auto p = dcr::planMatrixInput (true, /*attached*/ true, 128, 128);
        CHECK (p.action == dcr::MatrixInputAction::Read);
        CHECK (! p.stalls);
    }

    void test_matrixinput_app_silence_when_attached_but_underfull()
    {
        auto p = dcr::planMatrixInput (true, true, 64, 128);
        CHECK (p.action == dcr::MatrixInputAction::Silence);
        CHECK (! p.stalls); // never stalls the matrix
    }

    void test_matrixinput_app_silence_when_detached()
    {
        auto p = dcr::planMatrixInput (true, /*attached*/ false, 9999, 128);
        CHECK (p.action == dcr::MatrixInputAction::Silence);
        CHECK (! p.stalls);
    }
```

Add their calls in `main()` (after the Task 1 calls):

```cpp
    test_matrixinput_hardware_reads_when_full();
    test_matrixinput_hardware_stalls_when_underfull();
    test_matrixinput_app_reads_when_attached_and_full();
    test_matrixinput_app_silence_when_attached_but_underfull();
    test_matrixinput_app_silence_when_detached();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build --target dcorerouter_tests`
Expected: **compile failure** — `fatal error: 'Engine/MatrixInputPlan.h' file not found`.

- [ ] **Step 3: Create the header (implementation)**

Create `Source/Engine/MatrixInputPlan.h`:

```cpp
#pragma once

// JUCE-free rule for how the matrix treats one input channel each block.
//
// Hardware inputs GATE the matrix: if a hardware ring hasn't accumulated a full
// block yet, the matrix waits (stalls) -- it must not run ahead of real capture.
// App-audio inputs must NEVER stall the matrix: an app that is offline (its tap
// detached) or still warming up would otherwise freeze ALL audio.  An app input
// that isn't ready contributes silence for the block instead.
//
// Pure decision; MatrixProcessor calls it per input when deciding availability
// and reads (wiring is Part 3).  When `stalls` is true the caller bails the whole
// block and `action` is irrelevant.

namespace dcr
{
    enum class MatrixInputAction
    {
        Read, // read blockSize samples from this input's ring
        Silence // memset this input's block to zero
    };

    struct MatrixInputPlan
    {
        MatrixInputAction action;
        bool stalls; // true -> this input may hold up the whole block (hardware only)
    };

    // isAppInput : true for an app-audio capture input, false for a hardware input.
    // attached   : (app inputs) is a tap currently bound and producing?  ignored for hardware.
    // available  : samples currently readable in this input's ring.
    // blockSize  : the engine block the matrix wants to read.
    inline MatrixInputPlan planMatrixInput (bool isAppInput, bool attached, int available, int blockSize)
    {
        if (! isAppInput)
        {
            // Hardware: read when a full block is ready, otherwise stall the matrix.
            if (available >= blockSize)
                return { MatrixInputAction::Read, false };
            return { MatrixInputAction::Read, true }; // stall: action ignored by caller
        }

        // App: never stall.  Read only when attached AND a full block is ready.
        if (attached && available >= blockSize)
            return { MatrixInputAction::Read, false };
        return { MatrixInputAction::Silence, false };
    }
} // namespace dcr
```

- [ ] **Step 4: Run the tests to verify they pass**

Run: `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure`
Expected: build succeeds; CTest reports `0 failures`.

- [ ] **Step 5: Commit**

```bash
git add Source/Engine/MatrixInputPlan.h tests/CoreLogicTests.cpp
git commit -m "feat(appinput): JUCE-free matrix never-stall rule + tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Self-review

**Spec coverage (Part 1):**
- §9 auto-reattach state machine → Task 1 (`reconcile`), 6 cases incl. relaunch & mixed.
- §7 matrix never-stall rule → Task 2 (`planMatrixInput`), hardware-vs-app stall/silence.
- §17 testing bullets 1 & 3 (resolver state machine; matrix silence decision) → both tasks.
- Everything else (§4 data model, §5 worker, §6 wiring, §8 watcher, §10–§16) is
  explicitly deferred to Parts 2/3 with section mapping above — not dropped.

**Placeholder scan:** none — both headers and all tests are complete code; commands
and expected output are concrete.

**Type consistency:** Task 1 uses `Source`/`RunningProcess`/`Command`/`CommandType`
(`Attach`/`Detach`) and fields `bundleId`/`attachedProcessId`/`processId`/
`sourceIndex` consistently between the header and tests. Task 2 uses
`planMatrixInput`/`MatrixInputPlan`/`MatrixInputAction` (`Read`/`Silence`) and
fields `action`/`stalls` consistently. The two headers share namespace `dcr`
(Task 2) / `dcr::appinput` (Task 1) without collision.

---

## Next

After Part 1 lands, run a fresh writing-plans pass for **Part 2 (CoreAudio
capture)** against the AudioCap reference on a macOS 14.4+ device, then **Part 3
(integration + UI + persistence + permissions)**.

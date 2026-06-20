# Phase A — Industrialization Scaffolding Implementation Plan (revised)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fill the *remaining* engineering-infrastructure gaps in D-Router — formatting + lint config, a JUCE-linked test target with the first persistence regression tests, and CI — without changing app behavior.

**Architecture:** Pure additive scaffolding on top of current `origin/main`. The existing fast pure-C++ test target `dcorerouter_tests` (RingBuffer + RoutingMatrix, no JUCE) stays exactly as-is. A NEW `dcorerouter_tests_juce` console target (JUCE `UnitTest` + AudioToolbox) holds tests that need JUCE — starting with SnapshotStore/SettingsStore round-trips. Config files codify the existing JUCE house style. CI runs build + both test targets + format-check-on-changed-files on a macOS runner.

**Tech Stack:** C++20, JUCE 8.0.4 (FetchContent), CMake ≥ 3.22, Ninja, JUCE `UnitTest`, GitHub Actions (macOS), clang-format, clang-tidy.

## Status reconciliation (why this plan was revised)

`origin/main` advanced past the branch point and ALREADY contains, so these are dropped:
- `run.sh` bundle/process-name fix (`627538a`, now uses `$PRODUCT_NAME`).
- `CLAUDE.md` (`fbf0757`, `c3ce1e1`) — comprehensive: build, invariants, tests, style.
- Pure-logic test harness + RingBuffer + RoutingMatrix tests (`571f6cf`, PR #3) — kept untouched.
- Several Phase B fragiles (`587b176`: RT alloc, gate hysteresis, plugin layout swap, dedup) — re-assessed in Phase B.

**Remaining Phase A gaps (this plan):** `.editorconfig`, `.clang-format`, `.clang-tidy`, a JUCE-linked test target with SnapshotStore + SettingsStore tests, and CI.

## Global Constraints

- Platform: macOS 12+ only. CMake + Ninja; JUCE fetched at 8.0.4.
- Namespace `dcr::`. C++20.
- **DSP out of scope** — do not test or modify `Source/DSP/**`.
- Match existing JUCE house style; do NOT reformat existing files.
- App target `dcorerouter`; bundle/process name `D-Router`.
- Existing `dcorerouter_tests` (pure-logic, no JUCE) is NOT modified — add a separate target.
- Every task ends in a commit on branch `overhaul/phase-a-scaffolding`.
- Engine/Routing/audio-path behavior frozen.

---

### Task 1: `.editorconfig`

**Files:**
- Create: `.editorconfig`

**Interfaces:**
- Consumes: nothing. Produces: editor config only.

- [ ] **Step 1: Create `.editorconfig`**

```ini
root = true

[*]
charset = utf-8
end_of_line = lf
insert_final_newline = true
trim_trailing_whitespace = true

[*.{cpp,h,mm,hpp}]
indent_style = space
indent_size = 4

[*.{yml,yaml}]
indent_style = space
indent_size = 2
```

- [ ] **Step 2: Commit**

```bash
git add .editorconfig
git commit -m "chore: add .editorconfig"
```

---

### Task 2: `.clang-format` matching existing JUCE style

**Files:**
- Create: `.clang-format`

**Interfaces:**
- Consumes: nothing. Produces: format config used by Task 6's CI check.

- [ ] **Step 1: Create `.clang-format`**

`ColumnLimit: 0` is deliberate — it stops clang-format reflowing the hand-aligned long lines (the main churn source).

```yaml
---
Language: Cpp
BasedOnStyle: LLVM
Standard: c++20
ColumnLimit: 0
IndentWidth: 4
TabWidth: 4
UseTab: Never
AccessModifierOffset: -4
NamespaceIndentation: None
PointerAlignment: Left
BreakBeforeBraces: Allman
SpaceBeforeParens: Always
SpaceBeforeCpp11BracedList: true
SpacesInAngles: Never
AlignAfterOpenBracket: Align
AlignConsecutiveAssignments: Consecutive
AlignConsecutiveDeclarations: Consecutive
AllowShortFunctionsOnASingleLine: Inline
AllowShortBlocksOnASingleLine: Always
AllowShortIfStatementsOnASingleLine: Never
KeepEmptyLinesAtTheStartOfBlocks: false
MaxEmptyLinesToKeep: 2
SortIncludes: false
...
```

- [ ] **Step 2: Confirm clang-format available**

Run: `clang-format --version`
Expected: a version string (LLVM ≥ 12 or Apple). If missing: `brew install clang-format`.

- [ ] **Step 3: Measure diff on representative files**

Run:
```bash
for f in Source/Routing/RoutingMatrix.cpp Source/Engine/RingBuffer.h Source/Main.cpp; do
  echo "== $f =="; clang-format "$f" | diff -u "$f" - | head -40
done
```
Expected: only small, local, clearly-cosmetic differences (or none). If clang-format reflows whole blocks or destroys the column alignment, adjust (commonly set `AlignConsecutiveAssignments`/`AlignConsecutiveDeclarations: None`, or `AllowShortBlocksOnASingleLine: Never`) and re-run until minimal. Do NOT edit source files.

- [ ] **Step 4: Commit**

```bash
git add .clang-format
git commit -m "chore: add .clang-format matching existing JUCE house style"
```

---

### Task 3: `.clang-tidy` (curated, warnings-only)

**Files:**
- Create: `.clang-tidy`

**Interfaces:**
- Consumes: nothing. Produces: curated check set used by Task 6's non-blocking lint job.

- [ ] **Step 1: Create `.clang-tidy`**

```yaml
---
# Curated, high-signal checks only. Warnings, not errors, in Phase A.
# DSP is out of scope; CI runs tidy on non-DSP Source files only.
Checks: >
  -*,
  bugprone-*,
  -bugprone-easily-swappable-parameters,
  -bugprone-narrowing-conversions,
  performance-*,
  misc-misplaced-const,
  misc-redundant-expression,
  modernize-use-override,
  modernize-use-nullptr,
  cppcoreguidelines-pro-type-member-init,
  cppcoreguidelines-slicing
WarningsAsErrors: ''
HeaderFilterRegex: 'Source/(Engine|Routing|UI|Persistence|Diagnostics)/.*'
FormatStyle: file
```

- [ ] **Step 2: Verify it parses**

Run: `clang-tidy --dump-config --config-file=.clang-tidy >/dev/null && echo OK`
Expected: `OK`. If `clang-tidy` is absent: `brew install llvm`.

- [ ] **Step 3: Commit**

```bash
git add .clang-tidy
git commit -m "chore: add curated .clang-tidy (warnings-only, non-DSP)"
```

---

### Task 4: JUCE-linked test target + SnapshotStore round-trip tests

Foundational JUCE-test task: stands up `dcorerouter_tests_juce` (separate from the pure-logic target), proves it builds/links/runs, and lands the first persistence tests. Fold all harness setup here.

**Files:**
- Create: `tests/juce/TestMainJuce.cpp`
- Create: `tests/juce/SnapshotStoreTests.cpp`
- Modify: `CMakeLists.txt` (append a second test target after the existing `dcorerouter_tests` block, ~line 113)

**Interfaces:**
- Consumes: `dcr::Snapshot`, `dcr::SnapshotStore` (`Source/Persistence/SnapshotStore.h`): `static juce::ValueTree toValueTree(const Snapshot&)`, `static Snapshot fromValueTree(const juce::ValueTree&)`. `Snapshot` fields: `engineSampleRate`, `engineBlockSize`, `inputTrim`, `outputTrim`, `inputMute`, `crosspoints` (`{outputCh,inputCh,gain}`), `outputGroups` (`Group{name,layoutName,memberChannels,faderDb,muted}`).
- Produces: a `dcorerouter_tests_juce` target registered with ctest; a `juce::UnitTest` runner pattern (static instances + a `main`) that Task 5 extends by adding one `.cpp` to the target's sources.

- [ ] **Step 1: Write the runner `tests/juce/TestMainJuce.cpp`**

```cpp
// Console runner for JUCE-dependent D-Router unit tests. Runs every
// statically-registered juce::UnitTest; non-zero exit on any failure (ctest).
#include <juce_core/juce_core.h>

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        if (auto* r = runner.getResult (i))
            failures += r->failures;

    juce::Logger::writeToLog (failures == 0 ? "ALL JUCE TESTS PASSED"
                                            : "JUCE TESTS FAILED: " + juce::String (failures));
    return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Write `tests/juce/SnapshotStoreTests.cpp`**

```cpp
#include <juce_core/juce_core.h>
#include "Persistence/SnapshotStore.h"

using dcr::Snapshot;
using dcr::SnapshotStore;

struct SnapshotStoreTests : juce::UnitTest
{
    SnapshotStoreTests() : juce::UnitTest ("SnapshotStore") {}

    void runTest() override
    {
        beginTest ("toValueTree -> fromValueTree preserves scalar + vector state");
        {
            Snapshot s;
            s.engineSampleRate = 44100.0;
            s.engineBlockSize  = 256;
            s.inputTrim  = { 1.0f, 0.5f };
            s.outputTrim = { 0.25f };
            s.inputMute  = { 0, 1 };
            s.crosspoints.push_back ({ /*out*/ 0, /*in*/ 1, /*gain*/ 0.75f });

            auto r = SnapshotStore::fromValueTree (SnapshotStore::toValueTree (s));

            expectEquals (r.engineSampleRate, 44100.0);
            expectEquals (r.engineBlockSize, 256);
            expectEquals ((int) r.inputTrim.size(), 2);
            expectEquals (r.inputTrim[1], 0.5f);
            expectEquals ((int) r.outputTrim.size(), 1);
            expectEquals (r.outputTrim[0], 0.25f);
            expectEquals ((int) r.inputMute.size(), 2);
            expect (r.inputMute[1] != 0);
            expectEquals ((int) r.crosspoints.size(), 1);
            expectEquals (r.crosspoints[0].outputCh, 0);
            expectEquals (r.crosspoints[0].inputCh, 1);
            expectEquals (r.crosspoints[0].gain, 0.75f);
        }

        beginTest ("group state round-trips (name, layout, members, fader, mute)");
        {
            Snapshot s;
            Snapshot::Group g;
            g.name = "Mains"; g.layoutName = "Stereo";
            g.memberChannels = { 0, 1 }; g.faderDb = -6.0f; g.muted = true;
            s.outputGroups.push_back (g);

            auto r = SnapshotStore::fromValueTree (SnapshotStore::toValueTree (s));
            expectEquals ((int) r.outputGroups.size(), 1);
            expectEquals (r.outputGroups[0].name, juce::String ("Mains"));
            expectEquals (r.outputGroups[0].layoutName, juce::String ("Stereo"));
            expectEquals ((int) r.outputGroups[0].memberChannels.size(), 2);
            expectEquals (r.outputGroups[0].faderDb, -6.0f);
            expect (r.outputGroups[0].muted);
        }

        beginTest ("empty snapshot round-trips without error");
        {
            Snapshot s;
            auto r = SnapshotStore::fromValueTree (SnapshotStore::toValueTree (s));
            expectEquals ((int) r.inputTrim.size(), 0);
            expectEquals ((int) r.crosspoints.size(), 0);
        }
    }
};

static SnapshotStoreTests snapshotStoreTests;
```

- [ ] **Step 3: Add the second test target to root `CMakeLists.txt`**

Append AFTER the existing `add_test(NAME dcorerouter_tests ...)` line (~line 113). It links the app's JUCE module set + AudioToolbox because `SnapshotStore.h` transitively includes `Engine/AudioEngine.h` (for `AudioEngine::DeviceSpec`) and `Engine/EngineSettings.h` (AudioToolbox).

```cmake

# ---- JUCE-linked tests (persistence etc.; need ValueTree/XML/File) ---------
juce_add_console_app(dcorerouter_tests_juce PRODUCT_NAME "dcorerouter_tests_juce")
juce_generate_juce_header(dcorerouter_tests_juce)

target_sources(dcorerouter_tests_juce PRIVATE
    tests/juce/TestMainJuce.cpp
    tests/juce/SnapshotStoreTests.cpp
    Source/Persistence/SnapshotStore.cpp
)
target_include_directories(dcorerouter_tests_juce PRIVATE Source)
target_compile_definitions(dcorerouter_tests_juce PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
)
target_link_libraries(dcorerouter_tests_juce PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_dsp
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra
    juce::juce_opengl
    "-framework AudioToolbox"
    "-framework CoreAudio"
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
)
add_test(NAME dcorerouter_tests_juce COMMAND dcorerouter_tests_juce)
```

- [ ] **Step 4: Configure + build the new target**

Run:
```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target dcorerouter_tests_juce -j 8
```
Expected: configures (first run fetches JUCE — slow), compiles, links. If `cmake`/`ninja` missing: `brew install cmake ninja`. If a link error names `AudioEngine`/`DeviceWorker` symbols, `SnapshotStore.cpp` references engine code beyond inline `DeviceSpec` — STOP and report; do not pull `AudioEngine.cpp` in (it drags the whole engine + threads into the test binary).

- [ ] **Step 5: Run and verify pass**

Run: `ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: `100% tests passed`. The `SnapshotStore` suite appears.

- [ ] **Step 6: Verify the harness has teeth**

Temporarily change one assertion (e.g. `expectEquals (r.engineBlockSize, 256)` → `255`), rebuild, run ctest, confirm it FAILS, then revert and confirm it passes. Proves the runner surfaces failures.

- [ ] **Step 7: Commit**

```bash
git add tests/juce/ CMakeLists.txt
git commit -m "test: add JUCE-linked test target + SnapshotStore round-trip tests"
```

---

### Task 5: SettingsStore round-trip tests

**Files:**
- Create: `tests/juce/SettingsStoreTests.cpp`
- Modify: `CMakeLists.txt` (add the test file + `Source/Persistence/SettingsStore.cpp` to `dcorerouter_tests_juce`)

**Interfaces:**
- Consumes: `dcr::EngineSettings` (`Source/Engine/EngineSettings.h`), `dcr::SettingsStore` (`Source/Persistence/SettingsStore.h`): `static bool save(const EngineSettings&)`, `static EngineSettings load()`, `static juce::File getFile()`. Uses a fixed on-disk path; the test backs up/restores any existing file so it never clobbers a real user file.
- Produces: nothing downstream. Regression guard for Phase B's "persist the dropped threshold fields" + version work.

- [ ] **Step 1: Write `tests/juce/SettingsStoreTests.cpp`**

Documents CURRENT behavior: only asserts the fields that ARE persisted today, and `logMessage`s the known threshold-field gap (kept green in Phase A; Phase B flips the assertion on).

```cpp
#include <juce_core/juce_core.h>
#include "Persistence/SettingsStore.h"

using dcr::EngineSettings;
using dcr::SettingsStore;

struct SettingsStoreTests : juce::UnitTest
{
    SettingsStoreTests() : juce::UnitTest ("SettingsStore") {}

    void runTest() override
    {
        // Protect any real user settings file: back up, restore on scope exit.
        const juce::File   file   = SettingsStore::getFile();
        const bool         had    = file.existsAsFile();
        const juce::String backup = had ? file.loadFileAsString() : juce::String();

        beginTest ("persisted audio-path fields survive save -> load");
        {
            EngineSettings s;
            s.engineSampleRate    = 96000.0;
            s.engineBlockSize     = 512;
            s.outputPreFillBlocks = 12;
            s.gainSmoothingMs     = 40;

            expect (SettingsStore::save (s));
            EngineSettings r = SettingsStore::load();

            expectEquals (r.engineSampleRate, 96000.0);
            expectEquals (r.engineBlockSize, 512);
            expectEquals (r.outputPreFillBlocks, 12);
            expectEquals (r.gainSmoothingMs, 40);
        }

        // KNOWN GAP (fixed in Phase B): warn/crit threshold fields not yet
        // persisted. Documented here; asserted in Phase B.
        logMessage ("NOTE: stalledWarnRatio/cpuWarnRatio persistence is a known "
                    "Phase B gap; not asserted in Phase A.");

        if (had) file.replaceWithText (backup);
        else     file.deleteFile();
    }
};

static SettingsStoreTests settingsStoreTests;
```

- [ ] **Step 2: Extend the target in `CMakeLists.txt`**

Add to `dcorerouter_tests_juce`'s `target_sources(...)` so it includes:
```cmake
    tests/juce/SettingsStoreTests.cpp
    Source/Persistence/SettingsStore.cpp
```
(alongside the existing `TestMainJuce.cpp`, `SnapshotStoreTests.cpp`, `SnapshotStore.cpp`).

- [ ] **Step 3: Build and run**

Run:
```bash
cmake --build build --target dcorerouter_tests_juce -j 8
ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure
```
Expected: all pass; `SettingsStore` suite runs and the NOTE line appears.

- [ ] **Step 4: Commit**

```bash
git add tests/juce/SettingsStoreTests.cpp CMakeLists.txt
git commit -m "test: add SettingsStore save/load round-trip (documents Phase B gap)"
```

---

### Task 6: GitHub Actions CI

**Files:**
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: the build, both ctest targets, and `.clang-format` from earlier tasks.
- Produces: a green CI pipeline on push/PR.

- [ ] **Step 1: Create `.github/workflows/ci.yml`**

Format job checks only files changed vs the base ref (best practice; avoids legacy-diff noise). clang-tidy is a separate non-blocking job.

```yaml
name: CI
on:
  push:
    branches: [main]
  pull_request:

jobs:
  build-and-test:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
      - name: Install build tools
        run: brew install cmake ninja
      - name: Cache JUCE FetchContent
        uses: actions/cache@v4
        with:
          path: build/_deps
          key: juce-8.0.4-${{ runner.os }}
      - name: Configure
        run: cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
      - name: Build app
        run: cmake --build build -j
      - name: Build tests
        run: cmake --build build --target dcorerouter_tests --target dcorerouter_tests_juce -j
      - name: Run tests
        run: ctest --test-dir build --output-on-failure

  format-check:
    runs-on: macos-14
    steps:
      - uses: actions/checkout@v4
        with:
          fetch-depth: 0
      - name: Install clang-format
        run: brew install clang-format
      - name: clang-format changed files
        run: |
          base="${{ github.event.pull_request.base.sha }}"
          if [ -z "$base" ]; then base="$(git rev-parse HEAD~1)"; fi
          changed=$(git diff --name-only "$base" HEAD -- '*.cpp' '*.h' '*.mm' \
                    | grep -v '^Source/DSP/' || true)
          if [ -z "$changed" ]; then echo "no non-DSP C++ files changed"; exit 0; fi
          echo "$changed"
          clang-format --dry-run --Werror $changed

  lint:
    runs-on: macos-14
    continue-on-error: true   # non-blocking in Phase A
    steps:
      - uses: actions/checkout@v4
      - name: Install tools
        run: brew install cmake ninja llvm
      - name: Configure (compile commands)
        run: cmake -B build -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_BUILD_TYPE=Debug
      - name: clang-tidy non-DSP sources
        run: |
          files=$(git ls-files 'Source/**/*.cpp' | grep -v '^Source/DSP/' || true)
          [ -z "$files" ] && exit 0
          $(brew --prefix llvm)/bin/clang-tidy -p build $files || true
```

- [ ] **Step 2: Validate the YAML**

Run: `python3 -c "import yaml; yaml.safe_load(open('.github/workflows/ci.yml')); print('YAML OK')"`
Expected: `YAML OK`.

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: add macOS build+test, format-check, non-blocking clang-tidy"
```

- [ ] **Step 4: (After push) confirm CI green**

After the branch is pushed and a PR opened, confirm `build-and-test` and `format-check` pass and `lint` runs (non-blocking). Pushing is a separate, user-authorized step — do not push without asking.

---

## Self-Review

**Spec coverage (remaining gaps only):** `.editorconfig` → Task 1. `.clang-format` → Task 2. `.clang-tidy` → Task 3. JUCE test target + SnapshotStore tests → Task 4. SettingsStore tests → Task 5. CI build+test+format+lint, no artifact → Task 6. Items already upstream (run.sh, CLAUDE.md, RingBuffer/RoutingMatrix tests, harness) are intentionally excluded per the Status reconciliation section. Group-manager tests remain deferred to Phase C (where that code is touched).

**Placeholder scan:** No TBD/TODO; every code/config step shows full content; every command lists expected output. Task 5's "documents current behavior" test is explicit about what it asserts vs. the Phase B gap.

**Type consistency:** Test code uses APIs verified from headers — `dcr::Snapshot`/`SnapshotStore::to/fromValueTree`, `Snapshot::Group`/`Crosspoint` fields, `EngineSettings` fields + `SettingsStore::save/load/getFile`. New target name `dcorerouter_tests_juce` and `target_sources` list are consistent across Tasks 4–5. The existing `dcorerouter_tests` target is never modified.

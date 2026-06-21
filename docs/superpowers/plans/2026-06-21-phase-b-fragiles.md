# D-Router Phase B — Fix the Fragiles Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Harden D-Router's persistence and diagnostics — make file writes crash-safe, make snapshot loads report *why* they failed, stop silently dropping user settings, make the crash handler async-signal-safe, and close the async lifetime-guard gaps — without touching DSP or the real-time audio path.

**Architecture:** Test-first against the Phase A JUCE-linked regression net (`dcorerouter_tests_juce`). Persistence robustness is factored into one shared atomic-write helper that both stores call. The snapshot loader gains a typed `LoadResult` so callers can distinguish "no file yet" from "corrupt". The crash handler pre-opens its log fd at install time so the signal path only does `write(2)` + `backtrace_symbols_fd`. The async audit adds the existing `aliveToken` guard to the two `callAsync` sites that lack it.

**Tech Stack:** C++20, JUCE 8 (`juce::TemporaryFile`, `juce::ValueTree`/XML, `juce::UnitTest`), CoreAudio/Foundation (Obj-C++ for the crash handler), CMake, CTest.

## Global Constraints

- **DSP is out of scope.** Do not touch `Source/DSP/`, `Source/DSP/Builtin/`, or any plugin-host DSP internals.
- **C++20**, JUCE house style (Allman braces, space-before-paren in calls). Match surrounding code; comment the *why*, especially for RT-safety and lifetime ordering.
- **`dcr::` namespace** for all new symbols.
- **Real-time safety** is unaffected by this phase (persistence/diagnostics/message-thread only) — but never add allocation/locks to any `processBlock`, the matrix thread, or a CoreAudio callback.
- **clang-format runs ONE FILE AT A TIME** here (the installed v22 mishandles batch args and `-i` is broken). Format each touched file with: `clang-format <file> > /tmp/cf && mv /tmp/cf <file>`.
- **Public-repo hygiene:** no third-party brand/product names in code/comments/commit messages.
- **Small, reviewable commits**; end every commit message with the trailer:
  `Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>`
- **Branch:** all work lands on `overhaul/phase-b-fragiles` (already created off current `main`). Push to **`fork`**, never `origin`.
- **Build + test commands:**
  - Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release` (once)
  - Build JUCE tests: `cmake --build build --target dcorerouter_tests_juce -j`
  - Run JUCE tests: `ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
  - Build the app (catches caller-update breakage): `cmake --build build -j`
  - Pure-logic regression net (unchanged): `ctest --test-dir build -R '^dcorerouter_tests$' --output-on-failure`

---

## File Structure

- **Create** `Source/Persistence/AtomicXmlWrite.h` / `.cpp` — one free function `dcr::writeXmlAtomically(xml, target)` shared by both stores.
- **Create** `tests/juce/AtomicXmlWriteTests.cpp` — unit tests for the helper.
- **Create** `tests/juce/CrashHandlerTests.cpp` — process-level (fork) test of the signal path.
- **Modify** `Source/Persistence/SettingsStore.cpp` — atomic write, `version` field, persist 4 threshold fields + clamp.
- **Modify** `Source/Persistence/SnapshotStore.h` / `.cpp` — `LoadResult` enum + `kVersion`, version-honoring `load()`, atomic `save()`.
- **Modify** `Source/MainComponent.cpp` — update 3 `load()` callers to the typed result; add `aliveToken` guards to 2 `callAsync` sites.
- **Modify** `Source/Diagnostics/CrashHandler.mm` — pre-open log fd at install; async-signal-safe handler.
- **Modify** `tests/juce/SettingsStoreTests.cpp` / `SnapshotStoreTests.cpp` — new robustness cases.
- **Modify** `CMakeLists.txt` — add the new sources to `dcorerouter_tests_juce`.

---

## Task 1: Atomic XML write helper

**Files:**
- Create: `Source/Persistence/AtomicXmlWrite.h`
- Create: `Source/Persistence/AtomicXmlWrite.cpp`
- Create: `tests/juce/AtomicXmlWriteTests.cpp`
- Modify: `CMakeLists.txt` (add the two new non-test sources + the test source to `dcorerouter_tests_juce`)

**Interfaces:**
- Produces: `bool dcr::writeXmlAtomically (const juce::XmlElement& xml, const juce::File& target);` — serialises `xml` to a `juce::TemporaryFile` in the target's directory, then atomically renames it over `target`. Creates the parent directory first. Returns `false` if the temp write or the rename fails. Consumed by Tasks 2 and 4.

- [ ] **Step 1: Write the helper header**

Create `Source/Persistence/AtomicXmlWrite.h`:

```cpp
#pragma once

#include <juce_core/juce_core.h>

namespace dcr
{

    // Atomically write `xml` to `target`: serialise into a TemporaryFile in the
    // SAME directory, then rename it over the target.  A crash (or power loss)
    // mid-write then leaves the previous file intact instead of a half-written,
    // unparseable one -- which matters because the bare xml->writeTo() this
    // replaces could corrupt the very snapshot/settings file recovery wants.
    // Returns false if the temp write or the atomic replace fails.
    bool writeXmlAtomically (const juce::XmlElement& xml, const juce::File& target);

} // namespace dcr
```

- [ ] **Step 2: Write the failing test**

Create `tests/juce/AtomicXmlWriteTests.cpp`:

```cpp
#include "Persistence/AtomicXmlWrite.h"
#include <juce_core/juce_core.h>

struct AtomicXmlWriteTests : juce::UnitTest
{
    AtomicXmlWriteTests() : juce::UnitTest ("AtomicXmlWrite") {}

    void runTest() override
    {
        beginTest ("writes parseable content to a fresh path");
        {
            juce::TemporaryFile scratch; // owns + deletes its path on scope exit
            const juce::File target = scratch.getFile();

            juce::XmlElement xml ("root");
            xml.setAttribute ("k", "v1");
            expect (dcr::writeXmlAtomically (xml, target));
            expect (target.existsAsFile());

            auto back = juce::parseXML (target);
            expect (back != nullptr);
            expectEquals (back->getStringAttribute ("k"), juce::String ("v1"));
        }

        beginTest ("atomically replaces existing content");
        {
            juce::TemporaryFile scratch;
            const juce::File target = scratch.getFile();

            juce::XmlElement a ("root");
            a.setAttribute ("k", "v1");
            expect (dcr::writeXmlAtomically (a, target));

            juce::XmlElement b ("root");
            b.setAttribute ("k", "v2");
            expect (dcr::writeXmlAtomically (b, target));

            auto back = juce::parseXML (target);
            expect (back != nullptr);
            expectEquals (back->getStringAttribute ("k"), juce::String ("v2"));
        }

        beginTest ("leaves no temp sibling for the target");
        {
            juce::TemporaryFile scratch;
            const juce::File target = scratch.getFile();

            juce::XmlElement xml ("root");
            expect (dcr::writeXmlAtomically (xml, target));

            auto leftovers = target.getParentDirectory()
                                 .findChildFiles (juce::File::findFiles, false, "*.temp");
            for (auto& f : leftovers)
                expect (! f.getFileName().startsWith (target.getFileName()));
        }
    }
};

static AtomicXmlWriteTests atomicXmlWriteTests;
```

- [ ] **Step 3: Wire the new sources into CMake**

In `CMakeLists.txt`, inside the `target_sources(dcorerouter_tests_juce PRIVATE ...)` block (currently ends with `Source/Persistence/SettingsStore.cpp`), add the helper impl and the test:

```cmake
target_sources(dcorerouter_tests_juce PRIVATE
    tests/juce/TestMainJuce.cpp
    tests/juce/SnapshotStoreTests.cpp
    tests/juce/SettingsStoreTests.cpp
    tests/juce/AtomicXmlWriteTests.cpp
    Source/Persistence/SnapshotStore.cpp
    Source/Persistence/SettingsStore.cpp
    Source/Persistence/AtomicXmlWrite.cpp
)
```

- [ ] **Step 4: Run the test to verify it fails (link error)**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target dcorerouter_tests_juce -j`
Expected: FAIL to link — `writeXmlAtomically` is declared/tested but not defined.

- [ ] **Step 5: Write the implementation**

Create `Source/Persistence/AtomicXmlWrite.cpp`:

```cpp
#include "Persistence/AtomicXmlWrite.h"

namespace dcr
{

    bool writeXmlAtomically (const juce::XmlElement& xml, const juce::File& target)
    {
        target.getParentDirectory().createDirectory();

        // TemporaryFile (target) places the temp file in the SAME directory, so
        // overwriteTargetFileWithTemporary() is a same-filesystem rename --
        // atomic on macOS (no cross-device copy).
        juce::TemporaryFile temp (target);
        if (! xml.writeTo (temp.getFile()))
            return false;
        return temp.overwriteTargetFileWithTemporary();
    }

} // namespace dcr
```

- [ ] **Step 6: Build and run the test to verify it passes**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: PASS — `AtomicXmlWrite` test group all green.

- [ ] **Step 7: Format the changed files (one at a time)**

```bash
for f in Source/Persistence/AtomicXmlWrite.h Source/Persistence/AtomicXmlWrite.cpp tests/juce/AtomicXmlWriteTests.cpp; do
  clang-format "$f" > /tmp/cf && mv /tmp/cf "$f"
done
```

- [ ] **Step 8: Commit**

```bash
git add Source/Persistence/AtomicXmlWrite.h Source/Persistence/AtomicXmlWrite.cpp \
        tests/juce/AtomicXmlWriteTests.cpp CMakeLists.txt
git commit -m "feat(persistence): add atomic XML write helper

Shared writeXmlAtomically() serialises to a same-dir TemporaryFile then
renames over the target, so a crash mid-write leaves the prior file intact
instead of a truncated one. Used next by both persistence stores.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: SettingsStore robustness — atomic write, version field, persist dropped thresholds

**Files:**
- Modify: `Source/Persistence/SettingsStore.cpp` (identifiers ~12-30, `load()` 40-90, `save()` 92-119)
- Test: `tests/juce/SettingsStoreTests.cpp`

**Interfaces:**
- Consumes: `dcr::writeXmlAtomically` (Task 1).
- Produces: no signature change (`SettingsStore::load()` still returns `EngineSettings`, `save()` still `bool`). New persisted XML attributes: `version`, `stalledWarnRatio`, `stalledCritRatio`, `cpuWarnRatio`, `cpuCritRatio`.

**Notes:** Follow the existing `pdcEnabled` model (a plain `setProperty`/`getProperty` pair) for the four float fields, mirroring the `meterDecayFactor` double-cast + `jlimit` clamp idiom already in the file. The four fields are ratios → clamp to `[0, 1]`. The `version` field is written for forward-compat (future schema migrations); the current loader does not branch on it (there is only v1), matching how snapshot keeps its own scheme — see [SnapshotStore in Task 4]. `EngineSettings` gains **no** new struct member.

- [ ] **Step 1: Write the failing tests**

In `tests/juce/SettingsStoreTests.cpp`, **replace** the trailing Phase A "KNOWN GAP" `logMessage(...)` block (lines 49-52) with these cases (keep the existing `RestoreGuard` and the first `beginTest`):

```cpp
        beginTest ("warn/crit threshold fields survive save -> load");
        {
            EngineSettings s;
            s.stalledWarnRatio = 0.02f;
            s.stalledCritRatio = 0.10f;
            s.cpuWarnRatio = 0.60f;
            s.cpuCritRatio = 0.85f;

            expect (SettingsStore::save (s));
            EngineSettings r = SettingsStore::load();

            expectEquals (r.stalledWarnRatio, 0.02f);
            expectEquals (r.stalledCritRatio, 0.10f);
            expectEquals (r.cpuWarnRatio, 0.60f);
            expectEquals (r.cpuCritRatio, 0.85f);
        }

        beginTest ("settings file carries a version attribute");
        {
            EngineSettings s;
            expect (SettingsStore::save (s));
            auto xml = juce::parseXML (SettingsStore::getFile());
            expect (xml != nullptr);
            expect (xml->hasAttribute ("version"));
            expectEquals (xml->getIntAttribute ("version"), 1);
        }

        beginTest ("out-of-range threshold ratios clamp to [0,1] on load");
        {
            EngineSettings s;
            s.cpuWarnRatio = 5.0f;      // tampered / out of range
            s.stalledWarnRatio = -1.0f;
            expect (SettingsStore::save (s));
            EngineSettings r = SettingsStore::load();
            expectEquals (r.cpuWarnRatio, 1.0f);
            expectEquals (r.stalledWarnRatio, 0.0f);
        }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: FAIL — threshold round-trip mismatches (fields not persisted; load returns defaults 0.01/0.05/0.70/0.90), and no `version` attribute.

- [ ] **Step 3: Add the include and new identifiers**

In `Source/Persistence/SettingsStore.cpp`, after the existing `#include <juce_data_structures/juce_data_structures.h>` (line 3), add:

```cpp
#include "Persistence/AtomicXmlWrite.h"
```

In the anonymous namespace of identifiers (after `const juce::Identifier criticalColor ("criticalColorRGB");`, line 29), add:

```cpp
        const juce::Identifier settingsVersion ("version");
        const juce::Identifier stalledWarnRatio ("stalledWarnRatio");
        const juce::Identifier stalledCritRatio ("stalledCritRatio");
        const juce::Identifier cpuWarnRatio ("cpuWarnRatio");
        const juce::Identifier cpuCritRatio ("cpuCritRatio");

        // Bumped only on an incompatible settings-schema change.  Written for
        // forward-compat; the current loader tolerates any value (there is only
        // v1) and fills missing fields from EngineSettings defaults.
        constexpr int kSettingsVersion = 1;
```

- [ ] **Step 4: Read the new fields in `load()` with clamps**

In `SettingsStore::load()`, after the `s.criticalColorRGB = ...` line (line 71), add the reads:

```cpp
        s.stalledWarnRatio = (float) (double) t.getProperty (stalledWarnRatio, (double) s.stalledWarnRatio);
        s.stalledCritRatio = (float) (double) t.getProperty (stalledCritRatio, (double) s.stalledCritRatio);
        s.cpuWarnRatio = (float) (double) t.getProperty (cpuWarnRatio, (double) s.cpuWarnRatio);
        s.cpuCritRatio = (float) (double) t.getProperty (cpuCritRatio, (double) s.cpuCritRatio);
```

In the same function, in the "Defensive clamp" block, after `s.statusTimerMs = juce::jlimit (100, 10000, s.statusTimerMs);` (line 88), add:

```cpp
        s.stalledWarnRatio = juce::jlimit (0.0f, 1.0f, s.stalledWarnRatio);
        s.stalledCritRatio = juce::jlimit (0.0f, 1.0f, s.stalledCritRatio);
        s.cpuWarnRatio = juce::jlimit (0.0f, 1.0f, s.cpuWarnRatio);
        s.cpuCritRatio = juce::jlimit (0.0f, 1.0f, s.cpuCritRatio);
```

- [ ] **Step 5: Write the new fields + version in `save()` and switch to the atomic write**

In `SettingsStore::save()`, immediately after `juce::ValueTree t (rootId);` (line 94), add:

```cpp
        t.setProperty (settingsVersion, kSettingsVersion, nullptr);
```

After `t.setProperty (criticalColor, (int) s.criticalColorRGB, nullptr);` (line 113), add:

```cpp
        t.setProperty (stalledWarnRatio, (double) s.stalledWarnRatio, nullptr);
        t.setProperty (stalledCritRatio, (double) s.stalledCritRatio, nullptr);
        t.setProperty (cpuWarnRatio, (double) s.cpuWarnRatio, nullptr);
        t.setProperty (cpuCritRatio, (double) s.cpuCritRatio, nullptr);
```

Replace the final write (lines 115-118):

```cpp
        auto xml = t.createXml();
        if (xml == nullptr)
            return false;
        return xml->writeTo (getFile());
```

with:

```cpp
        auto xml = t.createXml();
        if (xml == nullptr)
            return false;
        return writeXmlAtomically (*xml, getFile());
```

- [ ] **Step 6: Build and run the tests to verify they pass**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: PASS — all `SettingsStore` cases green.

- [ ] **Step 7: Format the changed files (one at a time)**

```bash
for f in Source/Persistence/SettingsStore.cpp tests/juce/SettingsStoreTests.cpp; do
  clang-format "$f" > /tmp/cf && mv /tmp/cf "$f"
done
```

- [ ] **Step 8: Commit**

```bash
git add Source/Persistence/SettingsStore.cpp tests/juce/SettingsStoreTests.cpp
git commit -m "fix(persistence): persist threshold settings + version, write atomically

The four warn/crit ratio fields (stalled/cpu) were shown in the UI but never
serialised, so user customisation silently reverted each launch. Persist them
(clamped to [0,1] on load), add a settings schema version for forward-compat,
and route the write through writeXmlAtomically so a crash mid-save can't
truncate settings.xml.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: SnapshotStore typed LoadResult + version honoring

**Files:**
- Modify: `Source/Persistence/SnapshotStore.h` (class decl 81-93)
- Modify: `Source/Persistence/SnapshotStore.cpp` (`toValueTree` 81, `load()` 408-420)
- Modify: `Source/MainComponent.cpp` (3 callers: 307, 331, 1882)
- Test: `tests/juce/SnapshotStoreTests.cpp`

**Interfaces:**
- Produces:
  - `enum class SnapshotStore::LoadResult { Ok, NoFile, ParseError, UnsupportedVersion, Corrupt };`
  - `static constexpr int SnapshotStore::kVersion = 1;`
  - `static LoadResult SnapshotStore::load (const juce::File&, Snapshot&);` (was `bool`)
- Consumes: nothing new.

**Notes:** The writer has always stamped `version=1` (SnapshotStore.cpp:81), so a file of the root type but with a missing/zero version is foreign/corrupt, not a legacy v0. `save()` stays bare `xml->writeTo` for this task; Task 4 makes it atomic (keeps the load-API blast radius — the `MainComponent` caller edits — in its own review gate).

- [ ] **Step 1: Write the failing tests**

In `tests/juce/SnapshotStoreTests.cpp`, add these cases at the end of `runTest()` (after the "empty snapshot" block, before the closing `}` of `runTest`):

```cpp
        beginTest ("load reports NoFile for a missing path");
        {
            juce::TemporaryFile scratch;
            juce::File missing = scratch.getFile();
            missing.deleteFile();
            Snapshot s;
            expect (SnapshotStore::load (missing, s) == SnapshotStore::LoadResult::NoFile);
        }

        beginTest ("round-trips through disk as LoadResult::Ok");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            Snapshot s;
            s.engineSampleRate = 88200.0;
            s.engineBlockSize = 64;
            expect (SnapshotStore::save (file, s));

            Snapshot r;
            expect (SnapshotStore::load (file, r) == SnapshotStore::LoadResult::Ok);
            expectEquals (r.engineSampleRate, 88200.0);
            expectEquals (r.engineBlockSize, 64);
        }

        beginTest ("load reports ParseError on non-XML garbage");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            file.replaceWithText ("this is definitely not xml");
            Snapshot s;
            expect (SnapshotStore::load (file, s) == SnapshotStore::LoadResult::ParseError);
        }

        beginTest ("load reports Corrupt for valid XML of the wrong shape");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            file.replaceWithText ("<somethingElse foo='1'/>");
            Snapshot s;
            expect (SnapshotStore::load (file, s) == SnapshotStore::LoadResult::Corrupt);
        }

        beginTest ("load reports UnsupportedVersion for a future schema");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();
            Snapshot s;
            auto tree = SnapshotStore::toValueTree (s);
            tree.setProperty ("version", SnapshotStore::kVersion + 1, nullptr);
            auto xml = tree.createXml();
            expect (xml != nullptr);
            expect (xml->writeTo (file));
            Snapshot out;
            expect (SnapshotStore::load (file, out) == SnapshotStore::LoadResult::UnsupportedVersion);
        }
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `cmake --build build --target dcorerouter_tests_juce -j`
Expected: FAIL to compile — `SnapshotStore::LoadResult` / `kVersion` don't exist and `load()` returns `bool`.

- [ ] **Step 3: Add the enum + version constant + new load signature to the header**

In `Source/Persistence/SnapshotStore.h`, replace the `public:` section of `class SnapshotStore` (lines 83-92):

```cpp
    public:
        static juce::File getDirectory(); // ~/Library/Application Support/dcorerouter/snapshots
        static juce::File getLastUsedFile(); // ~/Library/Application Support/dcorerouter/last.xml

        static bool save (const juce::File& file, const Snapshot& s);
        static bool load (const juce::File& file, Snapshot& outSnap);

        // ValueTree <-> Snapshot
        static juce::ValueTree toValueTree (const Snapshot& s);
        static Snapshot fromValueTree (const juce::ValueTree& tree);
```

with:

```cpp
    public:
        // Bumped only on an incompatible snapshot-schema change.  toValueTree()
        // stamps it; load() rejects anything newer.
        static constexpr int kVersion = 1;

        // Why a load failed -- lets crash-recovery tell "no snapshot yet" (fine,
        // start blank) apart from "file is corrupt / from a newer build" (worth
        // telling the user) instead of collapsing both into a bare false.
        enum class LoadResult
        {
            Ok,
            NoFile,
            ParseError,
            UnsupportedVersion,
            Corrupt
        };

        static juce::File getDirectory(); // ~/Library/Application Support/dcorerouter/snapshots
        static juce::File getLastUsedFile(); // ~/Library/Application Support/dcorerouter/last.xml

        static bool save (const juce::File& file, const Snapshot& s);
        static LoadResult load (const juce::File& file, Snapshot& outSnap);

        // ValueTree <-> Snapshot
        static juce::ValueTree toValueTree (const Snapshot& s);
        static Snapshot fromValueTree (const juce::ValueTree& tree);
```

- [ ] **Step 4: Use `kVersion` in the writer**

In `Source/Persistence/SnapshotStore.cpp`, in `toValueTree()`, replace line 81:

```cpp
        root.setProperty (ids::version, 1, nullptr);
```

with:

```cpp
        root.setProperty (ids::version, kVersion, nullptr);
```

- [ ] **Step 5: Rewrite `load()` to honor the version and return the typed result**

In `Source/Persistence/SnapshotStore.cpp`, replace the whole `load()` body (lines 408-420):

```cpp
    bool SnapshotStore::load (const juce::File& file, Snapshot& outSnap)
    {
        if (!file.existsAsFile())
            return false;
        auto xml = juce::parseXML (file);
        if (xml == nullptr)
            return false;
        auto tree = juce::ValueTree::fromXml (*xml);
        if (!tree.isValid())
            return false;
        outSnap = fromValueTree (tree);
        return true;
    }
```

with:

```cpp
    SnapshotStore::LoadResult SnapshotStore::load (const juce::File& file, Snapshot& outSnap)
    {
        if (!file.existsAsFile())
            return LoadResult::NoFile;

        auto xml = juce::parseXML (file);
        if (xml == nullptr)
            return LoadResult::ParseError; // not well-formed XML at all

        auto tree = juce::ValueTree::fromXml (*xml);
        if (!tree.isValid() || !tree.hasType (ids::root))
            return LoadResult::Corrupt; // XML, but not one of ours

        // The writer has always stamped version >= 1, so a missing/zero version
        // on a root-typed file means it's foreign or truncated, not a legacy v0.
        const int version = (int) tree.getProperty (ids::version, 0);
        if (version <= 0)
            return LoadResult::Corrupt;
        if (version > kVersion)
            return LoadResult::UnsupportedVersion; // written by a newer build

        outSnap = fromValueTree (tree);
        return LoadResult::Ok;
    }
```

- [ ] **Step 6: Update the three `MainComponent` callers**

In `Source/MainComponent.cpp`:

Line 307 — replace:

```cpp
                            if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s))
```

with:

```cpp
                            if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s)
                                == SnapshotStore::LoadResult::Ok)
```

Line 331 — replace:

```cpp
            if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s))
                applySnapshot (s);
```

with:

```cpp
            if (SnapshotStore::load (SnapshotStore::getLastUsedFile(), s)
                == SnapshotStore::LoadResult::Ok)
                applySnapshot (s);
```

Lines 1881-1883 (in `loadSnapshotInteractive`) — replace:

```cpp
                Snapshot s;
                if (SnapshotStore::load (file, s))
                    applySnapshot (s);
```

with (the typed result lets us tell the user *why* an explicitly-chosen file failed):

```cpp
                Snapshot s;
                const auto res = SnapshotStore::load (file, s);
                if (res == SnapshotStore::LoadResult::Ok)
                    applySnapshot (s);
                else if (res != SnapshotStore::LoadResult::NoFile)
                    juce::NativeMessageBox::showMessageBoxAsync (
                        juce::MessageBoxIconType::WarningIcon,
                        "Could not load snapshot",
                        "\"" + file.getFileName() + "\" could not be loaded -- it may be "
                        "corrupt or saved by a newer version of D-Router.");
```

- [ ] **Step 7: Run the JUCE tests, then build the full app to catch caller breakage**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: PASS — all `SnapshotStore` cases green.

Run: `cmake --build build -j`
Expected: app builds clean (the 3 caller updates compile; no other `load()` callers exist).

- [ ] **Step 8: Format the changed files (one at a time)**

```bash
for f in Source/Persistence/SnapshotStore.h Source/Persistence/SnapshotStore.cpp \
         Source/MainComponent.cpp tests/juce/SnapshotStoreTests.cpp; do
  clang-format "$f" > /tmp/cf && mv /tmp/cf "$f"
done
```

- [ ] **Step 9: Commit**

```bash
git add Source/Persistence/SnapshotStore.h Source/Persistence/SnapshotStore.cpp \
        Source/MainComponent.cpp tests/juce/SnapshotStoreTests.cpp
git commit -m "fix(persistence): typed SnapshotStore::load result + honor version

load() now returns LoadResult {Ok,NoFile,ParseError,UnsupportedVersion,Corrupt}
and reads the schema version the writer has always stamped (rejecting newer
files) instead of ignoring it. Crash-recovery and the interactive loader can
now distinguish 'no snapshot yet' from 'corrupt/foreign file' -- the latter now
surfaces a message instead of silently doing nothing.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: SnapshotStore atomic save

**Files:**
- Modify: `Source/Persistence/SnapshotStore.cpp` (`save()` 398-406, add include)
- Test: `tests/juce/SnapshotStoreTests.cpp`

**Interfaces:**
- Consumes: `dcr::writeXmlAtomically` (Task 1), `SnapshotStore::LoadResult::Ok` (Task 3).
- Produces: no signature change.

- [ ] **Step 1: Write the failing test**

In `tests/juce/SnapshotStoreTests.cpp`, add at the end of `runTest()`:

```cpp
        beginTest ("save then overwrite leaves a loadable, current file (atomic)");
        {
            juce::TemporaryFile scratch;
            const juce::File file = scratch.getFile();

            Snapshot a;
            a.engineBlockSize = 128;
            expect (SnapshotStore::save (file, a));

            Snapshot b;
            b.engineBlockSize = 512;
            expect (SnapshotStore::save (file, b)); // overwrite

            Snapshot r;
            expect (SnapshotStore::load (file, r) == SnapshotStore::LoadResult::Ok);
            expectEquals (r.engineBlockSize, 512);

            auto leftovers = file.getParentDirectory()
                                 .findChildFiles (juce::File::findFiles, false, "*.temp");
            for (auto& f : leftovers)
                expect (! f.getFileName().startsWith (file.getFileName()));
        }
```

- [ ] **Step 2: Run the test to verify it passes for content but documents intent**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: PASS on the round-trip/overwrite assertions even before the change (current `save()` already round-trips); the test pins the behavior the atomic write must preserve. Proceed to make the write atomic.

- [ ] **Step 3: Switch `save()` to the atomic helper**

In `Source/Persistence/SnapshotStore.cpp`, add after the existing top `#include "Persistence/SnapshotStore.h"` (line 1):

```cpp
#include "Persistence/AtomicXmlWrite.h"
```

Replace `save()` (lines 398-406):

```cpp
    bool SnapshotStore::save (const juce::File& file, const Snapshot& s)
    {
        auto tree = toValueTree (s);
        auto xml = tree.createXml();
        if (xml == nullptr)
            return false;
        file.getParentDirectory().createDirectory();
        return xml->writeTo (file);
    }
```

with:

```cpp
    bool SnapshotStore::save (const juce::File& file, const Snapshot& s)
    {
        auto tree = toValueTree (s);
        auto xml = tree.createXml();
        if (xml == nullptr)
            return false;
        // Atomic: a crash mid-write must not shred last.xml -- the file
        // crash-recovery reads on the next launch.
        return writeXmlAtomically (*xml, file);
    }
```

- [ ] **Step 4: Build and run the test to verify it still passes**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: PASS — round-trip + overwrite + no-temp-leftover all green.

- [ ] **Step 5: Format the changed files (one at a time)**

```bash
for f in Source/Persistence/SnapshotStore.cpp tests/juce/SnapshotStoreTests.cpp; do
  clang-format "$f" > /tmp/cf && mv /tmp/cf "$f"
done
```

- [ ] **Step 6: Commit**

```bash
git add Source/Persistence/SnapshotStore.cpp tests/juce/SnapshotStoreTests.cpp
git commit -m "fix(persistence): write snapshots atomically

Route SnapshotStore::save through writeXmlAtomically so a crash mid-write
leaves the previous last.xml intact rather than truncating the very file
crash-recovery depends on.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: CrashHandler async-signal-safety

**Files:**
- Modify: `Source/Diagnostics/CrashHandler.mm` (install 139-164; `writeBacktraceToLog` 41-71; `uncaughtNSExceptionHandler` 108-135)
- Create: `tests/juce/CrashHandlerTests.cpp`
- Modify: `CMakeLists.txt` (add `CrashHandlerTests.cpp`, `CrashHandler.mm`, `Logger.cpp` + `-framework Foundation` to `dcorerouter_tests_juce`)

**Interfaces:**
- Consumes: `dcr::Logger::init()`, `dcr::Logger::getCurrentLogFile()`, `dcr::CrashHandler::install()`.
- Produces: no public signature change. New internal: a file-scope `std::atomic<int> gLogFd{-1}` pre-opened at `install()` time.

**Notes:** The fix is to do **zero** async-unsafe work in the signal path: `install()` (message thread) opens the log fd once; the handler only `write(2)`s + `backtrace_symbols_fd`s to that fd. `backtrace`/`backtrace_symbols_fd` and `write`/`fsync` are kept; the `Logger::getCurrentLogFile()` call, the `juce::String` path handling, and the `::open()` are removed *from the handler*. The NSException path is switched to the same pre-opened fd for consistency (it already runs in a safe context, but reusing the fd drops its `open()`/`close()`).

- [ ] **Step 1: Write the failing test (process-level fork)**

Create `tests/juce/CrashHandlerTests.cpp`:

```cpp
#include "Diagnostics/CrashHandler.h"
#include "Diagnostics/Logger.h"

#include <juce_core/juce_core.h>

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

// The crash handler runs in a signal handler, so we exercise it the only honest
// way: fork a child, raise a fatal signal in it, and confirm from the parent
// that (a) the child died from the re-raised signal (didn't hang) and (b) the
// pre-opened log fd received a backtrace.
struct CrashHandlerTests : juce::UnitTest
{
    CrashHandlerTests() : juce::UnitTest ("CrashHandler") {}

    void runTest() override
    {
        beginTest ("signal path writes a backtrace to the pre-opened log fd without hanging");

        dcr::Logger::init();
        dcr::CrashHandler::install();

        const juce::File logFile = dcr::Logger::getCurrentLogFile();
        expect (logFile.existsAsFile());

        const pid_t pid = ::fork();
        if (pid == 0)
        {
            // Child: trip the handler. SA_RESETHAND + re-raise terminates us.
            ::raise (SIGABRT);
            ::_exit (0); // unreachable if the handler re-raises
        }

        expect (pid > 0);
        int status = 0;
        ::waitpid (pid, &status, 0);
        expect (WIFSIGNALED (status)); // died from the signal, i.e. did not hang

        const juce::String contents = logFile.loadFileAsString();
        expect (contents.contains ("HARD CRASH"));
        expect (contents.contains ("SIGABRT"));

        // Don't leave this process's signal dispositions pointing at our handler
        // for the remaining tests.
        ::signal (SIGABRT, SIG_DFL);
        dcr::Logger::shutdown();
    }
};

static CrashHandlerTests crashHandlerTests;
```

- [ ] **Step 2: Wire the test + its deps into CMake**

In `CMakeLists.txt`, add to `target_sources(dcorerouter_tests_juce PRIVATE ...)`:

```cmake
    tests/juce/CrashHandlerTests.cpp
    Source/Diagnostics/CrashHandler.mm
    Source/Diagnostics/Logger.cpp
```

In the `target_link_libraries(dcorerouter_tests_juce PRIVATE ...)` block, add alongside the other `-framework` lines (after `"-framework CoreAudio"`):

```cmake
    "-framework Foundation"
```

- [ ] **Step 3: Run the test to verify it fails**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: PASS is plausible even now (current handler opens its own fd and writes the same markers). The test's real job is to **lock in** async-safety as we remove the in-handler `open()`/`Logger` calls in the next steps — it must stay green across the refactor. If it fails here, fix the test harness before refactoring.

- [ ] **Step 4: Add the pre-opened fd and open it in `install()`**

In `Source/Diagnostics/CrashHandler.mm`, in the anonymous namespace, replace the re-entry guard declaration (lines 22-24):

```cpp
// Re-entry guard -- if our handler itself crashes, just hand back to
// the default to avoid infinite recursion.
std::atomic<int> reentry{0};
```

with:

```cpp
// Re-entry guard -- if our handler itself crashes, just hand back to
// the default to avoid infinite recursion.
std::atomic<int> reentry{0};

// The log file descriptor, opened ONCE on the message thread at install()
// time.  The signal handler must never call open()/Logger/JUCE (none are
// async-signal-safe); it only write(2)s + backtrace_symbols_fd's to this fd.
std::atomic<int> gLogFd{-1};
```

- [ ] **Step 5: Make `writeBacktraceToLog` use only the pre-opened fd**

Replace `writeBacktraceToLog` (lines 41-71):

```cpp
void writeBacktraceToLog(const char* sigName)
{
    if (reentry.fetch_add(1, std::memory_order_acq_rel) != 0)
        return;

    // We snapshot the path on init since calling JUCE::File from a
    // signal handler is unsafe.  Logger::getCurrentLogFile() reads a
    // unique_ptr -- racy but in practice OK for this best-effort path.
    auto path = Logger::getCurrentLogFile().getFullPathName();
    if (path.isEmpty())
        return;

    int fd = ::open(path.toRawUTF8(), O_WRONLY | O_APPEND);
    if (fd < 0)
        return;

    writeAsyncSafe(fd, "\n");
    writeAsyncSafe(fd, "================================================================\n");
    writeAsyncSafe(fd, "=== HARD CRASH ");
    writeAsyncSafe(fd, sigName);
    writeAsyncSafe(fd, "\n");
    writeAsyncSafe(fd, "=== Backtrace:\n");

    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, fd);

    writeAsyncSafe(fd, "================================================================\n");
    ::fsync(fd);
    ::close(fd);
}
```

with:

```cpp
void writeBacktraceToLog(const char* sigName)
{
    if (reentry.fetch_add(1, std::memory_order_acq_rel) != 0)
        return;

    // Strict async-signal-safe path: the fd was opened at install() time, so
    // here we only write(2) + backtrace_symbols_fd (both async-signal-safe).
    // No Logger/JUCE/File, no open(), no allocation.
    const int fd = gLogFd.load(std::memory_order_acquire);
    if (fd < 0)
        return;

    writeAsyncSafe(fd, "\n");
    writeAsyncSafe(fd, "================================================================\n");
    writeAsyncSafe(fd, "=== HARD CRASH ");
    writeAsyncSafe(fd, sigName);
    writeAsyncSafe(fd, "\n");
    writeAsyncSafe(fd, "=== Backtrace:\n");

    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, fd);

    writeAsyncSafe(fd, "================================================================\n");
    ::fsync(fd);
    // Do NOT close: we're about to restore SIG_DFL and re-raise, so the process
    // is dying anyway and the fd needs no teardown.
}
```

- [ ] **Step 6: Switch the NSException handler to the pre-opened fd**

Replace the fd acquisition at the top of `uncaughtNSExceptionHandler` (lines 112-118):

```cpp
    auto path = Logger::getCurrentLogFile().getFullPathName();
    if (path.isEmpty())
        return;

    int fd = ::open(path.toRawUTF8(), O_WRONLY | O_APPEND);
    if (fd < 0)
        return;
```

with:

```cpp
    // Reuse the install()-time fd for consistency with the signal path.
    const int fd = gLogFd.load(std::memory_order_acquire);
    if (fd < 0)
        return;
```

Then, near the end of the same function, replace the closing two lines (lines 133-134):

```cpp
    ::fsync(fd);
    ::close(fd);
```

with:

```cpp
    ::fsync(fd);
    // fd is the shared pre-opened log handle -- leave it open.
```

- [ ] **Step 7: Open the fd in `install()`**

In `CrashHandler::install()`, immediately before the `#if JUCE_MAC` block that sets the NSException handler (line 159), add:

```cpp
    // Pre-open the log fd on the message thread so the signal handler never has
    // to.  Logger::init() runs before install() (see header), so the file exists.
    {
        const auto path = Logger::getCurrentLogFile().getFullPathName();
        if (path.isNotEmpty())
        {
            const int fd = ::open(path.toRawUTF8(), O_WRONLY | O_APPEND | O_CREAT, 0644);
            gLogFd.store(fd, std::memory_order_release);
        }
    }

```

- [ ] **Step 8: Build and run the test to verify it stays green**

Run: `cmake --build build --target dcorerouter_tests_juce -j && ctest --test-dir build -R dcorerouter_tests_juce --output-on-failure`
Expected: PASS — `CrashHandler` test still writes "HARD CRASH"/"SIGABRT" and the child is `WIFSIGNALED` (no hang), now via the async-safe path.

- [ ] **Step 9: Build the full app (CrashHandler.mm is also in the app target)**

Run: `cmake --build build -j`
Expected: app builds clean.

- [ ] **Step 10: Format the changed files (one at a time)**

```bash
for f in Source/Diagnostics/CrashHandler.mm tests/juce/CrashHandlerTests.cpp; do
  clang-format "$f" > /tmp/cf && mv /tmp/cf "$f"
done
```

- [ ] **Step 11: Commit**

```bash
git add Source/Diagnostics/CrashHandler.mm tests/juce/CrashHandlerTests.cpp CMakeLists.txt
git commit -m "fix(diagnostics): make the crash signal path async-signal-safe

The handler called Logger::getCurrentLogFile() and open() inside the signal
handler -- neither is async-signal-safe, so a crash inside malloc could
deadlock the handler. Pre-open the log fd at install() time and have the
handler only write(2)+backtrace_symbols_fd to it. Adds a fork-based test that
confirms a raised SIGABRT writes a backtrace and terminates (no hang).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Async lifetime-guard audit

**Files:**
- Modify: `Source/MainComponent.cpp` (sites 291 and 1226)

**Interfaces:**
- Consumes: the existing `aliveToken` member (`std::shared_ptr<std::atomic<bool>>`, MainComponent.h:169), flipped to `false` first thing in teardown (MainComponent.cpp:393), and the established guard idiom already used at lines 1662 and 1736.
- Produces: nothing new.

**Notes:** This is an audit, not a feature. A destruction-during-async race is not deterministically reproducible in a unit test, so verification is: the documented audit table below, a clean build, and the full existing test suites staying green. The audit found exactly two `juce::MessageManager::callAsync` sites that capture a bare `this` without the guard; the plugin-restore callbacks (1662/1736) already guard, and `OutputGroupPanel`'s editor-close callback uses a `juce::Component::SafePointer` (safe). Apply the *existing* pattern; do not invent a new mechanism (that's Phase C's structural cleanup).

Audit result:

| Site | What it is | Status before | Action |
|---|---|---|---|
| MainComponent.cpp:291 | crash-recovery dialog `callAsync`, then `showAsync` result | bare `this`, unguarded | add guard at both lambda entries |
| MainComponent.cpp:1226 | reconfigure-completion `callAsync` from the worker thread | bare `this`, unguarded | add guard at lambda entry |
| MainComponent.cpp:1662/1736 | plugin-restore queue callbacks | already capture+check `alive` | none |
| OutputGroupPanel.cpp:364 | editor-close `callAsync` | `SafePointer<Card>` | none |

- [ ] **Step 1: Guard the crash-recovery dialog (site 291)**

In `Source/MainComponent.cpp`, replace the `callAsync` opening (line 291):

```cpp
            juce::MessageManager::callAsync ([this] {
```

with (capture the alive token and bail if we've since been destroyed):

```cpp
            auto alive = aliveToken;
            juce::MessageManager::callAsync ([this, alive] {
                if (! alive->load (std::memory_order_acquire))
                    return;
```

Then, in the same block, replace the inner `showAsync` result lambda opening (line 302):

```cpp
                    [this] (int result) {
```

with:

```cpp
                    [this, alive] (int result) {
                        if (! alive->load (std::memory_order_acquire))
                            return;
```

(The dialog can outlive the component if the user quits with it open; both the
deferred presentation and the result callback now no-op on a dead `this`.)

- [ ] **Step 2: Guard the reconfigure-completion callback (site 1226)**

In `Source/MainComponent.cpp`, replace the `callAsync` opening (lines 1226-1227):

```cpp
            juce::MessageManager::callAsync (
                [this, specs, preserved, started, preserveChains, chainsToRestore = std::move (harvestedChains)]() mutable {
```

with (the worker may dispatch this after teardown has flipped the token):

```cpp
            auto alive = aliveToken;
            juce::MessageManager::callAsync (
                [this, alive, specs, preserved, started, preserveChains, chainsToRestore = std::move (harvestedChains)]() mutable {
                    if (! alive->load (std::memory_order_acquire))
                        return;
```

- [ ] **Step 3: Build the full app**

Run: `cmake --build build -j`
Expected: app builds clean.

- [ ] **Step 4: Run both test suites to confirm no regression**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS — both `dcorerouter_tests` and `dcorerouter_tests_juce` green.

- [ ] **Step 5: Format the changed file**

```bash
clang-format Source/MainComponent.cpp > /tmp/cf && mv /tmp/cf Source/MainComponent.cpp
```

- [ ] **Step 6: Commit**

```bash
git add Source/MainComponent.cpp
git commit -m "fix(ui): guard the two unguarded async callbacks with aliveToken

The crash-recovery dialog (callAsync + showAsync result) and the
reconfigure-completion callback captured a bare this and could fire after the
component began teardown. Apply the existing aliveToken guard used by the
plugin-restore callbacks so a destroyed MainComponent is a no-op, not a UAF.
Structural cleanup of this pattern is Phase C.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Final verification (after all tasks)

- [ ] **Full build:** `cmake --build build -j` — app + both test targets compile.
- [ ] **All tests:** `ctest --test-dir build --output-on-failure` — `dcorerouter_tests` and `dcorerouter_tests_juce` both pass.
- [ ] **Format check (per-file, mirrors CI):**
  ```bash
  for f in $(git diff --name-only main...HEAD | grep -E '\.(h|cpp|mm)$'); do
    diff <(clang-format "$f") "$f" >/dev/null || echo "NEEDS FORMAT: $f"
  done
  ```
  Expected: no output.
- [ ] **Manual crash verification (not unit-testable in-process):** launch the app, induce a hard crash (e.g. a known-bad plugin), confirm `~/Library/Logs/D-Router/session-*.log` gets a `HARD CRASH` + backtrace and the process exits rather than hanging.
- [ ] **Manual kill-during-save (atomicity):** confidence comes from the helper design (same-dir TemporaryFile + atomic rename); optionally verify last.xml stays loadable after a forced quit during a save.
- [ ] **Push to the fork (NOT origin):** `git push fork overhaul/phase-b-fragiles`
- [ ] **Update memory** `drouter-overhaul-status.md`: mark Phase B done, list the commits, note what was verified vs. left for manual testing.

## Self-Review notes

- **Spec coverage:** atomic writes (Tasks 1,2,4) · SnapshotStore version + typed LoadResult (Task 3) · SettingsStore version + 4 dropped fields (Task 2) · async-signal-safe CrashHandler (Task 5) · async lifetime-guard audit (Task 6). All Phase B targets mapped.
- **DSP untouched:** confirmed — only Persistence, Diagnostics, and message-thread UI callbacks.
- **Type consistency:** `LoadResult` enum + `kVersion` defined in Task 3 header; all callers and tests reference `SnapshotStore::LoadResult::Ok`/`kVersion` consistently. `writeXmlAtomically(const juce::XmlElement&, const juce::File&)` defined Task 1, called identically in Tasks 2 and 4.
- **Known non-unit-testable items** (CrashHandler beyond the fork smoke test; lifetime races in Task 6) are called out honestly rather than faked with green-looking assertions.

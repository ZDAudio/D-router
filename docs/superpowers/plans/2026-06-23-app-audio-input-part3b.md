# App Audio Input Implementation Plan — Part 3b: live wiring + first capture

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make app-audio capture actually work in the running app: `MainComponent`
owns the process watcher, drives attach/detach by reconciling the engine's app
workers against the running processes, threads the app-input list through the
reconfigure path, and exposes a minimal "Add App Audio Input" entry — enough to
route a captured app to an output and hear it.

**Architecture:** `MainComponent` owns an `AppAudioProcesses` watcher and a
`currentAppInputs` list (+ a parallel `appAttachedPids`). A reconcile step runs on
the existing message-thread timer (and on the watcher's launch/quit callback),
**gated by `reconfig.active()`** so it never races an engine rebuild — both run on
the single message thread, so the gate is sufficient. Adding/removing an app source
updates `currentAppInputs` and re-runs `applyDeviceSelection` (the existing
reconfigure), which now passes `currentAppInputs` to `engine.start(specs, appInputs)`.

**Tech Stack:** JUCE (MainComponent, Timer, PopupMenu), the Part-1 resolver
(`dcr::appinput::reconcile`), the Part-2 watcher (`AppAudioProcesses`), the Part-3a
engine API (`getAppWorker`/`getNumAppWorkers`/`getAppInputSpecs`).

---

## Scope & verification reality

Part 3b is the **first real-device capture milestone**. What I (the implementer) can
verify: the app **builds**, **launches without crashing**, and the menu entries
appear. What only the **user on a real machine** can verify (state honestly, do NOT
claim): the TCC prompt on first tap, that a captured app's audio actually flows and
routes to an output, mute behavior, and auto-reattach across quit/relaunch. Reason:
TCC keys on the signed app + needs GUI consent, and "do I hear it" needs ears.

Deferred to **Part 3c**: Soft-In groups, Snapshot persistence of `appInputs`, a
polished per-source list UI (status dots, mute toggles) in `DeviceManagerDialog`,
matrix labeling, TCC-denial UX, and the macOS 14.4 availability gate (3b assumes
14.4+; the gate/disable-below-14.4 lands in 3c).

Integration facts (verified by reading the code):
- `MainComponent` is a `juce::Timer`; owns `engine`, `currentSpecs` (MainComponent.h:140),
  `reconfig` (ReconfigurationController), `perfMonitor`.
- `applyDeviceSelection` (MainComponent.cpp ~1079): `reconfig.tryBegin()` → freeze-set
  (panels + `stopTimer()` @1143 + `perfMonitor.pause()` @1150) → worker thread (fade,
  stopProcessor, `engine.stop()`, `engine.start(specs)` @1204) → `callAsync` (restore,
  resume panels, `perfMonitor.resume()` @1282, `startTimer` @1287, `reconfig.finish()` @1297;
  `currentSpecs = specs` @1213).
- `timerCallback` (MainComponent.cpp ~409): `refreshStatus()` + `replanPdc()`.
- `reconfig.active()` (ReconfigurationController.h:59) → true while any phase != Idle.
- Device dialog: `DeviceManagerDialog::launch(engine, current, callback)` from
  `openDeviceDialog()` (MainComponent.cpp:1070); File menu "Devices..." (miDevices).
- Edit-menu enum block at MainComponent.cpp ~525 (miPanic=1100, miReset, miTogglePdc).

## File structure (Part 3b)

- **Modify** `Source/MainComponent.h` — forward-declare `AppAudioProcesses`; add
  members (`appAudioProcesses`, `currentAppInputs`, `appAttachedPids`); declare
  `reconcileAppAudioAttachments()`, `openAppInputMenu()`, `addAppInput(...)`,
  `clearAppInputs()`.
- **Modify** `Source/MainComponent.cpp` — include the new headers; construct the
  watcher; thread `currentAppInputs` through `applyDeviceSelection`; the reconcile
  method; the timer hook; the menu entries.

No new files. (The minimal v1 UI is menu-driven; no new dialog component.)

---

### Task 1: Own the watcher + thread appInputs through the reconfigure

**Files:**
- Modify: `Source/MainComponent.h`, `Source/MainComponent.cpp`

- [ ] **Step 1: Header — forward decl, includes, members, method decls**

In `Source/MainComponent.h`, forward-declare near the other engine types and add the
members after `currentSpecs` (line 140):

```cpp
class AppAudioProcesses; // fwd (Source/Engine/AppAudioProcesses.h)
// ...
    std::vector<AudioEngine::DeviceSpec> currentSpecs;
    // App-audio capture: the desired source list, the watcher, and the per-source
    // currently-attached process id (parallel to currentAppInputs; 0 == detached).
    std::vector<AudioEngine::AppInputSpec> currentAppInputs;
    std::unique_ptr<AppAudioProcesses> appAudioProcesses;
    std::vector<int> appAttachedPids;
```

Add the private method declarations (near the other private methods, e.g. by
`timerCallback`):

```cpp
    void reconcileAppAudioAttachments();
    void openAppInputMenu();
    void addAppInput (const juce::String& bundleId, const juce::String& displayName);
    void clearAppInputs();
```

- [ ] **Step 2: .cpp — includes + construct the watcher**

In `Source/MainComponent.cpp`, add includes near the top:

```cpp
#include "Engine/AppAudioProcesses.h"
#include "Engine/AppInputResolver.h"
```

In the `MainComponent` constructor (after `engine` is usable), create the watcher and
point its change callback at the reconcile (it fires on the main thread):

```cpp
    appAudioProcesses = std::make_unique<AppAudioProcesses>();
    appAudioProcesses->onProcessesChanged = [this] { reconcileAppAudioAttachments(); };
```

If `MainComponent`'s destructor is defaulted in the header, move it to the .cpp
(`MainComponent::~MainComponent() = default;`) so the `unique_ptr<AppAudioProcesses>`
member can destruct with the type complete.

- [ ] **Step 3: Thread `currentAppInputs` into the reconfigure worker thread**

In `applyDeviceSelection`, the worker-thread lambda currently captures
`[this, specs, preserved, preserveChains]` and calls (line ~1204)
`engine.start (specs)`. Capture a copy of the app-input list and pass it:

```cpp
    reconfigThread = std::thread ([this, specs, preserved, preserveChains,
                                      appInputs = currentAppInputs] {
        // ...unchanged...
        const bool started = specs.empty() ? true : engine.start (specs, appInputs);
        // ...unchanged...
    });
```

This preserves app inputs across a device-only reconfigure (currentAppInputs
unchanged) and applies new ones when an app-source change triggers the reconfigure.

- [ ] **Step 4: Reset `appAttachedPids` after the rebuild**

App workers come back **detached** from `engine.start`, so the tracked attach state
must reset. In the `callAsync` message-thread callback (after the engine has
restarted, near `currentSpecs = specs` @1213), add:

```cpp
        appAttachedPids.assign ((size_t) engine.getNumAppWorkers(), 0);
```

- [ ] **Step 5: Build**

Run: `cmake --build build -j`
Expected: builds clean (watcher owned, appInputs threaded; no reconcile/UI yet, so no
app sources exist at runtime and behavior is unchanged).

- [ ] **Step 6: Commit**

```bash
git add Source/MainComponent.h Source/MainComponent.cpp
git commit -m "feat(appinput): MainComponent owns the app-audio watcher; appInputs threaded through reconfigure

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: Reconcile attach/detach on the timer (and on launch/quit)

**Files:**
- Modify: `Source/MainComponent.cpp`

- [ ] **Step 1: Implement `reconcileAppAudioAttachments()`**

Gated by `reconfig.active()` (both this and the reconfigure run on the message
thread, so the gate alone is race-free). Build the resolver inputs from the engine's
app specs + the tracked attach pids and the live process set, then apply commands.

```cpp
void MainComponent::reconcileAppAudioAttachments()
{
    if (reconfig.active() || appAudioProcesses == nullptr)
        return;
    const int n = engine.getNumAppWorkers();
    if (n == 0)
        return;
    if ((int) appAttachedPids.size() != n)
        appAttachedPids.assign ((size_t) n, 0);

    // Currently-running tappable processes (bundle id -> process-object id).
    std::vector<dcr::appinput::RunningProcess> running;
    for (const auto& e : appAudioProcesses->enumerate())
        running.push_back ({ e.bundleId, (int) e.processObject });

    // The configured sources with their current attach state.
    const auto& specs = engine.getAppInputSpecs();
    std::vector<dcr::appinput::Source> sources;
    for (int i = 0; i < n; ++i)
        sources.push_back ({ specs[(size_t) i].bundleId.toStdString(), appAttachedPids[(size_t) i] });

    for (const auto& cmd : dcr::appinput::reconcile (sources, running))
    {
        auto* w = engine.getAppWorker (cmd.sourceIndex);
        if (w == nullptr)
            continue;
        if (cmd.type == dcr::appinput::CommandType::Attach)
        {
            if (w->attach ((AudioObjectID) cmd.processId))
                appAttachedPids[(size_t) cmd.sourceIndex] = cmd.processId;
        }
        else
        {
            w->detach();
            appAttachedPids[(size_t) cmd.sourceIndex] = 0;
        }
    }
}
```

- [ ] **Step 2: Call it from the timer**

In `timerCallback()` (after the PDC backstop), add:

```cpp
    reconcileAppAudioAttachments(); // self-gates on reconfig.active()
```

(The watcher's `onProcessesChanged` already calls it directly for prompt
attach/detach on app launch/quit; the timer is the steady backstop. Both self-gate.)

- [ ] **Step 3: Build**

Run: `cmake --build build -j`
Expected: builds clean. (Still no way to add a source, so `n == 0` and reconcile is a
no-op at runtime.)

- [ ] **Step 4: Commit**

```bash
git add Source/MainComponent.cpp
git commit -m "feat(appinput): reconcile app-tap attach/detach on the message-thread timer (freeze-set gated)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: Minimal "Add App Audio Input" menu entry

**Files:**
- Modify: `Source/MainComponent.cpp` (menu enum, menu build, menu handler, methods)

- [ ] **Step 1: Add menu ids**

In the menu-id enum (~MainComponent.cpp:525, the Edit group near `miTogglePdc`), add:

```cpp
        miAddAppInput = 1105,
        miClearAppInputs,
```

- [ ] **Step 2: Add the Edit-menu items**

In the Edit-menu build (`case 1:` of `getMenuForIndex`), add (enabled only when the
watcher exists):

```cpp
        m.addSeparator();
        m.addItem (miAddAppInput, "Add App Audio Input...", appAudioProcesses != nullptr, false);
        m.addItem (miClearAppInputs, "Clear App Audio Inputs", ! currentAppInputs.empty(), false);
```

- [ ] **Step 3: Handle the menu ids**

In `menuItemSelected` (the switch on menu id), add:

```cpp
        case miAddAppInput: openAppInputMenu(); break;
        case miClearAppInputs: clearAppInputs(); break;
```

- [ ] **Step 4: Implement the three methods**

```cpp
void MainComponent::openAppInputMenu()
{
    if (appAudioProcesses == nullptr)
        return;

    // Build a picker of apps currently producing output (fall back to any with a
    // bundle id).  De-dupe by bundle id so helper processes don't flood the list.
    auto entries = appAudioProcesses->enumerate();
    juce::PopupMenu menu;
    std::vector<std::pair<juce::String, juce::String>> items; // bundleId, displayName
    juce::StringArray seen;
    for (const auto& e : entries)
    {
        if (! e.runningOutput)
            continue;
        juce::String bid (e.bundleId.c_str());
        if (bid.isEmpty() || seen.contains (bid))
            continue;
        seen.add (bid);
        juce::String name (e.displayName.empty() ? e.bundleId.c_str() : e.displayName.c_str());
        items.push_back ({ bid, name });
        menu.addItem ((int) items.size(), name);
    }
    if (items.empty())
        menu.addItem (-1, "(no app is currently producing audio)", false, false);

    menu.showMenuAsync (juce::PopupMenu::Options{}, [this, items] (int choice) {
        if (choice >= 1 && choice <= (int) items.size())
            addAppInput (items[(size_t) (choice - 1)].first, items[(size_t) (choice - 1)].second);
    });
}

void MainComponent::addAppInput (const juce::String& bundleId, const juce::String& displayName)
{
    // Ignore duplicates (one capture per app for v1).
    for (const auto& s : currentAppInputs)
        if (s.bundleId == bundleId)
            return;

    AudioEngine::AppInputSpec spec;
    spec.bundleId = bundleId;
    spec.displayName = displayName;
    currentAppInputs.push_back (spec);
    applyDeviceSelection (currentSpecs); // reconfigure with the existing devices + new app inputs
}

void MainComponent::clearAppInputs()
{
    if (currentAppInputs.empty())
        return;
    currentAppInputs.clear();
    applyDeviceSelection (currentSpecs);
}
```

- [ ] **Step 5: Build + launch sanity**

Run: `cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: builds; headless tests still `0 failures`.
Then: `open build/dcorerouter_artefacts/Release/D-Router.app` and confirm it launches
and the Edit menu shows "Add App Audio Input..." (I can verify launch + menu presence;
NOT capture).

- [ ] **Step 6: Commit**

```bash
git add Source/MainComponent.cpp
git commit -m "feat(appinput): minimal Add/Clear App Audio Input menu entries (process picker)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Real-device verification (USER — I cannot do this)

After Part 3b builds, the user verifies on a real macOS 14.4+ machine. Record results
honestly:
- [ ] Edit ▸ "Add App Audio Input..." lists apps currently playing audio.
- [ ] Picking one triggers the first-time `kTCCServiceAudioCapture` prompt; granting it sticks.
- [ ] The app appears as a new stereo input pair in the matrix; routing it to an output is audible and in sync.
- [ ] Default mute behavior: the captured app goes silent on its normal output while routed through D-Router.
- [ ] Quit the app and relaunch it: capture auto-reattaches (within a timer tick) with no glitch on other audio.
- [ ] "Clear App Audio Inputs" removes it cleanly (engine restarts, no crash).

## Self-review

**Spec coverage (3b):** §8 watcher ownership + §9 reconcile wiring → Tasks 1–2; §6/§7
live attach/detach via the engine API → Task 2; minimal source management UI → Task 3.
Deferred to 3c (with mapping): §11 Soft-In groups, §15 persistence, §12 polished UI,
§13 multichannel (already fixed stereo), §14 TCC-denial UX + 14.4 gate.

**Placeholder scan:** none — methods are complete; edit points cite verified line
numbers. The real-device checklist is explicitly the user's, not a code placeholder.

**Type consistency:** `currentAppInputs` is `std::vector<AudioEngine::AppInputSpec>`
(matches the Part-3a engine type). `reconcile`/`Source`/`RunningProcess`/`Command`/
`CommandType` match Part-1 `AppInputResolver.h`. `getAppWorker`/`getNumAppWorkers`/
`getAppInputSpecs` match the Part-3a `AudioEngine` API. `attach(AudioObjectID)`/
`detach()` match Part-2 `AppAudioWorker`. `reconfig.active()` matches
`ReconfigurationController.h:59`.

## Next

Part 3c: Soft-In groups (`OutputGroup::Kind` + auto-create over each app source's
channels), Snapshot persistence (`appInputs` + `Group.kind`), polished per-source UI
(status/mute/remove in `DeviceManagerDialog`), matrix labeling, TCC-denial UX, and the
macOS 14.4 availability gate.

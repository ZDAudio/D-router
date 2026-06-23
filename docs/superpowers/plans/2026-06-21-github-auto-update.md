# GitHub Auto-Update — Implementation Plan

> Local plan (not committed to the public repo). Spec:
> `docs/superpowers/specs/2026-06-21-github-auto-update-design.md`.

**Goal:** Opt-in macOS self-update — check `ZDAudio/D-router` GitHub releases (incl.
pre-releases), prompt non-blockingly, and on user click download + swap the running
`.app` + relaunch.

**Architecture:** A JUCE-free `Version` comparator (unit-tested); an `UpdateChecker`
(background thread → GitHub API → message-thread callback); an `UpdateInstaller`
(background download → `ditto` unzip → detached swap script → quit); an `UpdatePrompt`
DialogWindow; wired into `MainComponent` (menu item + launch timer). Version source
unified to CMake. Nothing touches the audio/matrix thread.

**Tech stack:** C++20, JUCE 8 (`juce::URL`/`WebInputStream`, `juce::JSON`,
`juce::ChildProcess`, `DialogWindow`), a bash swap script, ctest.

---

## File map

- **Create** `Source/Update/Version.h` — JUCE-free parse/compare. (header; used by tests + app)
- **Create** `Source/Update/UpdateChecker.h` / `.cpp` — fetch/parse/compare. (.cpp → CMake)
- **Create** `Source/Update/UpdateInstaller.h` / `.cpp` — download/unzip/swap/quit. (.cpp → CMake)
- **Create** `Source/Update/UpdatePrompt.h` — DialogWindow content component. (header)
- **Modify** `Source/Main.cpp` — `getApplicationVersion()` → `JUCE_APPLICATION_VERSION_STRING`.
- **Modify** `Source/MainComponent.h` / `.cpp` — menu item, launch timer, `checkForUpdates()`, prompt.
- **Modify** `tests/test_main.cpp` — Version tests.
- **Modify** `CMakeLists.txt` — add the two new `.cpp` to `target_sources(dcorerouter ...)`.

Repo for the API: `ZDAudio/D-router` (exact remote casing). Asset name: `D-Router.zip`.

---

## Task 1 — `Version.h` + tests (TDD; JUCE-free)

`parseVersion("v1.2.3-beta")` → `{1,2,3,"beta",valid}`; `compareVersions` (numeric x.y.z;
stable > prerelease at equal x.y.z; else prerelease string compare); `isNewer(cand,cur)`
strict-greater, **false if either fails to parse** (no false prompts).

Tests in `test_main.cpp` (`#include "Update/Version.h"`, register `test_update_version_compare`):
`0.2.0>0.1.0`, `v`-prefix, `1.10.0>1.9.0` (numeric not lexical), `0.2.0>0.2.0-beta`,
`!(0.2.0-beta>0.2.0)`, `0.2.0-beta.2>0.2.0-beta.1`, equal→false, malformed→false, parse fields.

Verify: `cmake --build build --target dcorerouter_tests && ctest --test-dir build`. Commit.

## Task 2 — version single source

`Source/Main.cpp:48` → `return JUCE_APPLICATION_VERSION_STRING;` (defined for the app target
in CMakeLists; equals CMake `project VERSION`). Build app. Commit.

## Task 3 — `UpdateChecker`

Background `juce::Thread`. `check(currentVersion, Callback)` where
`Callback = std::function<void(std::unique_ptr<ReleaseInfo>, bool ok)>` fired via
`MessageManager::callAsync`. `ReleaseInfo { String tag,name,notes; URL zipUrl; int64 zipSize; }`.
`run()`: GET `https://api.github.com/repos/ZDAudio/D-router/releases?per_page=20`,
headers `User-Agent: D-Router-Updater` + `Accept: application/vnd.github+json`, 10s timeout;
`JSON::parse` → array; first non-draft release with a `D-Router.zip` asset; if
`isNewer(tag,current)` → callback(info,true) else callback(nullptr,true); hard failure →
callback(nullptr,false). **unique_ptr cannot be captured in std::function** → `info.release()`
to a raw ptr, re-wrap inside the lambda. Add `UpdateChecker.cpp` to CMake. Build. Commit.

## Task 4 — `UpdatePrompt.h`

`Component` shown via `DialogWindow::LaunchOptions::launchAsync` (mirror
`MainComponent::showAboutDialog`). Shows current→new version + notes (read-only editor,
scrollable) + `[Upgrade]` `[Later]`. On Upgrade: `UpdateInstaller::canInstallInPlace(reason)`
— if false, show `reason` + `[Open Download Page]` (opens releases URL) ; else swap UI to a
`ProgressBar` + label + `[Cancel]`, owns an `UpdateInstaller`, calls `start()`. Done(ok=false,
err) → show err + restore buttons; success → app quits itself.

## Task 5 — `UpdateInstaller` (download + verify + unzip)

Background `juce::Thread`. `start(ReleaseInfo, Progress, Done)`, `cancel()`.
`run()`: download `zipUrl` → `~/Library/Caches/D-Router/update/D-Router.zip` (64 KiB chunks,
progress via callAsync, honor cancel/exit); verify `got == zipSize`; `ditto -x -k zip staged/`
(`juce::ChildProcess`, 60s); assert `staged/D-Router.app` exists. Add `UpdateInstaller.cpp` to
CMake. Build. Commit.

## Task 6 — swap script + quit/relaunch + edge guards

`static bool canInstallInPlace(String& reason)`: false if bundle path contains
`/AppTranslocation/` or parent `!hasWriteAccess()` (with a helpful reason). `appBundle()` =
`File::getSpecialLocation(currentApplicationFile)`. Build the script (paths shell-quoted via a
`'`-escaping helper, `getpid()` from `<unistd.h>`):

```bash
#!/bin/bash
PID=<pid>; OLD=<q>; NEW=<q staged/.app>; STAGING=<q>; ZIP=<q>; SELF="$0"
while kill -0 "$PID" 2>/dev/null; do sleep 0.2; done
sleep 0.3
xattr -cr "$NEW" 2>/dev/null
if ditto "$NEW" "$OLD.new"; then rm -rf "$OLD" && mv "$OLD.new" "$OLD" && xattr -cr "$OLD" 2>/dev/null; fi
open "$OLD"
rm -rf "$STAGING" "$ZIP" "$SELF"
```

`setExecutePermission(true)`; launch detached:
`std::system("nohup /bin/bash " + shQuote(script) + " >/dev/null 2>&1 &")`; then
`MessageManager::callAsync` → `JUCEApplicationBase::getInstance()->systemRequestedQuit()`
(real quit, runs shutdown → releases audio). Build. Commit.

## Task 7 — wire into `MainComponent`

`MainComponent.h`: `#include "Update/UpdateChecker.h"`; members `dcr::update::UpdateChecker updateChecker;`
+ `void checkForUpdates(bool userInitiated); void showUpdatePrompt(std::unique_ptr<...ReleaseInfo>);`.
`.cpp`: file-scope `kMenuCheckUpdatesId = 1501`; in the Apple menu add
`appleMenu.addItem(kMenuCheckUpdatesId, "Check for Updates...")` after About;
`menuItemSelected` → `kMenuCheckUpdatesId: checkForUpdates(true)`. `checkForUpdates`:
`updateChecker.check(JUCEApplication::getInstance()->getApplicationVersion(), cb)`; cb shows the
prompt if info, else (only if userInitiated) an async "You're up to date" / "Couldn't check"
AlertWindow. Launch check: in the ctor, `Timer::callAfterDelay(3000, [safe=Component::SafePointer<MainComponent>(this)]{ if(safe) safe->checkForUpdates(false); })`.
Build. Commit.

## Task 8 — full build + verification

`cmake --build build -j` (clean), `ctest` green. Manual (real machine): point at a test release,
confirm check→prompt→download→swap→relaunch; confirm ignore-keeps-old-version; confirm the
not-writable / translocation downgrade path. Document what's verified vs. needs the user.

---

## Self-review

- **Spec coverage:** trigger (T7 timer + menu) ✓; pre-release-inclusive newest (T3) ✓; non-forced
  prompt (T4) ✓; download+progress (T5) ✓; swap+relaunch+edges (T6) ✓; threading/no-RT (T3/T5
  threads) ✓; version single source (T2) ✓; D-Router.zip asset dependency (T3) ✓; Version tests
  (T1) ✓; YAGNI items absent ✓.
- **Placeholders:** swap-script `<pid>/<q>` are runtime-filled values, described in T6 — not gaps.
- **Type consistency:** `ReleaseInfo` defined in `UpdateChecker.h`, reused by installer/prompt;
  `isNewer/parseVersion/compareVersion` names consistent T1↔T3.

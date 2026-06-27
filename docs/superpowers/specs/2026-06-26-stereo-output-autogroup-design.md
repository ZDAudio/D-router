# Stereo-output auto-group + Settings Apply/Save merge — Design

**Date:** 2026-06-26  **Status:** approved (design), pending spec review

Two small, independent changes batched in one session (separate commits/PRs).

---

## Feature 1 — Auto output-group for stereo output devices

**Goal:** When a **stereo (exactly 2-channel) output device** is part of the engine, automatically present it as an output **group** (per-device fader + insert slot) — mirroring how a Soft-In app source auto-creates an input group today.

**Pattern mirrored:** [AudioEngine.cpp:293-310](../../../Source/Engine/AudioEngine.cpp) rebuilds Soft-In input groups on every engine start: `inputGroupManager.removeSoftInGroups()` → per app source `createGroup(name, channelSet)` → tag `Kind::SoftIn` → `assignChannel`. Derived, not persisted.

### Behavior
- **Scope:** output **devices** with exactly **2** output channels. Mono / multichannel output devices are skipped.
- **Trigger / lifecycle:** rebuilt on every engine start (in `AudioEngine::start`, an output-side pass right after the Soft-In input pass). The group is **derived** — not persisted (rebuilt from the persisted device list), so no orphan groups.
- **Respect manual groups (key rule):** `OutputGroupManager::assignChannel` auto-removes a channel from any prior group, so a channel lives in exactly one group. Therefore: **skip** the auto-group for a device whose output channels are **already in a (remaining) group** after the auto-groups are cleared — never steal channels from a user's manual group. (Rejected alternative: always override — it would destroy manual grouping on every restart.)
- **Audio-neutral:** created **VCA, 0 dB** (unity). No sound change until the user moves the fader or adds an insert.
- **Always on** (no toggle / setting) — matches "默认".

### Mechanism
1. `OutputGroup::Kind` gains a value `DeviceAuto` (alongside `Regular`, `SoftIn`).
2. `OutputGroupManager::removeAutoGroups()` — new, mirror of `InputGroupManager::removeSoftInGroups()`: removes only `Kind::DeviceAuto` groups, leaving user groups intact.
3. In `AudioEngine::start`, after the Soft-In pass and **before** the group-plugin re-prepare loop ([AudioEngine.cpp:312](../../../Source/Engine/AudioEngine.cpp)):
   - `groupManager.removeAutoGroups();`
   - for each `DeviceInfo` that is an **output** device (not app input) with `numOutputChannels == 2`:
     - if `groupManager.getGroupIndexForChannel(globalOutputBase) >= 0` **or** `...(globalOutputBase+1) >= 0` → **skip** (already grouped).
     - else `gi = createGroup(deviceName, AudioChannelSet::stereo())`, set `kind = DeviceAuto`, `assignChannel(gi, 0/1, base/base+1)`.
4. **Persistence:** the snapshot gather skips `Kind::SoftIn` ([MainComponent.cpp:1593, 1645](../../../Source/MainComponent.cpp)); add `Kind::DeviceAuto` to those same skips so auto output-groups are never written to disk.
5. **Appearance:** `OutputGroupPanel` ([:181-187](../../../Source/UI/OutputGroupPanel.cpp)) special-cases `SoftIn` (amber `FFB020`, "SOFT IN " prefix). Add a `DeviceAuto` case: a **distinct accent color** (proposed: cyan-blue `5AAAFF`) and a prefix (proposed: `"OUT  "` + device name) so it reads as auto-created, parallel to Soft-In. *(Color/prefix adjustable.)*

### Edge cases
- A stereo device the user already grouped manually → auto-group skipped (rule above); the user's group stands.
- Device removed → its `DeviceAuto` group is gone next start (rebuilt from the live device list).
- The built-in speakers (2 ch) get an auto-group too — intended (per-device fader).

### Testing
- **Unit (pure):** if the skip decision is extractable JUCE-free, add a case; otherwise the `removeAutoGroups` / kind handling is covered by real-device + the existing snapshot round-trip (assert `DeviceAuto` groups don't serialize).
- **Real-device:** add a stereo output → an "OUT <name>" group appears (VCA, unity); manually group a device's channels → no auto-group steals them; restart → no duplicate/orphan groups; snapshot has no auto-group.

---

## Feature 2 — Settings: merge Apply + Save (unrelated; separate commit/PR)

**Current** ([SettingsDialog.cpp:152-167](../../../Source/UI/SettingsDialog.cpp)): **Apply** runs `applyActions` + `onClose(working, persistToDisk=false)`; **Save** runs `applyActions` + `onClose(working, persistToDisk=true)`. Save already = Apply + persist.

**Change:** remove the separate **Apply** button; keep one button (label **"Save"**) that runs the actions, persists, and closes (current Save behavior). Update:
- `resized()` layout ([:201-207](../../../Source/UI/SettingsDialog.cpp)) — drop the Apply slot.
- Save tooltip → plain "Apply the changes and save them as the defaults for every launch."
- Reset tooltip ([:149](../../../Source/UI/SettingsDialog.cpp)) — keep "click Save afterwards to persist" (still accurate).
- Remove `applyButton` member + its `addAndMakeVisible`/`onClick`.
- `Cancel` and `Reset` unchanged.

**Testing:** build + launch; one Save button persists + closes; Cancel discards; Reset still works.

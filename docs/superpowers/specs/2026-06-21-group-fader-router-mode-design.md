# Group Fader Modes: VCA + Router — Design

Date: 2026-06-21
Branch: `feature/group-fader-router-mode`

## Problem

1. **VCA feedback bug.** Group faders use *linked-fader* semantics: moving the
   group fader applies the same dB delta to each member channel's `outputTrim` /
   `inputTrim` in the routing matrix. The audio responds correctly, but the
   per-channel trim sliders in `MatrixView` **never visually move** — they are
   only refreshed by `softRefreshFromEngine()`, which runs solely on a
   settings-restart fast path. The 30 Hz meter timer refreshes meters only. So
   the user sees no movement on the output/input tracks when riding a group
   fader.

2. **Only one fader behaviour exists.** Users want a second, non-destructive
   mode where the group fader is an independent gain stage layered on top of each
   member's own fader, instead of physically moving the member faders.

## Two modes (per group, input and output sides)

- **VCA** (default, current behaviour, backward compatible): moving the group
  fader nudges each member channel's own trim by the same dB delta. Member faders
  visibly move together. One gain stage (the channel trim). Group mute ORs onto
  each member's mute.

- **Router** (new): each member keeps its own independent trim, untouched by the
  group fader. The group fader is a **separate gain stage** multiplied on top:
  `final = channelTrim × groupGain`. Group mute zeroes the router stage (member
  mute state is left alone).

Mode is selectable per group via a toggle on the group card. Both `InputGroupManager`
and `OutputGroupManager` (which share the `OutputGroup` struct) support both modes.

## Data model

`Source/Routing/OutputGroup.h` — add:

```cpp
enum class FaderMode { VCA, Router };
std::atomic<FaderMode> faderMode { FaderMode::VCA };
```

`faderDb` is reused: in VCA it is the group position (delta reference); in Router
it is the overlay gain position. `muted` is reused for both modes.

## Pure helper (testable, JUCE-free)

New header `Source/Routing/GroupGain.h` (only `<cmath>`, `<algorithm>`), so it
can be unit-tested in the no-JUCE `dcorerouter_tests` target. Centralises the dB
helpers currently duplicated in both managers:

```cpp
namespace dcr::groupgain {
  float dbToLin (float db);          // db <= -60 -> 0; else 10^(db/20)
  float linToDb (float lin);         // lin <= 1e-6 -> -60; else 20*log10(lin)
  float clampTrimDb (float db);      // clamp to [-60, 12]
  float routerChannelGain (bool muted, float faderDb); // muted ? 0 : dbToLin(faderDb)
  float bakeVcaTrimDb (float memberDb, float faderDb); // clampTrimDb(memberDb + faderDb)
}
```

Both managers include it and use these for the new logic (and replace their local
anonymous-namespace `dbToLin`/`linToDb` to avoid divergence).

## Manager changes (both managers, mirrored)

New per-channel atomic array, sized to the channel count, default `1.0`:

```cpp
std::vector<std::atomic<float>> channelRouterGain;   // 1.0 == no router contribution
float getChannelRouterGain (int ch) const noexcept;  // lock-free atomic read for the RT thread
void  setGroupFaderMode (int groupIdx, OutputGroup::FaderMode mode, RoutingMatrix&);
```

Rule: for a channel in a Router-mode group, `channelRouterGain[ch] =
routerChannelGain(group.muted, group.faderDb)`. For VCA-mode or ungrouped
channels it is `1.0`.

- **`setNumOutputChannels` / `setNumInputChannels`**: rebuild the array (fresh
  vector of `1.0`) and recompute entries for all Router-mode groups, under the
  existing structural lock. This also covers snapshot restore (members/faderDb/
  faderMode are written directly, then the engine restart calls setNum*).
- **`moveGroupFader`**: branch on mode. VCA = existing delta-to-trim logic.
  Router = store `faderDb`, then write `channelRouterGain[ch]` for members
  (copy member list under the lock, release, then store the atomics — the same
  lock-release pattern the matrix writes already use), then `matrix.touch()`.
- **`setGroupMute`**: branch on mode. VCA = propagate to member `outputMute` /
  `inputMute` (existing). Router = write `channelRouterGain[ch]` for members
  (= 0 when muted), then `matrix.touch()`. Router mode **does not touch member
  matrix mutes** — the per-channel mute stays the user's own, independent of the
  group.
- **`assignChannel`**: after `rebuildChannelLookup()`, recompute router gains
  (structural op, under lock — rare, not a drag).

### Mode switch (`setGroupFaderMode`) — preserve audible level, reset fader to 0

Unified rule: **switching modes never changes the audible level; the group fader
resets to 0 dB.** Member list is copied under the lock then released before any
matrix write.

- **→ Router**: leave member trims; `faderDb = 0`; clear each member's matrix
  mute (`setMute(ch, false)` — Router owns group mute via the gain stage);
  `channelRouterGain[ch] = routerChannelGain(muted, 0)` (= 1.0 unless the group
  is muted, in which case 0). `matrix.touch()`.
- **→ VCA**: bake the current overlay into member trims —
  `setTrim(ch, dbToLin(bakeVcaTrimDb(linToDb(getTrim(ch)), faderDb)))`; `faderDb
  = 0`; re-assert group mute onto members (`setMute(ch, muted)` — VCA owns mute
  via member mutes); `channelRouterGain[ch] = 1.0`. `matrix.touch()`.

This preserves the **audible** mute outcome across a switch (a muted group stays
silent). Individual member mute state set independently by the user is **not**
preserved across a switch — inherent to the linked-mute model, which cannot tell
a group-set mute from a user-set one. Noted, acceptable.

Clamping caveat: baking that pushes a member past +12 / below −60 dB is clamped,
so an extreme overlay can shift the level slightly on the switch. Acceptable and
consistent with the existing VCA ±12/−60 trim range.

## RoutingMatrix

Add a public `void touch() noexcept { bumpDirty(); }` so a Router fader/mute
change (which writes no matrix cell) still retriggers the RT refresh.

## RT engine integration (`MatrixProcessor::refreshSnapshotIfDirty`)

This refresh is already gated on `matrix->getDirtyGeneration()`, so `touch()`
makes it re-run. One extra multiply each side:

- Input (`inputEffGain[n]`): `... = matrix->getInputTrim(n) *
  inputGroupManager->getChannelRouterGain(n)` (after the mute/solo gate; a
  Router-muted channel reads gain 0).
- Output (`outputFaderTarget`): `matrix->getOutputTrim(m) *
  groupManager->getChannelRouterGain(m)` (output mute still forces 0).

Per-block smoothing toward target is unchanged, so router moves don't zipper. No
new allocation, no lock, RT-safe.

## VCA feedback bug fix (`MatrixView`)

In `timerCallback()` (30 Hz), track `lastTrimRefreshGen`; when
`engine.getRoutingMatrix().getDirtyGeneration()` changes, run a focused
`refreshTrimWidgetsFromEngine()`:

- update each visible channel's trim slider (`dontSendNotification`), **skipping
  any slider currently under the mouse** (`isMouseButtonDown()`), and only when
  the value differs beyond a small epsilon;
- update mute / solo toggles to match the engine.

This fixes feedback for every path that mutates trims — group faders, multi-select
links, snapshot restore — not just group faders. Pure UI; no RT impact. It does
not force grid repaints or FX-button refreshes (those have their own paths).

## UI: mode toggle (`OutputGroupPanel::Card`)

Add a compact `VCA` / `RTR` toggle button to each card (left column, below mute).
Click flips the group's mode via a new `mgrSetFaderMode(gIdx, mode)` dispatcher
(mirrors `mgrMoveFader`). The card's existing timer sync already pulls `faderDb`
and `muted`; it also syncs the mode-button label. The fader snaps to 0 on a mode
switch automatically (the timer reads the reset `faderDb`).

## Persistence (`SnapshotStore` + `MainComponent`)

- `Snapshot::Group` gains `int faderMode = 0;` (0 = VCA).
- New identifier `faderMode`; written in `writeGroupList`, read in `readGroupList`
  with default 0 → **old snapshots load as VCA** (backward compatible).
- `gatherCurrentSnapshot`: `gs.faderMode = (int) g->faderMode.load();`
- `restoreToManager`: `g->faderMode.store((OutputGroup::FaderMode) gs.faderMode);`
  Router gains are recomputed by the post-restore `setNum*` call.

## Tests (`tests/test_main.cpp`, no JUCE)

Add cases over `dcr::groupgain`:
- `routerChannelGain`: unmuted returns `dbToLin(faderDb)`; muted returns 0; 0 dB → 1.0.
- `dbToLin`/`linToDb` round-trip and the −60/1e-6 floors.
- `bakeVcaTrimDb`: `bake(x, 0) == clampTrimDb(x)`; additive; clamps at ±12/−60.
- Round-trip identity used by the mode switch: Router level `t*dbToLin(f)` equals
  the baked VCA trim `dbToLin(bakeVcaTrimDb(linToDb(t), f))` within tolerance when
  unclamped.

RT/audio behaviour (actual mixing, no zipper, device output) is verified by the
user on real devices per `CLAUDE.md`.

## Out of scope

No change to group plugin chains, PDC, metering taps, or the master-gain stage.
No new group types. dB helper centralisation is limited to the two managers that
gain the new logic.

## Workflow

Branch `feature/group-fader-router-mode` from `main`; build + `ctest`; open PR;
squash-merge. (Isolated from parallel work happening on another branch.)

# CLAUDE.md — D-Router

Real-time NxM audio routing matrix for macOS. **C++20**, JUCE 8 (fetched via
CMake `FetchContent`), CoreAudio. Hand-written real-time mixing engine.

## Build / run

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# artefact: build/dcorerouter_artefacts/Release/D-Router.app
./run.sh                 # build + relaunch (uses PRODUCT_NAME=D-Router)
./package.sh             # copy to dist/, re-sign ad-hoc, zip (needed or macOS reports "damaged")
```

`PRODUCT_NAME` is **D-Router** (CMakeLists.txt) — the bundle is `D-Router.app`
and the process is `D-Router` (not `dcorerouter`). `build/` and `dist/` are
gitignored; never commit them. `package.sh` re-signs because `cp -R` breaks the
ad-hoc seal and recipients get a "damaged" Gatekeeper error otherwise.

## Tests

```bash
cmake --build build --target dcorerouter_tests
ctest --test-dir build --output-on-failure          # ~0.5s, headless, no JUCE
```

`tests/test_main.cpp` (+ the `dcorerouter_tests` target) is a pure-logic
regression net: `FloatRingBuffer` SPSC + `RoutingMatrix` (gains, mute/solo,
self-loop block, snapshot). **Run it before claiming a change is safe, and add
cases for any new deterministic logic** (e.g. PDC delay-line alignment).
DSP/JUCE-linked tests (STFT/WOLA reconstruction, gain staging) are a planned
second target. Verify against reality — most of the audio behavior can only be
confirmed by the user testing on real devices, so say plainly what is and isn't
verified.

## Workflow

Public repo (`ZDAudio/D-router`) with outside contributors. For non-trivial
changes prefer a branch + PR + squash-merge (keeps history linear and
attributable); small fixes may go straight to `main`. End commit messages with
the `Co-Authored-By` trailer. Keep changes scoped to the task (no drive-by
refactors); follow structural renames all the way through (the review caught
`swapStateWith` forgetting `lastLayout`).

## Architecture

| Area | Path | Role |
|---|---|---|
| Engine | `Source/Engine/` | `DeviceWorker` (per-device CoreAudio IO + per-channel SRC), `MatrixProcessor` (the RT mix thread), SPSC `RingBuffer`, `EngineSettings`, `AudioEngine` (owns it all). |
| Routing | `Source/Routing/` | `RoutingMatrix` (atomic grid of gains/mutes), output/input group managers (per-group **VCA** linked-fader or **Router** overlay-gain mode; `GroupGain.h` = shared JUCE-free dB math). |
| DSP | `Source/DSP/` | `PluginHost` (per-channel, mono), `MultiChannelPluginHost` (group, N-ch), `Builtin/` suite + `InternalPluginFormat`. |
| UI | `Source/UI/` | `MatrixView`, single-component `CrosspointGrid`, group panels, status/engine monitor, dialogs, `LookAndFeel`. |
| Persistence | `Source/Persistence/` | ValueTree/XML snapshots, settings, crash guard. |

**Threads:** message (UI) · one **matrix thread** (RT-scheduled, event-driven via
`MatrixProcessor::inputReady`, drains SPSC rings) · one CoreAudio HAL callback
per device · a `WorkerPool` for parallel plugin processing.

## Non-negotiable invariants

- **Real-time safety** on the matrix thread, the CoreAudio callbacks, and inside
  any `processBlock`: **no heap allocation, no locks, no blocking, no I/O.**
  Pre-size all scratch in `open()`/`prepare()`/`configure()`. Reuse containers
  (`.clear()`), never construct per-block. (Audit history caught several of
  these — keep it clean.)
- **Plugin hosts** swap the plugin under a `juce::SpinLock`; the audio thread
  uses `ScopedTryLockType` and skips the slot if it can't lock or the slot is
  `broken`. Every plugin call (`prepareToPlay`/`processBlock`/dtor) is wrapped in
  `runGuarded` (catches NSException + C++) and sets `broken` on failure.
- **Plugin state restore order**: `prepareToPlay` (+ re-apply bus layout for
  multichannel) **then** `setStateInformation`. `MultiChannelPluginHost::prepare`
  re-applies `lastLayout`; `swapStateWith` must swap `lastLayout` too.
- **Plugin editor lifetime**: close editor windows **before** the plugin
  instance is destroyed (engine restart / slot swap / remove), or teardown
  segfaults. Group editors survive a restart (group plugins persist); per-channel
  editors must be closed (their hosts are dropped by `engine.stop()`).
- **Gain staging** (output path, `MatrixProcessor::tryProcessOneBlock`): mix is
  input-side only (`inputEffGain × crosspoint`); output plugins + group inserts
  run **pre-fader**; then a **post-fader stage** applies output trim/mute; then a
  separate **master gain** (engine-restart click fade — never touches user
  trims). Don't reorder these without updating the meter tap.
- **Built-in DSP** subclasses `BuiltinProcessor`; `processBlock` wraps `processDsp`
  in `ScopedNoDenormals`. To add one: id in `ids` namespace + `makeById` +
  `getBuiltinDescriptions` (all in `BuiltinProcessors.h` / `InternalPluginFormat.cpp`),
  and define any custom `createEditor()` **out-of-line** in `InternalPluginFormat.cpp`.
  Spectral plugins sit on the shared `SpectralProcessor` (STFT/WOLA) base.

## Public-repo hygiene

Keep **third-party brand / product names out of public-facing text** (README,
repo description, releases, LICENSE, code comments, commit messages). The
virtual-device match list in `AudioEngine.cpp` (`"blackhole"`, `"dante"`, …) is
the one exception — it's functional matching data, not prose. LICENSE is
proprietary (© ZDAudio, all rights reserved).

## Known limitation

**No plugin delay compensation (PDC).** Plugins report latency
(`setLatencySamples`) but the engine ignores it, so a spectral plugin on one
output/group desyncs the rest. Being built next — see the project memory.

## Style

Match the surrounding code: same comment density (this codebase comments the
*why*, especially for RT-safety and lifetime ordering), naming, and idioms.
Build before claiming done; report test/verify results honestly.

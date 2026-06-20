<div align="center">

<img src="Resources/icon.png" width="120" alt="D-Router icon" />

# D-Router

**A real-time, low-latency NxM audio routing matrix for macOS.**

*by ZDAudio*

![platform](https://img.shields.io/badge/platform-macOS%2012%2B-black)
![language](https://img.shields.io/badge/C%2B%2B-20-blue)
![status](https://img.shields.io/badge/status-private%20beta-yellow)

</div>

---

D-Router is a software patchbay / routing matrix that connects any combination of
audio devices on your Mac — physical interfaces and virtual loopbacks — and
freely routes channels between them with per-crosspoint gain, sample-rate
conversion, plug-in insert chains, and linked group faders. It is built in
**C++20** for macOS, with a hand-written real-time mixing engine.

> **Status: private beta.** Pre-release software, under active development. Not
> for redistribution.

## Highlights

- **NxM matrix routing** with a single high-performance crosspoint grid that
  stays smooth into the hundreds of channels (target 512×512), per-crosspoint
  gain, mute / solo, and zipper-free gain smoothing.
- **Multi-device, multi-clock** — aggregate any set of input and output devices.
  Each channel gets its own sample-rate converter into a fixed engine clock
  domain; matched-rate paths are bypassed for transparency.
- **Real-time engine** — a dedicated, real-time-scheduled matrix thread,
  event-driven off the device callbacks, feeding per-channel lock-free SPSC ring
  buffers. Live xrun / drop / headroom diagnostics.
- **Plug-in hosting** — host plug-ins in per-channel insert chains (3 slots) and
  multi-channel **group** chains (5 slots), with drag-to-reorder, bypass, and
  crash-guarded load / teardown. Inserts run **pre-fader** (signal → plugin →
  fader → output).
- **18 built-in DSP plug-ins** (see below) — no external dependencies needed to
  get useful processing on any channel or bus.
- **Output / input groups** — linked master faders that move each member's trim,
  group mute, and a shared multi-channel insert chain.
- **Workflow** — per-device channel collapse / expand, snapshot save / load, a
  unified "buffer safety" latency preset, virtual-device self-loop blocking, a
  native menu bar, and close-to-tray.

## Built-in plug-ins

| | | |
|---|---|---|
| Gain / Utility | HP/LP Filter | 5-band Parametric EQ |
| Compressor | Noise Gate | Limiter |
| Reverb | Delay | Tone Generator |
| Tremolo | Stereo Width | De-esser |
| Channel Strip | Multiband Compressor (4-band) | Level Rider |
| PPM Meter | Spectral Auto-EQ | Resonance Suppressor |

Several ship with custom visual editors (EQ curve, compressor transfer + GR
meter, level-rider gain history, spectral curve, PPM ballistics). The two
spectral plug-ins (Auto-EQ, Resonance Suppressor) share an STFT/WOLA engine and
add one FFT frame of latency.

## Building

Requirements: **macOS 12+**, **CMake 3.22+**, and the Xcode command-line tools.
Dependencies are fetched automatically via CMake `FetchContent` — no manual
setup.

```bash
git clone https://github.com/<your-username>/dcorerouter.git
cd dcorerouter
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

The app is produced at:

```
build/dcorerouter_artefacts/Release/D-Router.app
```

`package.sh` re-signs the bundle ad-hoc and zips it for distribution (note:
ad-hoc signing is not notarization — recipients who download the zip must clear
the macOS quarantine flag once: `xattr -cr /path/to/D-Router.app`).

## Architecture

```
device callbacks ─► SPSC input rings ─► [matrix thread] ─► SPSC output rings ─► device output
                                              │
              input inserts → input groups → mix (crosspoints) →
              output inserts → output groups → post-fader gain
```

| Area | Location | Role |
|---|---|---|
| Engine | `Source/Engine/` | Device workers, per-channel SRC, the real-time `MatrixProcessor`, ring buffers, settings. |
| Routing | `Source/Routing/` | `RoutingMatrix` (grid + gains), output / input group managers (linked faders). |
| DSP | `Source/DSP/` | Plug-in hosts, the `Builtin/` plug-in suite + internal plug-in format. |
| UI | `Source/UI/` | `MatrixView`, the single-component `CrosspointGrid`, group panels, status / engine monitor, dialogs, custom look-and-feel. |
| Persistence | `Source/Persistence/` | Tree / XML snapshots, settings, crash guard. |

## License

Proprietary — © ZDAudio. All rights reserved. See [LICENSE](LICENSE).

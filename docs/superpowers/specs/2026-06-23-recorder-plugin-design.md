# Recorder plugin — design

**Status:** Designed 2026-06-23. Not yet implemented.
**Scope:** A built-in *recorder* plugin that captures whatever bus it is inserted
on (per-channel slot → mono file, stereo output group → stereo file) to disk as
WAV / FLAC / AAC, with all disk I/O off the real-time thread.

---

## 1. Goal

A user drops a **Recorder** built-in onto any FX slot and presses **Record** in
its editor to capture that insert point to an audio file. It is a *tap* plugin:
audio passes through unchanged (like the PPM / Stereo meters). What it records is
determined purely by where it is inserted — no special engine plumbing:

- a **per-channel slot** (mono host) → a **mono** file
- a **stereo output group** slot (multichannel host) → a **stereo** file
- an N-channel group → an N-channel file

Formats: **WAV** (16 / 24 / 32-float, default **24-bit**), **FLAC** (lossless),
**AAC** (`.m4a`, lossy). Sample rate always follows the engine's current rate.

### Why a plugin (not a special master tap)

The insert-point model reuses the entire plugin-slot machinery (instantiate,
bypass, snapshot save/restore, editor window) with almost no glue — same
rationale as the other built-ins. To record a stereo program you put the
Recorder on a stereo output group; to record one channel you put it on that
channel's slot. Zero special-casing in `MatrixProcessor`.

---

## 2. Architecture

A `BuiltinProcessor` subclass (`InternalPluginFormat` built-in), id
`builtin:recorder`. Disk writing uses JUCE's `AudioFormatWriter::ThreadedWriter`
(internal lock-free FIFO) serviced by a per-instance `juce::TimeSliceThread`.
This is the only RT-correct way to record: the matrix thread merely copies
samples into a pre-allocated FIFO; the background thread does every file/stream
operation.

```
any FX slot (per-channel mono host, or N-ch group host)
  └─ RecorderProcessor (BuiltinProcessor, pass-through)
        processBlock (matrix thread):
          SpinLock::ScopedTryLockType — if locked && activeWriter != null:
            activeWriter->write(buffer.getArrayOfReadPointers(), ns)   // lock-free, no alloc
          update peak / sample-count atomics (for editor)
  └─ RecorderEditor (juce::AudioProcessorEditor, header-only)
        juce::Timer (UI thread): elapsed time, file size, input meter
        Record/Stop button → startRecording()/stopRecording() on the message thread
        format + bit-depth dropdowns, Name field, folder display + Choose…/Reveal

start/stop (message thread):
  startRecording(): build FileOutputStream + AudioFormatWriter + ThreadedWriter
    (pre-allocates FIFO, ~a few seconds), start TimeSliceThread,
    then under SpinLock set activeWriter = threadedWriter.get()
  stopRecording():  under SpinLock clear activeWriter, then reset ThreadedWriter
    (background flush + finalize the file), stop TimeSliceThread
```

**RT safety:** the only audio-thread work added is one try-locked
`ThreadedWriter::write()` (lock-free) plus a couple of atomic stores. The engine
(`MatrixProcessor`, workers, RT path) is **untouched** — `ctest` stays green.
The `SpinLock` + `ScopedTryLockType` swap mirrors the existing plugin
hot-swap pattern: the message thread holds the lock only for the brief
pointer swap; the audio thread try-locks and, if it can't (only during a
swap), skips writing that block (a sub-millisecond gap, same trade-off the
codebase already accepts for plugin swaps).

### Files (all under `Source/`)

| File | Role |
|---|---|
| `DSP/Builtin/RecorderProcessor.h` | built-in pass-through processor; owns `ThreadedWriter` + `TimeSliceThread` + `SpinLock` + peak/sample-count atomics; start/stop logic; APVTS (format, bit depth); custom state (folder, name) |
| `DSP/Builtin/RecorderEditor.h` | header-only editor (Record/Stop, meter, time, size, dropdowns, Name, folder buttons) — like `PpmMeterEditor.h` |
| `DSP/Builtin/RecorderNaming.h` | **JUCE-free** helpers: `<prefix>_<timestamp>.<ext>` builder, format→extension, prefix sanitization |
| `DSP/Builtin/CoreAudioAacWriter.{h,cpp}` | macOS `ExtAudioFile`-backed `juce::AudioFormatWriter` for AAC (`.m4a`); slots into `ThreadedWriter` like any writer |
| `DSP/Builtin/BuiltinProcessors.h` | `ids::recorder` |
| `DSP/Builtin/InternalPluginFormat.cpp` | include, `makeById`, `getBuiltinDescriptions` list entry, out-of-line `createEditor()` |
| `CMakeLists.txt` | add `CoreAudioAacWriter.cpp` to the app target (AudioToolbox already linked) |
| `tests/CoreLogicTests.cpp` | `RecorderNaming` cases (added to the JUCE-free `dcorerouter_tests`) |

---

## 3. Real-time safety & recording lifecycle (non-negotiable)

- **Matrix thread (`processDsp`):** `SpinLock::ScopedTryLockType`. If locked and
  `activeWriter != nullptr`, call `activeWriter->write(...)` (lock-free, no
  alloc). If the try-lock fails (only during an arm/disarm swap) skip the block.
  No I/O, no allocation, no blocking lock on this thread — ever.
- **Start (message thread, from the editor button):** create `FileOutputStream`,
  `AudioFormatWriter` (via the chosen format), `ThreadedWriter` (this is where
  the FIFO is pre-allocated — message thread, not RT), start the
  `TimeSliceThread`, then under the `SpinLock` publish `activeWriter`.
- **Stop / engine stop / plugin destruction:** under the `SpinLock` clear
  `activeWriter`, then `threadedWriter.reset()` (background flush + finalize),
  stop the `TimeSliceThread`. **Both `releaseResources()` and the destructor
  must stop any in-progress recording** so a partial take is *finalized
  cleanly*, never left corrupt. Consequence (documented, not a bug): an engine
  reconfigure/restart that tears down or re-prepares the host **ends the current
  recording** (cleanly finalized).
- Writer channel count is fixed at `startRecording()` from the live buffer's
  channel count; the writer's sample rate is the engine rate (`dspSampleRate`).

---

## 4. Format & file destination

- **WAV** (`juce::WavAudioFormat`) at 16 / 24 / 32-float, default **24**; **FLAC**
  (`juce::FlacAudioFormat`, lossless) — both native to JUCE. **AAC** (`.m4a`) via
  `CoreAudioAacWriter` (macOS `ExtAudioFile`, `kAudioFormatMPEG4AAC`). Format and
  bit depth are APVTS `AudioParameterChoice` params (bit depth only meaningful
  for WAV; FLAC uses 16/24, AAC ignores it).
- **Fixed folder + auto-naming.** The output folder persists; if unset on first
  record it falls back to `~/Music/D-Router Recordings` (created if missing) so
  Record never blocks on a dialog. Filename = `<prefix>_<YYYY-MM-DD_HH-MM-SS>.<ext>`.
  Same-second collisions resolved with `juce::File::getNonexistentSibling`.
  `prefix` comes from the editor's editable **Name** field (default
  `"Recording"`), sanitized to filename-safe characters.

---

## 5. Editor

Header-only `juce::AudioProcessorEditor` (no new `.mm`/CMake source, like the
other editors). A `juce::Timer` polls the processor's atomics:

- large **Record / Stop** button (red + pulsing dot while armed)
- **elapsed time** (samples-written / sample-rate) and **file size** estimate
- **input level meter** (block-peak atomic, same idea as other editors)
- **Format** and **Bit depth** dropdowns (bit depth disabled unless WAV)
- **Name** text field (filename prefix)
- **folder** display + **Choose folder…** + **Reveal in Finder**

Uses the existing `LookAndFeel`. Controls that change the file (format, bit
depth, folder, name) are disabled while recording.

---

## 6. Persistence, tests, build

- **Persistence:** format + bit depth via APVTS. Folder path + name prefix via
  overridden `get/setStateInformation` (APVTS XML plus a sibling child element
  for the strings). **"Is recording" is never persisted** — a restored Recorder
  always loads in the stopped state.
- **Tests:** `RecorderNaming.h` is JUCE-free and covered in `CoreLogicTests.cpp`
  (`dcorerouter_tests`): timestamp+prefix→filename, format→extension, prefix
  sanitization, collision suffixing logic. The rest is disk I/O and device
  behavior — **only verifiable by the user on a real device**; the writeup will
  state plainly what is and isn't verified.
- **Build:** AudioToolbox is already linked (main app + JUCE test target). FLAC
  ships on by default in `juce_audio_formats` (confirm `JUCE_USE_FLAC`). Add
  `Source/DSP/Builtin/CoreAudioAacWriter.cpp` to the `dcorerouter` target. Run
  `clang-format` on every new/changed file (CI fails otherwise).

---

## 7. Known characteristics (by design, not bugs)

- **Group inserts run pre-fader** (per the gain-staging invariant: output
  plugins + group inserts run pre-fader, then a post-fader trim/mute stage).
  So the Recorder captures the **pre-fader program signal**, independent of the
  monitor fader — the usual, desired behavior for recording a processed program.
- A per-channel slot is a **mono** host, so per-channel recordings are mono;
  stereo/N-ch recording is done by inserting on an output group.
- Multiple Recorders (e.g. one per group) can record simultaneously; each is an
  independent instance with its own writer + `TimeSliceThread`. Timestamped
  names avoid collisions across instances.

---

## 8. Implementation phases

1. **`RecorderNaming.h` + tests** — JUCE-free naming/extension/sanitize logic,
   `CoreLogicTests.cpp` cases. (Red→green first.)
2. **`RecorderProcessor` (WAV + FLAC)** — pass-through tap, `ThreadedWriter` +
   `TimeSliceThread`, try-locked swap, start/stop, state, atomics. Register the
   built-in. Verify pass-through doesn't disturb audio (`ctest` green).
3. **`RecorderEditor`** — button, meter, time/size, dropdowns, Name, folder
   buttons. Out-of-line `createEditor()`.
4. **`CoreAudioAacWriter` (AAC)** — macOS `ExtAudioFile` writer, wire `.m4a` into
   the format choice + naming. CMake source.
5. **Real-device verification (user)** — record/stop on a real group + channel,
   each format, confirm files open and play; confirm engine-stop finalizes a
   take cleanly.

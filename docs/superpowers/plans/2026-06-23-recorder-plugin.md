# Recorder Plugin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A built-in "Recorder" plugin that captures whatever bus it is inserted on (per-channel slot → 2-ch duplicated-mono file, output group → stereo/N-ch file) to WAV / FLAC / AAC, with all disk I/O off the matrix thread.

**Architecture:** A `BuiltinProcessor` pass-through "tap" (like the PPM/Stereo meters). The matrix thread pushes float frames into a JUCE `AudioFormatWriter::ThreadedWriter` (lock-free FIFO); a per-instance `TimeSliceThread` does every file/stream op. Arm/disarm swaps the active-writer pointer under a `SpinLock`; the matrix thread try-locks and skips a block only during the brief swap — the same pattern the plugin hosts use for hot-swap. WAV/FLAC use stock JUCE writers; AAC uses a macOS `ExtAudioFile`-backed custom `AudioFormatWriter` that slots into the same `ThreadedWriter`.

**Tech Stack:** C++20, JUCE 8 (`juce_audio_formats`: `WavAudioFormat`, `FlacAudioFormat`, `AudioFormatWriter::ThreadedWriter`, `TimeSliceThread`), macOS AudioToolbox (`ExtAudioFile`, already linked), the existing `InternalPluginFormat` built-in pipeline.

---

## Reference facts (verified against the JUCE in `build/_deps/juce-src`)

- `AudioFormat::createWriterFor (OutputStream*, double sampleRate, unsigned int numChannels, int bitsPerSample, const StringPairArray& metadata, int qualityOptionIndex)` — the writer **owns and deletes the stream on success**; on failure (returns `nullptr`) the **caller keeps the stream**. Pattern: `auto* w = fmt.createWriterFor (fos.get(), …); if (w) fos.release();`.
- `WavAudioFormat` writer with `bitsPerSample == 32` sets `usesFloatingPointData = true` → **real 32-bit float WAV**. 16/24 are fixed-point.
- `FlacAudioFormat` is compiled in (`JUCE_USE_FLAC` defaults to `1`). FLAC supports 16/24 bits only (no float) — clamp a 32-float request to 24.
- `AudioFormatWriter::ThreadedWriter (AudioFormatWriter* writer, TimeSliceThread& thread, int numSamplesToBuffer)` owns/deletes the writer; `bool write (const float* const* data, int numSamples)` is the lock-free push (data channel count **must equal** the writer's channel count); `setFlushInterval (int)` flushes periodically; **deleting** the `ThreadedWriter` flushes + finalizes.
- `AudioBuffer<float>::getArrayOfReadPointers()` returns `const float* const*` — matches `ThreadedWriter::write`.
- The matrix-thread RT rule is satisfied by `juce::SpinLock` + `ScopedTryLockType` (the codebase's sanctioned plugin-swap pattern), **not** a blocking lock.

## File structure

| File | Responsibility |
|---|---|
| `Source/DSP/Builtin/RecorderNaming.h` | **JUCE-free** naming: format→extension, prefix sanitize, `<prefix>_<timestamp>.<ext>` builder. Unit-tested. |
| `Source/DSP/Builtin/CoreAudioAacWriter.{h,cpp}` | macOS `ExtAudioFile`-backed `juce::AudioFormatWriter` for AAC (`.m4a`). `.cpp` (AudioToolbox is a C API — no `.mm`/ARC needed). |
| `Source/DSP/Builtin/RecorderProcessor.h` | the built-in: pass-through tap + `ThreadedWriter`/`TimeSliceThread` recording engine + APVTS (format, bits) + folder/prefix state + editor readout atomics. |
| `Source/DSP/Builtin/RecorderEditor.h` | header-only editor: Record/Stop, time, size, input meter, format/bits combos, Name field, folder buttons. |
| `Source/DSP/Builtin/BuiltinProcessors.h` | add `ids::recorder`. |
| `Source/DSP/Builtin/InternalPluginFormat.cpp` | include + `makeById` + `getBuiltinDescriptions` list + out-of-line `createEditor()`. |
| `CMakeLists.txt` | add `CoreAudioAacWriter.cpp` to the `dcorerouter` target. |
| `tests/CoreLogicTests.cpp` | `RecorderNaming` cases + `main()` call. |

---

## Task 1: RecorderNaming.h (JUCE-free) + tests

**Files:**
- Create: `Source/DSP/Builtin/RecorderNaming.h`
- Modify: `tests/CoreLogicTests.cpp` (add include near the other `DSP/Builtin/*` includes at the top; add `test_recorder_naming()` function; call it in `main()` after `test_stereometer_high_lift_gain();` at line ~855)

- [ ] **Step 1: Write the failing test**

Add this function to `tests/CoreLogicTests.cpp` (anywhere among the other `test_*` functions, e.g. just before `int main()`):

```cpp
    // ---------------------------------------------------------------------------
    // RecorderNaming (JUCE-free recording-file naming)
    // ---------------------------------------------------------------------------
    void test_recorder_naming()
    {
        using namespace dcr::recorder;
        CHECK (extensionForFormat (0) == "wav");
        CHECK (extensionForFormat (1) == "flac");
        CHECK (extensionForFormat (2) == "m4a");
        CHECK (extensionForFormat (99) == "wav"); // out-of-range clamps

        CHECK (sanitizePrefix ("Vocals") == "Vocals");
        CHECK (sanitizePrefix ("a/b:c") == "a_b_c"); // unsafe -> '_'
        CHECK (sanitizePrefix ("") == "Recording"); // empty -> fallback
        CHECK (sanitizePrefix ("   ") == "Recording"); // whitespace -> fallback
        CHECK (sanitizePrefix ("__Lead__") == "Lead"); // trim _ . space
        CHECK (sanitizePrefix ("My Take 1") == "My Take 1"); // spaces kept

        CHECK (makeFileName ("Drums", 2026, 6, 23, 14, 30, 5, 0)
               == "Drums_2026-06-23_14-30-05.wav");
        CHECK (makeFileName ("", 2026, 12, 1, 9, 8, 7, 2)
               == "Recording_2026-12-01_09-08-07.m4a"); // zero-pad + fallback
    }
```

Add the include at the top of the file with the other built-in includes:

```cpp
#include "DSP/Builtin/RecorderNaming.h"
```

Add the call in `main()` right after `test_stereometer_high_lift_gain();`:

```cpp
    test_recorder_naming();
```

- [ ] **Step 2: Run test to verify it fails (won't compile yet)**

Run: `cmake --build build --target dcorerouter_tests 2>&1 | tail -20`
Expected: FAIL — `fatal error: 'DSP/Builtin/RecorderNaming.h' file not found` (header doesn't exist yet).

- [ ] **Step 3: Write the minimal implementation**

Create `Source/DSP/Builtin/RecorderNaming.h`:

```cpp
#pragma once

#include <string>

// JUCE-free recording-file naming helpers.  Kept dependency-free so the
// pure-logic test target (dcorerouter_tests) covers them without linking JUCE.
// The processor supplies the wall-clock fields (from juce::Time, message
// thread) and the format index; collision avoidance is the caller's job
// (juce::File::getNonexistentSibling).
namespace dcr::recorder
{
    // Format index -> lower-case extension (no dot).  0 = WAV, 1 = FLAC,
    // 2 = AAC (.m4a).  Out-of-range clamps to wav.
    inline std::string extensionForFormat (int formatIndex)
    {
        switch (formatIndex)
        {
            case 1:  return "flac";
            case 2:  return "m4a";
            default: return "wav";
        }
    }

    // Replace anything outside [A-Za-z0-9 _-] with '_', then trim surrounding
    // spaces / dots / underscores; fall back when nothing usable is left.
    inline std::string sanitizePrefix (const std::string& raw,
                                       const std::string& fallback = "Recording")
    {
        std::string out;
        out.reserve (raw.size());
        for (char c : raw)
        {
            const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                         || (c >= '0' && c <= '9') || c == ' ' || c == '_' || c == '-';
            out += ok ? c : '_';
        }
        const auto isTrim = [] (char c) { return c == ' ' || c == '.' || c == '_'; };
        std::size_t b = 0, e = out.size();
        while (b < e && isTrim (out[b]))
            ++b;
        while (e > b && isTrim (out[e - 1]))
            --e;
        out = out.substr (b, e - b);
        return out.empty() ? fallback : out;
    }

    // "<sanitized prefix>_YYYY-MM-DD_HH-MM-SS.<ext>".  month/day are 1-based.
    inline std::string makeFileName (const std::string& prefix,
                                     int year, int month, int day,
                                     int hour, int minute, int second,
                                     int formatIndex)
    {
        const auto p2 = [] (int v) {
            std::string s = std::to_string (v);
            return v < 10 ? "0" + s : s;
        };
        return sanitizePrefix (prefix) + "_"
             + std::to_string (year) + "-" + p2 (month) + "-" + p2 (day) + "_"
             + p2 (hour) + "-" + p2 (minute) + "-" + p2 (second) + "."
             + extensionForFormat (formatIndex);
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure -R dcorerouter_tests`
Expected: PASS — "0 failures", ctest `100% tests passed`.

- [ ] **Step 5: Format + commit**

```bash
clang-format -i Source/DSP/Builtin/RecorderNaming.h tests/CoreLogicTests.cpp
git add Source/DSP/Builtin/RecorderNaming.h tests/CoreLogicTests.cpp
git commit -m "feat(recorder): JUCE-free file-naming helper + tests

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: CoreAudioAacWriter (AAC `.m4a` via ExtAudioFile)

**Files:**
- Create: `Source/DSP/Builtin/CoreAudioAacWriter.h`
- Create: `Source/DSP/Builtin/CoreAudioAacWriter.cpp`
- Modify: `CMakeLists.txt` (add the `.cpp` to the `dcorerouter` target sources, next to `Source/DSP/Builtin/InternalPluginFormat.cpp` at line ~72)

- [ ] **Step 1: Create the header**

Create `Source/DSP/Builtin/CoreAudioAacWriter.h`:

```cpp
#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <memory>

namespace dcr::recorder
{
    // Creates an AAC (.m4a) AudioFormatWriter backed by macOS AudioToolbox
    // ExtAudioFile.  Stock JUCE has no AAC encoder; this slots into
    // AudioFormatWriter::ThreadedWriter like any other writer.  The writer is
    // float-native (usesFloatingPointData == true): the matrix thread's float
    // buffers are handed to the codec (on the background writer thread) as
    // non-interleaved Float32 PCM.  Returns nullptr if the file/codec can't be
    // opened.  Bitrate is the codec default (~128 kbps).
    std::unique_ptr<juce::AudioFormatWriter> createCoreAudioAacWriter (const juce::File& file,
                                                                       double sampleRate,
                                                                       int numChannels);
}
```

- [ ] **Step 2: Create the implementation**

Create `Source/DSP/Builtin/CoreAudioAacWriter.cpp`:

```cpp
#include "DSP/Builtin/CoreAudioAacWriter.h"

#include <AudioToolbox/AudioToolbox.h>

namespace dcr::recorder
{
    namespace
    {
        // Encodes float PCM to AAC in an .m4a container via ExtAudioFile.  Owned
        // by a ThreadedWriter, which calls write() on its background thread and
        // deletes it (flush + finalize) when recording stops.  All work here is
        // on that background thread -- blocking disk I/O and heap use are fine.
        class CoreAudioAacWriter final : public juce::AudioFormatWriter
        {
        public:
            static constexpr int kMaxChannels = 32;

            CoreAudioAacWriter (const juce::File& file, double sr, int numCh)
                : juce::AudioFormatWriter (nullptr, "AAC", sr, (unsigned int) numCh, 32)
            {
                usesFloatingPointData = true; // we feed the codec Float32 PCM

                AudioStreamBasicDescription out {};
                out.mFormatID = kAudioFormatMPEG4AAC;
                out.mSampleRate = sr;
                out.mChannelsPerFrame = (UInt32) numCh; // codec fills packet sizes

                CFStringRef cfPath = file.getFullPathName().toCFString();
                CFURLRef url = CFURLCreateWithFileSystemPath (kCFAllocatorDefault, cfPath,
                                                              kCFURLPOSIXPathStyle, false);
                CFRelease (cfPath);
                if (url == nullptr)
                    return;

                OSStatus st = ExtAudioFileCreateWithURL (url, kAudioFileM4AType, &out, nullptr,
                                                         kAudioFileFlags_EraseFile, &extFile);
                CFRelease (url);
                if (st != noErr || extFile == nullptr)
                {
                    extFile = nullptr;
                    return;
                }

                // Client (source) format: non-interleaved Float32 -- matches the
                // float* per-channel arrays JUCE hands to write().
                AudioStreamBasicDescription client {};
                client.mFormatID = kAudioFormatLinearPCM;
                client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked
                                    | kAudioFormatFlagIsNonInterleaved;
                client.mSampleRate = sr;
                client.mChannelsPerFrame = (UInt32) numCh;
                client.mBitsPerChannel = 32;
                client.mFramesPerPacket = 1;
                client.mBytesPerFrame = 4; // one float per (non-interleaved) channel
                client.mBytesPerPacket = 4;

                st = ExtAudioFileSetProperty (extFile, kExtAudioFileProperty_ClientDataFormat,
                                              sizeof (client), &client);
                if (st != noErr)
                {
                    ExtAudioFileDispose (extFile);
                    extFile = nullptr;
                    return;
                }

                channels = juce::jmin (numCh, kMaxChannels);
                opened = true;
            }

            ~CoreAudioAacWriter() override
            {
                if (extFile != nullptr)
                    ExtAudioFileDispose (extFile); // finalize the .m4a
            }

            bool isOpen() const noexcept { return opened; }

            // numSamples frames; for a float writer, samplesToWrite is float* per
            // channel cast to int* (per AudioFormatWriter::write contract).
            bool write (const int** samplesToWrite, int numSamples) override
            {
                if (! opened || extFile == nullptr || numSamples <= 0)
                    return false;

                const int n = channels;
                // Non-interleaved AudioBufferList pointing straight at the caller's
                // float arrays (no copy).  Stack storage avoids per-call alloc.
                char storage[sizeof (AudioBufferList) + (kMaxChannels - 1) * sizeof (AudioBuffer)];
                auto* abl = reinterpret_cast<AudioBufferList*> (storage);
                abl->mNumberBuffers = (UInt32) n;
                for (int ch = 0; ch < n; ++ch)
                {
                    abl->mBuffers[ch].mNumberChannels = 1;
                    abl->mBuffers[ch].mDataByteSize = (UInt32) numSamples * 4;
                    abl->mBuffers[ch].mData = const_cast<int*> (samplesToWrite[ch]);
                }

                return ExtAudioFileWrite (extFile, (UInt32) numSamples, abl) == noErr;
            }

        private:
            ExtAudioFileRef extFile = nullptr;
            int channels = 0;
            bool opened = false;
        };
    }

    std::unique_ptr<juce::AudioFormatWriter> createCoreAudioAacWriter (const juce::File& file,
                                                                       double sampleRate,
                                                                       int numChannels)
    {
        auto w = std::make_unique<CoreAudioAacWriter> (file, sampleRate, juce::jmax (1, numChannels));
        if (! w->isOpen())
            return nullptr;
        return w;
    }
}
```

- [ ] **Step 3: Wire into CMake**

In `CMakeLists.txt`, find `Source/DSP/Builtin/InternalPluginFormat.cpp` (≈ line 72) in the `dcorerouter` `target_sources`/app source list and add directly below it:

```cmake
    Source/DSP/Builtin/CoreAudioAacWriter.cpp
```

(AudioToolbox is already linked — `"-framework AudioToolbox"`, line ~115. No new framework.)

- [ ] **Step 4: Configure + build to verify it compiles**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 2>&1 | tail -25`
Expected: build succeeds (no link/compile errors). The writer isn't reachable from the UI yet — this step only proves it compiles and links against AudioToolbox.

- [ ] **Step 5: Format + commit**

```bash
clang-format -i Source/DSP/Builtin/CoreAudioAacWriter.h Source/DSP/Builtin/CoreAudioAacWriter.cpp
git add Source/DSP/Builtin/CoreAudioAacWriter.h Source/DSP/Builtin/CoreAudioAacWriter.cpp CMakeLists.txt
git commit -m "feat(recorder): macOS ExtAudioFile-backed AAC (.m4a) AudioFormatWriter

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: RecorderProcessor + registration

**Files:**
- Create: `Source/DSP/Builtin/RecorderProcessor.h`
- Modify: `Source/DSP/Builtin/BuiltinProcessors.h` (add `ids::recorder`, in the `ids` namespace at lines 31–52)
- Modify: `Source/DSP/Builtin/InternalPluginFormat.cpp` (include, `makeById`, `getBuiltinDescriptions` list — editor wiring comes in Task 4)

- [ ] **Step 1: Add the id**

In `Source/DSP/Builtin/BuiltinProcessors.h`, inside `namespace ids` (after `stereo_meter` at line 51):

```cpp
        static constexpr const char* recorder = "builtin:recorder";
```

- [ ] **Step 2: Create the processor**

Create `Source/DSP/Builtin/RecorderProcessor.h`:

```cpp
#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"
#include "DSP/Builtin/CoreAudioAacWriter.h"
#include "DSP/Builtin/RecorderNaming.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>

namespace dcr::builtin
{
    // ===========================================================================
    // Recorder -- a pass-through "tap" that records its insert point to disk.
    // Per-channel slot -> mono file; stereo/N-ch output group -> stereo/N-ch file.
    //
    // RT safety: the matrix thread only pushes float frames into a JUCE
    // AudioFormatWriter::ThreadedWriter (lock-free FIFO).  A TimeSliceThread does
    // every file/stream operation.  Arm/disarm swaps the active-writer pointer
    // under a SpinLock; the matrix thread uses ScopedTryLockType and skips the
    // block if it can't lock (only ever during the brief swap) -- the same
    // pattern the plugin hosts use to hot-swap plugins.
    // ===========================================================================
    class RecorderProcessor : public BuiltinProcessor
    {
    public:
        RecorderProcessor() : BuiltinProcessor (ids::recorder, "Recorder", createLayout())
        {
            formatParam = apvts.getRawParameterValue ("format");
            bitsParam = apvts.getRawParameterValue ("bits");
        }

        ~RecorderProcessor() override { stopRecording(); }

        static APVTS::ParameterLayout createLayout()
        {
            APVTS::ParameterLayout l;
            l.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "format", 1 }, "Format",
                juce::StringArray { "WAV", "FLAC", "AAC (.m4a)" }, 0));
            l.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "bits", 1 }, "Bit depth",
                juce::StringArray { "16-bit", "24-bit", "32-bit float" }, 1));
            return l;
        }

        juce::AudioProcessorEditor* createEditor() override; // RecorderEditor.h (Task 4)

        // ---- editor-facing API (message thread) --------------------------------
        bool isRecording() const noexcept { return recording.load (std::memory_order_relaxed); }
        bool audioFlowing() const noexcept { return liveChannels.load (std::memory_order_relaxed) > 0; }
        double recordedSeconds() const noexcept
        {
            const double sr = dspSampleRate > 0.0 ? dspSampleRate : 48000.0;
            return (double) samplesWritten.load (std::memory_order_relaxed) / sr;
        }
        float inputPeak() const noexcept { return peak.load (std::memory_order_relaxed); }
        juce::File currentFile() const { return juce::File (currentPath); }

        juce::File outputFolder() const
        {
            return folder.isNotEmpty() ? juce::File (folder) : defaultFolder();
        }
        void setOutputFolder (const juce::File& f) { folder = f.getFullPathName(); }
        juce::String namePrefix() const { return prefix; }
        void setNamePrefix (const juce::String& p) { prefix = p; }

        static juce::File defaultFolder()
        {
            return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                .getChildFile ("D-Router Recordings");
        }

        // Start/stop: message-thread only (called from the editor button).
        void startRecording()
        {
            stopRecording(); // idempotent: finalize any take in progress

            const int nch = juce::jmax (1, liveChannels.load (std::memory_order_relaxed));
            const double sr = dspSampleRate > 0.0 ? dspSampleRate : 48000.0;
            const int fmt = (int) (formatParam != nullptr ? formatParam->load() : 0.0f);

            juce::File dir = outputFolder();
            dir.createDirectory();

            const auto now = juce::Time::getCurrentTime();
            const auto name = dcr::recorder::makeFileName (prefix.toStdString(),
                now.getYear(), now.getMonth() + 1, now.getDayOfMonth(),
                now.getHours(), now.getMinutes(), now.getSeconds(), fmt);
            const juce::File file = dir.getChildFile (name).getNonexistentSibling();

            std::unique_ptr<juce::AudioFormatWriter> writer (makeWriter (file, sr, nch, fmt));
            if (writer == nullptr)
                return; // open failed -> stays stopped

            if (! backgroundThread.isThreadRunning())
                backgroundThread.startThread();

            auto tw = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
                writer.release(), backgroundThread, 1 << 18); // ~5 s FIFO, pre-allocated here
            tw->setFlushInterval ((int) sr); // ~1 s: a crash still leaves a valid file

            samplesWritten.store (0, std::memory_order_relaxed);
            currentPath = file.getFullPathName();
            recChannels = nch;

            {
                const juce::SpinLock::ScopedLockType sl (writerLock);
                threadedWriter = std::move (tw);
                activeWriter = threadedWriter.get();
            }
            recording.store (true, std::memory_order_relaxed);
        }

        void stopRecording()
        {
            {
                const juce::SpinLock::ScopedLockType sl (writerLock);
                activeWriter = nullptr; // matrix thread stops writing at once
            }
            threadedWriter.reset(); // background flush + finalize file
            recording.store (false, std::memory_order_relaxed);
        }

        // ---- state: APVTS XML + folder/prefix attributes -----------------------
        void getStateInformation (juce::MemoryBlock& dest) override
        {
            if (auto xml = apvts.copyState().createXml())
            {
                xml->setAttribute ("recFolder", folder);
                xml->setAttribute ("recPrefix", prefix);
                copyXmlToBinary (*xml, dest);
            }
        }
        void setStateInformation (const void* data, int size) override
        {
            if (auto xml = getXmlFromBinary (data, size))
            {
                folder = xml->getStringAttribute ("recFolder", folder);
                prefix = xml->getStringAttribute ("recPrefix", prefix);
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
            }
        }

    protected:
        void prepareDsp (double sr, int, int) override
        {
            dspSampleRate = sr;
            // A reconfigure re-prepares us; any take in progress was already
            // finalized in releaseResources().  liveChannels is set from the real
            // buffer in processDsp (NOT from the inflated preparedChannels).
            peak.store (0.0f, std::memory_order_relaxed);
        }

        void releaseResources() override
        {
            stopRecording(); // finalize a take cleanly across engine stop/restart
        }

        void processDsp (juce::AudioBuffer<float>& buffer) override
        {
            const int ns = buffer.getNumSamples();
            const int nch = buffer.getNumChannels();
            if (nch <= 0 || ns <= 0)
                return;

            liveChannels.store (nch, std::memory_order_relaxed);

            // input meter (cheap, no alloc)
            float pk = 0.0f;
            for (int ch = 0; ch < nch; ++ch)
                pk = juce::jmax (pk, buffer.getMagnitude (ch, 0, ns));
            peak.store (pk, std::memory_order_relaxed);

            // push to the disk-writer FIFO under a try-lock: never blocks the
            // matrix thread; skips a block only during an arm/disarm swap.
            const juce::SpinLock::ScopedTryLockType sl (writerLock);
            if (sl.isLocked() && activeWriter != nullptr && nch == recChannels)
            {
                if (activeWriter->write (buffer.getArrayOfReadPointers(), ns))
                    samplesWritten.fetch_add (ns, std::memory_order_relaxed);
            }
            // Audio passes through unchanged (this is a tap).
        }

    private:
        // WAV / FLAC via stock JUCE; AAC via the macOS ExtAudioFile writer.
        juce::AudioFormatWriter* makeWriter (const juce::File& file, double sr, int nch, int fmt)
        {
            if (fmt == 2) // AAC
                return dcr::recorder::createCoreAudioAacWriter (file, sr, nch).release();

            const int bitsChoice = (int) (bitsParam != nullptr ? bitsParam->load() : 1.0f);
            std::unique_ptr<juce::AudioFormat> af;
            int bits = 24;
            if (fmt == 1) // FLAC: 16/24 only (no float) -> clamp
            {
                af = std::make_unique<juce::FlacAudioFormat>();
                bits = (bitsChoice == 0) ? 16 : 24;
            }
            else // WAV: 16 / 24 / 32-float
            {
                af = std::make_unique<juce::WavAudioFormat>();
                bits = (bitsChoice == 0) ? 16 : (bitsChoice == 1) ? 24 : 32;
            }

            auto fos = std::make_unique<juce::FileOutputStream> (file);
            if (! fos->openedOk())
                return nullptr;

            juce::StringPairArray meta;
            if (auto* w = af->createWriterFor (fos.get(), sr, (unsigned int) nch, bits, meta, 0))
            {
                fos.release(); // the writer owns the stream now
                return w;
            }
            return nullptr; // createWriterFor failed -> unique_ptr deletes the stream
        }

        std::atomic<float>* formatParam = nullptr;
        std::atomic<float>* bitsParam = nullptr;

        juce::TimeSliceThread backgroundThread { "D-Router Recorder" };
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;

        juce::SpinLock writerLock; // guards the swap; try-locked on the matrix thread
        juce::AudioFormatWriter::ThreadedWriter* activeWriter = nullptr;
        int recChannels = 0;

        std::atomic<bool> recording { false };
        std::atomic<juce::int64> samplesWritten { 0 };
        std::atomic<float> peak { 0.0f };
        std::atomic<int> liveChannels { 0 };

        juce::String folder;                 // empty -> defaultFolder()
        juce::String prefix { "Recording" }; // filename prefix
        juce::String currentPath;            // last/active file
    };
}
```

- [ ] **Step 3: Register in `makeById` + descriptions (no editor yet)**

In `Source/DSP/Builtin/InternalPluginFormat.cpp`:

Add the include with the others (after `PpmMeterProcessor.h`, line ~10):

```cpp
#include "DSP/Builtin/RecorderProcessor.h"
```

In `makeById`, after the `ids::resonance` branch (line ~74):

```cpp
            if (id == ids::recorder)
                return std::make_unique<RecorderProcessor>();
```

In `getBuiltinDescriptions`, add `ids::recorder` to the end of the `allIds[]` array (line ~84):

```cpp
            ids::gain, ids::filter, ids::eq, ids::compressor, ids::gate, ids::limiter, ids::reverb, ids::delay, ids::tone, ids::tremolo, ids::width, ids::deesser, ids::strip, ids::mbcomp, ids::leveler, ids::ppm, ids::autoeq, ids::resonance, ids::recorder
```

> Note: `RecorderProcessor::createEditor()` is declared but defined out-of-line only in Task 4. So a full app build here will **compile** `RecorderProcessor.h` (via `InternalPluginFormat.cpp`) but **fail at link** with one undefined symbol: `RecorderProcessor::createEditor`. That is the expected outcome of this task — it proves the processor header is compile-clean; Task 4 closes the link gap.

- [ ] **Step 4: Build the app to compile-check the processor (link error expected)**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 2>&1 | tail -25`
Expected: all source files **compile** cleanly; the build fails **only** at the final link step with `Undefined symbol: dcr::builtin::RecorderProcessor::createEditor()`. If you see any *compile* error (in `RecorderProcessor.h` / `InternalPluginFormat.cpp`), fix it before committing. Any link error other than the single `createEditor` symbol is also a real problem to fix.

- [ ] **Step 5: Verify the JUCE-free tests still pass (regression guard)**

Run: `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure -R dcorerouter_tests`
Expected: PASS — the engine/core logic is untouched, `0 failures`.

- [ ] **Step 6: Format + commit**

```bash
clang-format -i Source/DSP/Builtin/RecorderProcessor.h Source/DSP/Builtin/BuiltinProcessors.h Source/DSP/Builtin/InternalPluginFormat.cpp
git add Source/DSP/Builtin/RecorderProcessor.h Source/DSP/Builtin/BuiltinProcessors.h Source/DSP/Builtin/InternalPluginFormat.cpp
git commit -m "feat(recorder): pass-through tap processor + ThreadedWriter recording engine

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: RecorderEditor + createEditor wiring

**Files:**
- Create: `Source/DSP/Builtin/RecorderEditor.h`
- Modify: `Source/DSP/Builtin/InternalPluginFormat.cpp` (include + out-of-line `createEditor()`)

- [ ] **Step 1: Create the editor**

Create `Source/DSP/Builtin/RecorderEditor.h`:

```cpp
#pragma once

#include "DSP/Builtin/RecorderProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{
    // Record/Stop + elapsed time + file size + input meter, with format / bit-depth
    // combos (APVTS-attached), an editable Name (filename prefix), and folder
    // choose / reveal.  A 15 Hz timer polls the processor's atomics.
    class RecorderEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
    {
    public:
        explicit RecorderEditor (RecorderProcessor& p)
            : juce::AudioProcessorEditor (p), rec (p)
        {
            using SA = juce::AudioProcessorValueTreeState;

            recordButton.onClick = [this] { toggleRecord(); };
            addAndMakeVisible (recordButton);

            timeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
            timeLabel.setFont (juce::FontOptions (18.0f, juce::Font::bold));
            addAndMakeVisible (timeLabel);
            sizeLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (150, 150, 160));
            addAndMakeVisible (sizeLabel);

            formatBox.addItemList ({ "WAV", "FLAC", "AAC (.m4a)" }, 1);
            addAndMakeVisible (formatBox);
            formatAtt = std::make_unique<SA::ComboBoxAttachment> (rec.getValueTreeState(), "format", formatBox);

            bitsBox.addItemList ({ "16-bit", "24-bit", "32-bit float" }, 1);
            addAndMakeVisible (bitsBox);
            bitsAtt = std::make_unique<SA::ComboBoxAttachment> (rec.getValueTreeState(), "bits", bitsBox);

            nameField.setText (rec.namePrefix(), juce::dontSendNotification);
            nameField.onTextChange = [this] { rec.setNamePrefix (nameField.getText()); };
            addAndMakeVisible (nameField);

            folderButton.setButtonText ("Folder...");
            folderButton.onClick = [this] { chooseFolder(); };
            addAndMakeVisible (folderButton);

            revealButton.setButtonText ("Reveal");
            revealButton.onClick = [this] { rec.outputFolder().revealToUser(); };
            addAndMakeVisible (revealButton);

            folderLabel.setColour (juce::Label::textColourId, juce::Colour::fromRGB (140, 140, 150));
            folderLabel.setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (folderLabel);

            setSize (380, 250);
            startTimerHz (15);
            refresh();
        }

        ~RecorderEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds().reduced (12);
            recordButton.setBounds (r.removeFromTop (44));
            r.removeFromTop (6);
            meterArea = r.removeFromTop (14);
            r.removeFromTop (6);
            timeLabel.setBounds (r.removeFromTop (24));
            sizeLabel.setBounds (r.removeFromTop (18));
            r.removeFromTop (10);
            auto fmtRow = r.removeFromTop (24);
            formatBox.setBounds (fmtRow.removeFromLeft (160));
            fmtRow.removeFromLeft (8);
            bitsBox.setBounds (fmtRow);
            r.removeFromTop (6);
            nameField.setBounds (r.removeFromTop (24));
            r.removeFromTop (6);
            auto folderRow = r.removeFromTop (24);
            folderButton.setBounds (folderRow.removeFromLeft (90));
            folderRow.removeFromLeft (6);
            revealButton.setBounds (folderRow.removeFromLeft (80));
            r.removeFromTop (4);
            folderLabel.setBounds (r.removeFromTop (16));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (16, 16, 20));
            g.setColour (juce::Colour::fromRGB (30, 30, 36));
            g.fillRect (meterArea);
            const float db = juce::Decibels::gainToDecibels (rec.inputPeak(), -60.0f);
            const float frac = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
            auto fill = meterArea.toFloat().withWidth ((float) meterArea.getWidth() * frac);
            g.setColour (db > -1.0f ? juce::Colour::fromRGB (255, 80, 60)
                                    : juce::Colour::fromRGB (0, 200, 120));
            g.fillRect (fill);
        }

    private:
        void toggleRecord()
        {
            if (rec.isRecording())
                rec.stopRecording();
            else
                rec.startRecording();
            refresh();
        }

        void chooseFolder()
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose recording folder", rec.outputFolder());
            chooser->launchAsync (juce::FileBrowserComponent::openMode
                                      | juce::FileBrowserComponent::canSelectDirectories,
                [this] (const juce::FileChooser& fc) {
                    auto f = fc.getResult();
                    if (f != juce::File())
                    {
                        rec.setOutputFolder (f);
                        refresh();
                    }
                });
        }

        void timerCallback() override
        {
            if (rec.isRecording())
            {
                timeLabel.setText (formatTime (rec.recordedSeconds()), juce::dontSendNotification);
                sizeLabel.setText (juce::File::descriptionOfSizeInBytes (rec.currentFile().getSize()),
                                   juce::dontSendNotification);
            }
            repaint (meterArea);
            updateButton();
        }

        void refresh()
        {
            const bool r = rec.isRecording();
            updateButton();
            formatBox.setEnabled (! r);
            bitsBox.setEnabled (! r);
            nameField.setEnabled (! r);
            folderButton.setEnabled (! r);
            folderLabel.setText (rec.outputFolder().getFullPathName(), juce::dontSendNotification);
            if (! r)
            {
                timeLabel.setText ("00:00:00", juce::dontSendNotification);
                sizeLabel.setText ({}, juce::dontSendNotification);
            }
        }

        void updateButton()
        {
            const bool r = rec.isRecording();
            recordButton.setButtonText (r ? "Stop" : "Record");
            recordButton.setColour (juce::TextButton::buttonColourId,
                r ? juce::Colour::fromRGB (200, 40, 40) : juce::Colour::fromRGB (50, 50, 58));
            recordButton.setEnabled (r || rec.audioFlowing());
        }

        static juce::String formatTime (double seconds)
        {
            const int total = (int) seconds;
            return juce::String::formatted ("%02d:%02d:%02d", total / 3600, (total / 60) % 60, total % 60);
        }

        RecorderProcessor& rec;
        juce::TextButton recordButton { "Record" }, folderButton, revealButton;
        juce::Label timeLabel, sizeLabel, folderLabel;
        juce::ComboBox formatBox, bitsBox;
        juce::TextEditor nameField;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> formatAtt, bitsAtt;
        std::unique_ptr<juce::FileChooser> chooser;
        juce::Rectangle<int> meterArea;
    };
}
```

- [ ] **Step 2: Wire `createEditor()` out-of-line**

In `Source/DSP/Builtin/InternalPluginFormat.cpp`, add the include with the other editor includes (after `PpmMeterEditor.h`, line ~9):

```cpp
#include "DSP/Builtin/RecorderEditor.h"
```

And add the out-of-line definition with the other `createEditor` definitions (after the `PpmMeterProcessor::createEditor` line ~29):

```cpp
    juce::AudioProcessorEditor* RecorderProcessor::createEditor() { return new RecorderEditor (*this); }
```

- [ ] **Step 3: Configure + build the full app**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j 2>&1 | tail -25`
Expected: build succeeds, producing `build/dcorerouter_artefacts/Release/D-Router.app`. The `createEditor` link gap from Task 3 is now closed.

- [ ] **Step 4: Verify tests stay green**

Run: `cmake --build build --target dcorerouter_tests && ctest --test-dir build --output-on-failure`
Expected: PASS — both `dcorerouter_tests` and `dcorerouter_tests_juce` green.

- [ ] **Step 5: Format + commit**

```bash
clang-format -i Source/DSP/Builtin/RecorderEditor.h Source/DSP/Builtin/InternalPluginFormat.cpp
git add Source/DSP/Builtin/RecorderEditor.h Source/DSP/Builtin/InternalPluginFormat.cpp
git commit -m "feat(recorder): editor (record/stop, meter, format/bits, name, folder)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Full build, format check, and real-device verification

**Files:** none (verification only).

- [ ] **Step 1: Confirm clang-format is clean (CI gate)**

Run: `git diff --name-only HEAD~4 | grep -E '\.(h|cpp|mm)$' | xargs clang-format --dry-run --Werror 2>&1 | tail -20`
Expected: no output / exit 0. If anything is flagged, `clang-format -i` the file and amend the relevant commit.

- [ ] **Step 2: Clean configure + build + full ctest**

Run: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure`
Expected: app builds; `100% tests passed`.

- [ ] **Step 3: Launch the app**

Run: `./run.sh`
Expected: D-Router launches. "Recorder" appears in the **Built-in** section of any FX slot's load menu.

- [ ] **Step 4: Real-device verification checklist (USER — cannot be automated)**

These confirm behavior the headless tests can't. Report results honestly; do not claim "works" without observing each:

  1. **Stereo group, WAV 24-bit:** insert Recorder on a stereo output group with audio playing. Record button enables once audio is flowing. Press Record → timer counts up, file size grows, meter moves. Press Stop. Open the file in `~/Music/D-Router Recordings` — it is a **stereo** 24-bit WAV that plays back correctly.
  2. **Per-channel slot:** insert on a single channel slot → the resulting file is **2-channel with identical L/R** (the per-channel host feeds a duplicated-mono buffer), not a true mono file.
  3. **FLAC + AAC:** repeat (1) with Format = FLAC, then AAC. Files are `.flac` / `.m4a`, smaller than WAV, and play in QuickTime/Finder.
  4. **32-bit float WAV:** Format = WAV, Bit depth = 32-bit float → file reports 32-bit float and plays.
  5. **Name + folder:** change Name (e.g. "Vocals") → filename is `Vocals_<timestamp>.<ext>`. Choose a different folder → files land there; "Reveal" opens it.
  6. **Engine-stop finalization:** start recording, then trigger an engine reconfigure/stop (e.g. change a device) → the in-progress file is **finalized and plays** (not truncated/corrupt).
  7. **Snapshot persistence:** set format/bits/name/folder, save a project, reload → those settings restore; the Recorder loads **stopped**.
  8. **No audio glitch:** with the Recorder inserted and recording, confirm the monitored audio is unaffected (it is a pass-through tap).

- [ ] **Step 5: Update project memory + finish the branch**

After the user confirms real-device results, record what was/wasn't verified in the memory file, then use **superpowers:finishing-a-development-branch** to open the PR (squash-merge, per the repo workflow). Note any item from Step 4 the user could not verify.

---

## Notes & deviations from the spec

- **Collision handling** uses `juce::File::getNonexistentSibling` at the call site (JUCE, already tested) rather than a JUCE-free helper — so the unit tests cover extension/sanitize/filename only, not collision suffixing. (Spec §6 listed collision logic as a test item; this is a cleaner equivalent.)
- **Name prefix** comes from the editor's Name field, not auto-derived from the insert point (a plugin instance can't see its host slot without invasive plumbing) — as agreed during brainstorming.
- **AAC bitrate** is the codec default (~128 kbps); no bitrate control (YAGNI; not in the format/bit-depth params).
- **Record gating:** the editor enables Record only once `audioFlowing()` (the processor has seen ≥1 real block), guaranteeing `startRecording` reads the true *presented* channel count (per-channel host = 2 duplicated-mono, group = N) rather than the base class's `preparedChannels` floor of 2. The per-channel host presents a duplicated-mono stereo buffer, so per-channel recordings are 2-channel (identical L/R), not true mono.
```

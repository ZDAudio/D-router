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
    // Recorder -- a pass-through "tap" that records its insert point to disk at
    // the channel width the host presents.  An output group records its true N
    // channels (a stereo group -> stereo file, etc.).  NOTE: the per-channel
    // (mono) host wraps a mono signal in a 2-channel scratch with L == R (it
    // copies the mono input into every plugin channel), so a per-channel insert
    // yields a 2-channel file with identical L/R -- not a true mono file.
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
                juce::ParameterID { "format", 1 }, "Format", juce::StringArray { "WAV", "FLAC", "AAC (.m4a)" }, 0));
            l.add (std::make_unique<juce::AudioParameterChoice> (
                juce::ParameterID { "bits", 1 }, "Bit depth", juce::StringArray { "16-bit", "24-bit", "32-bit float" }, 1));
            return l;
        }

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
                now.getYear(),
                now.getMonth() + 1,
                now.getDayOfMonth(),
                now.getHours(),
                now.getMinutes(),
                now.getSeconds(),
                fmt);
            const juce::File file = dir.getChildFile (name).getNonexistentSibling();

            std::unique_ptr<juce::AudioFormatWriter> writer (makeWriter (file, sr, nch, fmt));
            if (writer == nullptr)
                return; // open failed -> stays stopped

            if (!backgroundThread.isThreadRunning())
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
            // finalized in releaseResources().  The recording width comes from
            // liveChannels (the real processDsp buffer width), not the base's
            // preparedChannels (floored to 2), so a group records its true count
            // and isn't mis-sized.  (A per-channel host still hands us a 2-ch
            // duplicated-mono buffer -- see the class header.)
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
                bits = (bitsChoice == 0) ? 16 : (bitsChoice == 1) ? 24
                                                                  : 32;
            }

            auto fos = std::make_unique<juce::FileOutputStream> (file);
            if (!fos->openedOk())
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

        juce::String folder; // empty -> defaultFolder()
        juce::String prefix { "Recording" }; // filename prefix
        juce::String currentPath; // last/active file
    };
} // namespace dcr::builtin

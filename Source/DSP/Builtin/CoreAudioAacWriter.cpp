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
                : juce::AudioFormatWriter (nullptr, "AAC", sr, (unsigned int) juce::jmin (numCh, kMaxChannels), 32)
            {
                usesFloatingPointData = true; // we feed the codec Float32 PCM

                // Clamp the declared channel count to what write() can actually
                // feed (kMaxChannels), so the file never claims more channels than
                // we encode.  Router group buses are tiny; >32 never happens here.
                const int n = juce::jmin (numCh, kMaxChannels);

                AudioStreamBasicDescription out {};
                out.mFormatID = kAudioFormatMPEG4AAC;
                out.mSampleRate = sr;
                out.mChannelsPerFrame = (UInt32) n; // codec fills packet sizes

                CFStringRef cfPath = file.getFullPathName().toCFString();
                CFURLRef url = CFURLCreateWithFileSystemPath (kCFAllocatorDefault, cfPath, kCFURLPOSIXPathStyle, false);
                CFRelease (cfPath);
                if (url == nullptr)
                    return;

                OSStatus st = ExtAudioFileCreateWithURL (url, kAudioFileM4AType, &out, nullptr, kAudioFileFlags_EraseFile, &extFile);
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
                client.mChannelsPerFrame = (UInt32) n;
                client.mBitsPerChannel = 32;
                client.mFramesPerPacket = 1;
                client.mBytesPerFrame = 4; // one float per (non-interleaved) channel
                client.mBytesPerPacket = 4;

                st = ExtAudioFileSetProperty (extFile, kExtAudioFileProperty_ClientDataFormat, sizeof (client), &client);
                if (st != noErr)
                {
                    ExtAudioFileDispose (extFile);
                    extFile = nullptr;
                    return;
                }

                channels = n;
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
                if (!opened || extFile == nullptr || numSamples <= 0)
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
        if (!w->isOpen())
            return nullptr;
        return w;
    }
}

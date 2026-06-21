#include "SampleRateConverter.h"

#include <algorithm>
#include <cstring>

namespace dcr
{

    bool SampleRateConverter::prepare (double inputSampleRate,
        double outputSampleRate,
        unsigned int quality,
        unsigned int complexity)
    {
        reset();
        inRate = inputSampleRate;
        outRate = outputSampleRate;
        bypass = (std::abs (inputSampleRate - outputSampleRate) < 1.0e-6);

        if (bypass)
            return true;

        AudioStreamBasicDescription inFmt {};
        inFmt.mSampleRate = inputSampleRate;
        inFmt.mFormatID = kAudioFormatLinearPCM;
        inFmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
        inFmt.mFramesPerPacket = 1;
        inFmt.mChannelsPerFrame = 1;
        inFmt.mBitsPerChannel = 32;
        inFmt.mBytesPerPacket = 4;
        inFmt.mBytesPerFrame = 4;

        AudioStreamBasicDescription outFmt = inFmt;
        outFmt.mSampleRate = outputSampleRate;

        OSStatus s = AudioConverterNew (&inFmt, &outFmt, &converter);
        if (s != noErr || converter == nullptr)
            return false;

        {
            UInt32 q = quality;
            AudioConverterSetProperty (converter,
                kAudioConverterSampleRateConverterQuality,
                sizeof (q),
                &q);
        }
        {
            UInt32 c = complexity;
            AudioConverterSetProperty (converter,
                kAudioConverterSampleRateConverterComplexity,
                sizeof (c),
                &c);
        }
        {
            UInt32 prime = kConverterPrimeMethod_None;
            AudioConverterSetProperty (converter,
                kAudioConverterPrimeMethod,
                sizeof (prime),
                &prime);
        }
        return true;
    }

    void SampleRateConverter::reset()
    {
        if (converter != nullptr)
        {
            AudioConverterDispose (converter);
            converter = nullptr;
        }
        pending.clear();
        pendingPos = 0;
        bypass = false;
    }

    void SampleRateConverter::pushInput (const float* samples, int numSamples)
    {
        if (numSamples <= 0)
            return;

        // Compact buffer if half-consumed.
        if (pendingPos > 0 && pendingPos >= pending.size() / 2)
        {
            pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) pendingPos);
            pendingPos = 0;
        }
        pending.insert (pending.end(), samples, samples + numSamples);
    }

    OSStatus SampleRateConverter::inputProc (AudioConverterRef,
        UInt32* ioNumberDataPackets,
        AudioBufferList* ioData,
        AudioStreamPacketDescription**,
        void* inUserData)
    {
        auto* self = static_cast<SampleRateConverter*> (inUserData);
        const UInt32 requested = *ioNumberDataPackets;
        const size_t available = self->pending.size() - self->pendingPos;
        const UInt32 toGive = (UInt32) std::min ((size_t) requested, available);

        if (toGive == 0)
        {
            ioData->mBuffers[0].mData = nullptr;
            ioData->mBuffers[0].mDataByteSize = 0;
            *ioNumberDataPackets = 0;
            return 1; // any non-zero -> stop the conversion cycle; we'll resume next pull
        }

        // Copy to scratch so AudioConverter has a stable pointer for this call.
        self->scratch.assign (self->pending.begin() + (std::ptrdiff_t) self->pendingPos,
            self->pending.begin() + (std::ptrdiff_t) self->pendingPos + toGive);
        self->pendingPos += toGive;

        ioData->mBuffers[0].mData = self->scratch.data();
        ioData->mBuffers[0].mDataByteSize = toGive * (UInt32) sizeof (float);
        ioData->mBuffers[0].mNumberChannels = 1;
        *ioNumberDataPackets = toGive;
        return noErr;
    }

    int SampleRateConverter::getOutputLatencySamples() const
    {
        if (bypass || converter == nullptr)
            return 0;
        AudioConverterPrimeInfo info {};
        UInt32 size = sizeof (info);
        if (AudioConverterGetProperty (converter,
                kAudioConverterPrimeInfo,
                &size,
                &info)
            != noErr)
            return 0;
        return (int) info.leadingFrames;
    }

    int SampleRateConverter::pullOutput (float* dst, int numSamples)
    {
        if (numSamples <= 0)
            return 0;

        if (bypass)
        {
            const size_t avail = pending.size() - pendingPos;
            const int toCopy = (int) std::min ((size_t) numSamples, avail);
            if (toCopy > 0)
            {
                std::memcpy (dst, pending.data() + pendingPos, (size_t) toCopy * sizeof (float));
                pendingPos += (size_t) toCopy;
            }
            return toCopy;
        }

        if (converter == nullptr)
            return 0;

        AudioBufferList abl {};
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 1;
        abl.mBuffers[0].mDataByteSize = (UInt32) numSamples * (UInt32) sizeof (float);
        abl.mBuffers[0].mData = dst;

        UInt32 outPackets = (UInt32) numSamples;
        OSStatus s = AudioConverterFillComplexBuffer (converter, &inputProc, this, &outPackets, &abl, nullptr);
        if (s != noErr && s != 1)
            return 0;
        return (int) outPackets;
    }

} // namespace dcr

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

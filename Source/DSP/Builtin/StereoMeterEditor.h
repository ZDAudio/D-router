#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace dcr::builtin
{
    class StereoMeterProcessor;

    // Editor for the Stereo 3D Pan Scatter meter. Hosts a Metal MTKView (via
    // juce::NSViewComponent) that renders the 3D scatter; it drains the
    // processor's L/R rings on a timer, runs the FFT + analysis, and feeds the
    // renderer. Metal lives behind a pimpl in the .mm so this header is plain C++
    // (InternalPluginFormat.cpp's out-of-line createEditor includes it).
    class StereoMeterEditor : public juce::AudioProcessorEditor
    {
    public:
        explicit StereoMeterEditor (StereoMeterProcessor& p);
        ~StereoMeterEditor() override;

        void resized() override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

} // namespace dcr::builtin

#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"
#include "Engine/RingBuffer.h" // dcr::FloatRingBuffer
#include "Persistence/AtomicXmlWrite.h"

namespace dcr::builtin
{

    // ===========================================================================
    // Stereo 3D Pan Scatter -- a pass-through visualization meter. It modifies no
    // audio; it taps the (stereo) group's L/R into two lock-free SPSC rings that
    // its editor drains on the UI thread, where the FFT + 3D Metal render happen.
    // Surfaced only via the unlock-gated "Stereo Meter" item on stereo output
    // groups, so it is intentionally NOT in getBuiltinDescriptions().
    // ===========================================================================
    class StereoMeterProcessor : public BuiltinProcessor
    {
    public:
        StereoMeterProcessor() : BuiltinProcessor (ids::stereo_meter, "Stereo Meter", createLayout())
        {
            // Capture pristine defaults BEFORE any user-default is applied, so Reset
            // can always restore them.
            factoryState = apvts.copyState();
            loadUserDefault();
        }

        static APVTS::ParameterLayout createLayout()
        {
            using P = juce::AudioParameterFloat;
            using R = juce::NormalisableRange<float>;
            using A = juce::AudioParameterFloatAttributes;
            APVTS::ParameterLayout l;
            l.add (std::make_unique<P> (juce::ParameterID { "floorDb", 1 }, "Floor", R (-90.0f, -20.0f, 0.5f), -60.0f, A().withLabel ("dB")));
            l.add (std::make_unique<P> (juce::ParameterID { "ceilDb", 1 }, "Ceiling", R (-40.0f, 0.0f, 0.5f), 0.0f, A().withLabel ("dB")));
            // High-frequency tilt strength. Reshaped by StereoMeterMath::highLiftGain:
            // ~no lift at/below `liftPivot`, rising toward Nyquist (true highs only).
            l.add (std::make_unique<P> (juce::ParameterID { "highLift", 1 }, "High lift", R (0.0f, 1.0f, 0.01f), 0.5f));
            {
                auto pivotRange = R (200.0f, 8000.0f, 1.0f);
                pivotRange.setSkewForCentre (1500.0f);
                l.add (std::make_unique<P> (juce::ParameterID { "liftPivot", 1 }, "Lift pivot", pivotRange, 2000.0f, A().withLabel ("Hz")));
            }
            // Knee smoothing for the HF tilt: 0 = hard corner at the pivot,
            // higher rounds it (StereoMeterMath::highLiftGain softplus).
            l.add (std::make_unique<P> (juce::ParameterID { "liftKnee", 1 }, "Smooth", R (0.0f, 1.0f, 0.01f), 0.3f));
            l.add (std::make_unique<P> (juce::ParameterID { "pointMin", 1 }, "Min size", R (1.0f, 16.0f, 0.5f), 4.0f));
            l.add (std::make_unique<P> (juce::ParameterID { "pointMax", 1 }, "Max size", R (8.0f, 60.0f, 0.5f), 30.0f));
            l.add (std::make_unique<P> (juce::ParameterID { "heightScale", 1 }, "Height", R (0.3f, 2.5f, 0.01f), 1.0f));
            l.add (std::make_unique<P> (juce::ParameterID { "smooth", 1 }, "Smooth", R (0.05f, 1.0f, 0.01f), 0.5f));
            l.add (std::make_unique<P> (juce::ParameterID { "colorSat", 1 }, "Colour", R (0.0f, 1.0f, 0.01f), 1.0f));
            l.add (std::make_unique<P> (juce::ParameterID { "axisOpacity", 1 }, "Axis opacity", R (0.0f, 1.0f, 0.01f), 0.85f));
            l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "trailDepth", 1 }, "Trail", 1, 30, 10));
            l.add (std::make_unique<P> (juce::ParameterID { "trailDecay", 1 }, "Trail fade", R (0.50f, 0.97f, 0.01f), 0.80f));
            l.add (std::make_unique<P> (juce::ParameterID { "stemAmount", 1 }, "Stems", R (0.0f, 1.0f, 0.01f), 0.0f));
            // View mode: 0 = 3D orbit (default), 1 = Front (pan x freq, flat),
            // 2 = RTA (freq x level, parallel projection).
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "viewMode", 1 }, "View", juce::StringArray { "3D", "Front", "RTA" }, 0));
            return l;
        }

        // ----- user default (Save / Reset) -------------------------------------
        // The user-default is applied in the CONSTRUCTOR only, so a project
        // snapshot's setStateInformation() still overrides it on restore, while a
        // freshly added meter opens with the saved settings.
        static juce::File defaultsFile()
        {
            auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                           .getChildFile ("dcorerouter");
            dir.createDirectory();
            return dir.getChildFile ("StereoMeterDefaults.xml");
        }

        void saveUserDefault()
        {
            if (auto xml = apvts.copyState().createXml())
                dcr::writeXmlAtomically (*xml, defaultsFile());
        }

        void resetToFactory()
        {
            apvts.replaceState (factoryState.createCopy());
        }

        juce::AudioProcessorEditor* createEditor() override; // StereoMeterEditor.h

        // Drained by the editor on the UI thread (SPSC: audio writes, UI reads).
        dcr::FloatRingBuffer& ringL() noexcept { return meterL; }
        dcr::FloatRingBuffer& ringR() noexcept { return meterR; }
        double meterSampleRate() const noexcept { return dspSampleRate; }

    protected:
        void prepareDsp (double sr, int, int) override
        {
            dspSampleRate = sr;
        }

        void processDsp (juce::AudioBuffer<float>& buffer) override
        {
            const int ns = buffer.getNumSamples();
            const int nch = buffer.getNumChannels();
            if (nch <= 0)
                return;
            // Tap L (ch0) and R (ch1, or mono fallback). Lock-free, no alloc.
            meterL.write (buffer.getReadPointer (0), (size_t) ns);
            meterR.write (buffer.getReadPointer (nch >= 2 ? 1 : 0), (size_t) ns);
            // Audio passes through unchanged (this is a meter).
        }

    private:
        void loadUserDefault()
        {
            auto f = defaultsFile();
            if (!f.existsAsFile())
                return;
            if (auto xml = juce::parseXML (f))
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
        }

        // Power-of-two; ~0.68 s at 48k. The editor drains everything available
        // each tick and keeps only the most recent window, so this is just slack.
        dcr::FloatRingBuffer meterL { 32768 };
        dcr::FloatRingBuffer meterR { 32768 };
        juce::ValueTree factoryState;
    };

} // namespace dcr::builtin

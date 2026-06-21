#pragma once

#include "DSP/Builtin/PpmMeterProcessor.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{

    // Vertical PPM ladders (one per channel) with a dBFS scale on the left and a
    // dBu scale (derived from the alignment level) on the right, a moving
    // quasi-peak bar and a peak-hold tick.
    class PpmMeterEditor : public juce::AudioProcessorEditor,
                           private juce::Timer
    {
    public:
        explicit PpmMeterEditor (PpmMeterProcessor& p)
            : juce::AudioProcessorEditor (p), meter (p)
        {
            generic = std::make_unique<juce::GenericAudioProcessorEditor> (meter);
            addAndMakeVisible (*generic);
            setSize (360, 420);
            startTimerHz (30);
        }
        ~PpmMeterEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            meterArea = r.removeFromTop (getHeight() - genericH);
            generic->setBounds (r.reduced (4));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (14, 14, 18));
            drawScaleAndMeters (g);
        }

    private:
        void timerCallback() override { repaint (meterArea); }

        void drawScaleAndMeters (juce::Graphics& g)
        {
            auto area = meterArea.reduced (8);
            const float lo = -60.0f, hi = 0.0f; // dBFS range
            const float align = meter.getAlignDbFs(); // dBFS at 0 dBu

            auto leftScale = area.removeFromLeft (34);
            auto rightScale = area.removeFromRight (34);
            auto bars = area.reduced (6, 4);

            auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (lo, hi, db),
                                             lo,
                                             hi,
                                             (float) bars.getBottom(),
                                             (float) bars.getY()); };

            // Grid + dBFS (left) and dBu (right) labels.
            g.setFont (juce::FontOptions (9.0f));
            for (float db = lo; db <= hi + 0.1f; db += 6.0f)
            {
                const int y = (int) yFor (db);
                g.setColour (juce::Colour::fromRGB (38, 38, 46));
                g.drawHorizontalLine (y, (float) bars.getX(), (float) bars.getRight());
                g.setColour (juce::Colour::fromRGB (110, 110, 120));
                g.drawText (juce::String ((int) db), leftScale.getX(), y - 6, leftScale.getWidth() - 2, 12, juce::Justification::centredRight);
                g.setColour (juce::Colour::fromRGB (90, 110, 150));
                g.drawText (juce::String ((int) (db - align)), rightScale.getX() + 2, y - 6, rightScale.getWidth() - 2, 12, juce::Justification::centredLeft);
            }
            g.setColour (juce::Colour::fromRGB (70, 70, 82));
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.drawText ("dBFS", leftScale.getX(), bars.getY() - 12, leftScale.getWidth(), 11, juce::Justification::centred);
            g.drawText ("dBu", rightScale.getX(), bars.getY() - 12, rightScale.getWidth(), 11, juce::Justification::centred);

            // Alignment reference line (0 dBu).
            const int alignY = (int) yFor (align);
            g.setColour (juce::Colour::fromRGB (90, 110, 150).withAlpha (0.6f));
            g.drawHorizontalLine (alignY, (float) bars.getX(), (float) bars.getRight());

            // One bar per channel.
            const int n = meter.getMeterChannels();
            if (n <= 0)
                return;
            const float bw = (float) bars.getWidth() / (float) n;
            for (int ch = 0; ch < n; ++ch)
            {
                const float qp = meter.getQuasiPeakDb (ch);
                const float hold = meter.getPeakHoldDb (ch);
                juce::Rectangle<float> col ((float) bars.getX() + ch * bw + 2.0f, (float) bars.getY(), bw - 4.0f, (float) bars.getHeight());

                g.setColour (juce::Colour::fromRGB (22, 22, 28));
                g.fillRect (col);

                const float topY = yFor (qp);
                auto fill = col.withTop (topY);
                // green below align, amber within ~6 dB of 0, red near full scale.
                juce::Colour c = qp > -1.0f   ? juce::Colour::fromRGB (255, 80, 60)
                                 : qp > align ? juce::Colour::fromRGB (255, 200, 60)
                                              : juce::Colour::fromRGB (0, 220, 130);
                g.setColour (c);
                g.fillRect (fill);

                // peak-hold tick
                const float hy = yFor (hold);
                g.setColour (juce::Colours::white.withAlpha (0.9f));
                g.fillRect (col.getX(), hy - 1.0f, col.getWidth(), 2.0f);

                g.setColour (juce::Colour::fromRGB (60, 60, 70));
                g.drawRect (col, 1.0f);

                // numeric readout
                g.setColour (juce::Colour::fromRGB (170, 170, 180));
                g.setFont (juce::FontOptions (9.0f));
                g.drawText (juce::String (hold, 1), (int) col.getX() - 2, bars.getBottom() + 1, (int) col.getWidth() + 4, 12, juce::Justification::centred);
            }
        }

        static constexpr int genericH = 84;

        PpmMeterProcessor& meter;
        std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
        juce::Rectangle<int> meterArea;
    };

} // namespace dcr::builtin

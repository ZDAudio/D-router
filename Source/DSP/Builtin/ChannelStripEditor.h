#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{

    // Channel Strip editor: a combined EQ response curve + compressor GR meter on
    // top, and the full grouped parameter list (HP / Gate / EQ / Comp / Output,
    // rendered with section headers by GenericAudioProcessorEditor) scrolling
    // below.  Drag an EQ band node on the curve to set freq (x) / gain (y).
    class ChannelStripEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
    {
    public:
        explicit ChannelStripEditor (ChannelStripProcessor& proc)
            : juce::AudioProcessorEditor (proc), strip (proc)
        {
            generic = std::make_unique<juce::GenericAudioProcessorEditor> (strip);
            viewport.setViewedComponent (generic.get(), false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);
            setSize (560, 620);
            startTimerHz (20);
        }

        ~ChannelStripEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            topArea = r.removeFromTop (topHeight);
            meterArea = topArea.removeFromRight (54).reduced (6);
            curveArea = topArea.reduced (8);
            viewport.setBounds (r);
            generic->setSize (r.getWidth() - 10, juce::jmax (generic->getHeight(), (2 + 3 + ChannelStripProcessor::kEqBands * 5 + 6 + 1) * 26 + 60));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (14, 14, 18));
            drawCurve (g);
            drawMeter (g);
        }

        void mouseDown (const juce::MouseEvent& e) override { dragBand = nodeHitTest (e.position); }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragBand < 0)
                return;
            setParam (ChannelStripProcessor::eqId (dragBand, "freq"),
                juce::jlimit (20.0f, 20000.0f, freqForX (juce::jlimit ((float) curveArea.getX(), (float) curveArea.getRight(), e.position.x))));
            const int type = (int) getParam (ChannelStripProcessor::eqId (dragBand, "type"));
            if (type <= 2)
                setParam (ChannelStripProcessor::eqId (dragBand, "gain"),
                    juce::jlimit (-18.0f, 18.0f, gainForY (juce::jlimit ((float) curveArea.getY(), (float) curveArea.getBottom(), e.position.y))));
            setParam (ChannelStripProcessor::eqId (dragBand, "on"), 1.0f);
            repaint (topArea);
        }

    private:
        void timerCallback() override { repaint (topArea); }

        float getParam (const juce::String& id) const
        {
            if (auto* a = strip.getValueTreeState().getRawParameterValue (id))
                return a->load();
            return 0.0f;
        }
        void setParam (const juce::String& id, float v)
        {
            if (auto* p = strip.getValueTreeState().getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (v));
        }

        float xForFreq (float f) const
        {
            return juce::jmap (std::log10 (juce::jlimit (20.0f, 20000.0f, f)),
                std::log10 (20.0f),
                std::log10 (20000.0f),
                (float) curveArea.getX(),
                (float) curveArea.getRight());
        }
        float freqForX (float x) const
        {
            return std::pow (10.0f, juce::jmap (x, (float) curveArea.getX(), (float) curveArea.getRight(), std::log10 (20.0f), std::log10 (20000.0f)));
        }
        float yForGain (float db) const
        {
            return juce::jmap (juce::jlimit (-kSpan, kSpan, db), kSpan, -kSpan, (float) curveArea.getY(), (float) curveArea.getBottom());
        }
        float gainForY (float y) const
        {
            return juce::jmap (y, (float) curveArea.getY(), (float) curveArea.getBottom(), kSpan, -kSpan);
        }

        double magnitudeDbAt (double freq) const
        {
            const double sr = juce::jmax (8000.0, strip.getSampleRate());
            double mag = 1.0;
            for (int b = 0; b < ChannelStripProcessor::kEqBands; ++b)
                if (getParam (ChannelStripProcessor::eqId (b, "on")) > 0.5f)
                {
                    const int t = (int) getParam (ChannelStripProcessor::eqId (b, "type"));
                    const float f = getParam (ChannelStripProcessor::eqId (b, "freq"));
                    const float gn = getParam (ChannelStripProcessor::eqId (b, "gain"));
                    const float q = juce::jlimit (0.1f, 10.0f, getParam (ChannelStripProcessor::eqId (b, "q")));
                    if (auto c = makeEqBandCoeffs (t, sr, f, q, gn))
                        mag *= c->getMagnitudeForFrequency (freq, sr);
                }
            return juce::Decibels::gainToDecibels (mag, -60.0);
        }

        juce::Point<float> nodePos (int b) const
        {
            const float f = getParam (ChannelStripProcessor::eqId (b, "freq"));
            const int t = (int) getParam (ChannelStripProcessor::eqId (b, "type"));
            const float g = (t <= 2) ? getParam (ChannelStripProcessor::eqId (b, "gain")) : 0.0f;
            return { xForFreq (f), yForGain (g) };
        }
        int nodeHitTest (juce::Point<float> p) const
        {
            for (int b = 0; b < ChannelStripProcessor::kEqBands; ++b)
                if (nodePos (b).getDistanceFrom (p) < 12.0f)
                    return b;
            return -1;
        }

        void drawCurve (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (curveArea);
            g.setColour (juce::Colour::fromRGB (40, 40, 48));
            for (float db = -kSpan; db <= kSpan + 0.1f; db += 6.0f)
                g.drawHorizontalLine ((int) yForGain (db), (float) curveArea.getX(), (float) curveArea.getRight());
            for (float f : { 50.0f, 100.0f, 500.0f, 1000.0f, 5000.0f, 10000.0f })
            {
                const float x = xForFreq (f);
                g.drawVerticalLine ((int) x, (float) curveArea.getY(), (float) curveArea.getBottom());
                g.setColour (juce::Colour::fromRGB (90, 90, 100));
                g.setFont (juce::FontOptions (10.0f));
                g.drawText (f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String ((int) f),
                    (int) x + 2,
                    curveArea.getBottom() - 13,
                    36,
                    12,
                    juce::Justification::topLeft);
                g.setColour (juce::Colour::fromRGB (40, 40, 48));
            }
            g.setColour (juce::Colour::fromRGB (70, 70, 82));
            g.drawHorizontalLine ((int) yForGain (0.0f), (float) curveArea.getX(), (float) curveArea.getRight());

            juce::Path path;
            for (int x = curveArea.getX(); x <= curveArea.getRight(); ++x)
            {
                const float y = yForGain ((float) magnitudeDbAt (freqForX ((float) x)));
                if (x == curveArea.getX())
                    path.startNewSubPath ((float) x, y);
                else
                    path.lineTo ((float) x, y);
            }
            g.setColour (juce::Colour::fromRGB (0, 255, 210));
            g.strokePath (path, juce::PathStrokeType (2.0f));

            for (int b = 0; b < ChannelStripProcessor::kEqBands; ++b)
            {
                const bool on = getParam (ChannelStripProcessor::eqId (b, "on")) > 0.5f;
                const auto p = nodePos (b);
                g.setColour (on ? juce::Colour::fromRGB (255, 180, 60) : juce::Colour::fromRGB (90, 90, 100));
                g.fillEllipse (p.x - 5.0f, p.y - 5.0f, 10.0f, 10.0f);
                g.setColour (juce::Colours::black);
                g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                g.drawText (juce::String (b + 1), (int) p.x - 5, (int) p.y - 6, 10, 12, juce::Justification::centred);
            }
            g.setColour (juce::Colour::fromRGB (130, 130, 140));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("EQ", curveArea.getX() + 3, curveArea.getY() + 2, 40, 12, juce::Justification::topLeft);
        }

        void drawMeter (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (meterArea);
            const float gr = juce::jlimit (-24.0f, 0.0f, strip.getCompGainReductionDb());
            auto bar = meterArea.toFloat();
            g.setColour (juce::Colour::fromRGB (255, 90, 60));
            g.fillRect (bar.withHeight (bar.getHeight() * (-gr / 24.0f)));
            g.setColour (juce::Colour::fromRGB (60, 60, 70));
            g.drawRect (meterArea, 1);
            g.setColour (juce::Colour::fromRGB (150, 150, 160));
            g.setFont (juce::FontOptions (9.0f));
            g.drawText ("Comp GR", meterArea.getX() - 8, meterArea.getBottom() + 1, meterArea.getWidth() + 16, 12, juce::Justification::centred);
        }

        static constexpr float kSpan = 18.0f;
        static constexpr int topHeight = 200;

        ChannelStripProcessor& strip;
        std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
        juce::Viewport viewport;
        juce::Rectangle<int> topArea, curveArea, meterArea;
        int dragBand = -1;
    };

} // namespace dcr::builtin

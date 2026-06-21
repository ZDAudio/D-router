#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{

    // Custom editor for ParametricEQ: a draggable frequency-response curve on top
    // (drag a band node horizontally = frequency, vertically = gain; scroll over a
    // node = Q), with the full GenericAudioProcessorEditor parameter list scrolling
    // below.  Reads/writes the processor's APVTS so the curve and the generic
    // sliders stay in sync both ways.
    class ParametricEqEditor : public juce::AudioProcessorEditor,
                               private juce::Timer
    {
    public:
        explicit ParametricEqEditor (ParametricEQ& proc)
            : juce::AudioProcessorEditor (proc), eq (proc)
        {
            generic = std::make_unique<juce::GenericAudioProcessorEditor> (eq);
            viewport.setViewedComponent (generic.get(), false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            setSize (560, 540);
            startTimerHz (20); // keep the curve in sync when sliders move
        }

        ~ParametricEqEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            curveArea = r.removeFromTop (curveHeight);
            viewport.setBounds (r);
            // Generic editor: full width (minus scrollbar), natural tall height.
            const int w = r.getWidth() - 10;
            const int h = juce::jmax (generic->getHeight(), ParametricEQ::kNumBands * 5 * 26 + 30);
            generic->setSize (w, h);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (14, 14, 18));
            drawCurve (g);
        }

        // ----- interaction ------------------------------------------------------
        void mouseDown (const juce::MouseEvent& e) override
        {
            dragBand = nodeHitTest (e.position);
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (dragBand < 0)
                return;
            const float f = freqForX (juce::jlimit ((float) curveArea.getX(),
                (float) curveArea.getRight(),
                e.position.x));
            setParam (ParametricEQ::paramId (dragBand, "freq"),
                juce::jlimit (20.0f, 20000.0f, f));

            // Vertical drag = gain (bell / shelves only; pass filters ignore gain).
            const int type = (int) getParam (ParametricEQ::paramId (dragBand, "type"));
            if (type <= 2)
            {
                const float gainDb = gainForY (juce::jlimit ((float) curveArea.getY(),
                    (float) curveArea.getBottom(),
                    e.position.y));
                setParam (ParametricEQ::paramId (dragBand, "gain"),
                    juce::jlimit (-24.0f, 24.0f, gainDb));
            }
            // Dragging a band implicitly enables it.
            setParam (ParametricEQ::paramId (dragBand, "on"), 1.0f);
            repaint();
        }

        void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
        {
            const int b = nodeHitTest (e.position);
            if (b < 0)
            {
                viewport.mouseWheelMove (e, w);
                return;
            }
            const auto qid = ParametricEQ::paramId (b, "q");
            const float q = juce::jlimit (0.1f, 10.0f, getParam (qid) * (1.0f + w.deltaY * 0.5f));
            setParam (qid, q);
            repaint();
        }

    private:
        void timerCallback() override { repaint (curveArea); }

        // ---- param helpers -----------------------------------------------------
        float getParam (const juce::String& id) const
        {
            if (auto* a = eq.getValueTreeState().getRawParameterValue (id))
                return a->load();
            return 0.0f;
        }
        void setParam (const juce::String& id, float value)
        {
            if (auto* p = eq.getValueTreeState().getParameter (id))
                p->setValueNotifyingHost (p->convertTo0to1 (value));
        }

        // ---- coordinate mapping ------------------------------------------------
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
            return juce::jmap (juce::jlimit (-kGainSpan, kGainSpan, db), kGainSpan, -kGainSpan, (float) curveArea.getY(), (float) curveArea.getBottom());
        }
        float gainForY (float y) const
        {
            return juce::jmap (y, (float) curveArea.getY(), (float) curveArea.getBottom(), kGainSpan, -kGainSpan);
        }

        // ---- DSP magnitude (mirrors ParametricEQ::updateBandIfNeeded) ----------
        juce::dsp::IIR::Coefficients<float>::Ptr bandCoeffs (int b) const
        {
            const double sr = juce::jmax (8000.0, eq.getSampleRate());
            const int type = (int) getParam (ParametricEQ::paramId (b, "type"));
            const float freq = juce::jlimit (20.0f, 20000.0f, getParam (ParametricEQ::paramId (b, "freq")));
            const float gain = getParam (ParametricEQ::paramId (b, "gain"));
            const float q = juce::jlimit (0.1f, 10.0f, getParam (ParametricEQ::paramId (b, "q")));
            const float gainLin = std::pow (10.0f, gain * 0.05f);
            switch (type)
            {
                case 1:
                    return juce::dsp::IIR::Coefficients<float>::makeLowShelf (sr, freq, q, gainLin);
                case 2:
                    return juce::dsp::IIR::Coefficients<float>::makeHighShelf (sr, freq, q, gainLin);
                case 3:
                    return juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, freq, q);
                case 4:
                    return juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, freq, q);
                default:
                    return juce::dsp::IIR::Coefficients<float>::makePeakFilter (sr, freq, q, gainLin);
            }
        }

        double magnitudeDbAt (double freq) const
        {
            const double sr = juce::jmax (8000.0, eq.getSampleRate());
            double mag = 1.0;
            for (int b = 0; b < ParametricEQ::kNumBands; ++b)
                if (getParam (ParametricEQ::paramId (b, "on")) > 0.5f)
                    if (auto c = bandCoeffs (b))
                        mag *= c->getMagnitudeForFrequency (freq, sr);
            return juce::Decibels::gainToDecibels (mag, -60.0);
        }

        int nodeHitTest (juce::Point<float> p) const
        {
            for (int b = 0; b < ParametricEQ::kNumBands; ++b)
            {
                const auto pos = nodePos (b);
                if (pos.getDistanceFrom (p) < 12.0f)
                    return b;
            }
            return -1;
        }

        juce::Point<float> nodePos (int b) const
        {
            const float f = getParam (ParametricEQ::paramId (b, "freq"));
            const int type = (int) getParam (ParametricEQ::paramId (b, "type"));
            const float g = (type <= 2) ? getParam (ParametricEQ::paramId (b, "gain")) : 0.0f;
            return { xForFreq (f), yForGain (g) };
        }

        void drawCurve (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (curveArea);

            // Grid: gain lines.
            g.setColour (juce::Colour::fromRGB (40, 40, 48));
            for (float db = -kGainSpan; db <= kGainSpan + 0.1f; db += 12.0f)
            {
                const float y = yForGain (db);
                g.drawHorizontalLine ((int) y, (float) curveArea.getX(), (float) curveArea.getRight());
                g.setColour (juce::Colour::fromRGB (90, 90, 100));
                g.setFont (juce::FontOptions (10.0f));
                g.drawText (juce::String ((int) db) + " dB",
                    curveArea.getX() + 2,
                    (int) y - 12,
                    44,
                    12,
                    juce::Justification::topLeft);
                g.setColour (juce::Colour::fromRGB (40, 40, 48));
            }
            // Grid: freq lines.
            for (float f : { 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f })
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

            // 0 dB line bolder.
            g.setColour (juce::Colour::fromRGB (70, 70, 82));
            g.drawHorizontalLine ((int) yForGain (0.0f), (float) curveArea.getX(), (float) curveArea.getRight());

            // Response curve.
            juce::Path path;
            const int x0 = curveArea.getX(), x1 = curveArea.getRight();
            for (int x = x0; x <= x1; ++x)
            {
                const double freq = freqForX ((float) x);
                const float y = yForGain ((float) magnitudeDbAt (freq));
                if (x == x0)
                    path.startNewSubPath ((float) x, y);
                else
                    path.lineTo ((float) x, y);
            }
            g.setColour (juce::Colour::fromRGB (0, 255, 210));
            g.strokePath (path, juce::PathStrokeType (2.0f));

            // Band nodes.
            for (int b = 0; b < ParametricEQ::kNumBands; ++b)
            {
                const bool on = getParam (ParametricEQ::paramId (b, "on")) > 0.5f;
                const auto pos = nodePos (b);
                g.setColour (on ? juce::Colour::fromRGB (255, 180, 60)
                                : juce::Colour::fromRGB (90, 90, 100));
                g.fillEllipse (pos.x - 5.0f, pos.y - 5.0f, 10.0f, 10.0f);
                g.setColour (juce::Colours::black);
                g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
                g.drawText (juce::String (b + 1), (int) pos.x - 5, (int) pos.y - 6, 10, 12, juce::Justification::centred);
            }
        }

        static constexpr float kGainSpan = 24.0f;
        static constexpr int curveHeight = 200;

        ParametricEQ& eq;
        std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
        juce::Viewport viewport;
        juce::Rectangle<int> curveArea;
        int dragBand = -1;
    };

} // namespace dcr::builtin

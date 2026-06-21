#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{

    // Custom editor for the Compressor: a static transfer curve (input dB ->
    // output dB, with threshold / ratio / knee) plus a live gain-reduction meter,
    // over the full generic parameter list.
    class CompressorEditor : public juce::AudioProcessorEditor,
                             private juce::Timer
    {
    public:
        explicit CompressorEditor (CompressorProcessor& proc)
            : juce::AudioProcessorEditor (proc), comp (proc)
        {
            generic = std::make_unique<juce::GenericAudioProcessorEditor> (comp);
            viewport.setViewedComponent (generic.get(), false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            setSize (480, 460);
            startTimerHz (24);
        }

        ~CompressorEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            topArea = r.removeFromTop (topHeight);
            meterArea = topArea.removeFromRight (54).reduced (6);
            curveArea = topArea.reduced (8);
            viewport.setBounds (r);
            generic->setSize (r.getWidth() - 10, juce::jmax (generic->getHeight(), 6 * 26 + 30));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (14, 14, 18));
            drawTransfer (g);
            drawMeter (g);
        }

    private:
        void timerCallback() override { repaint (meterArea.expanded (2)); }

        float getParam (const juce::String& id) const
        {
            if (auto* a = comp.getValueTreeState().getRawParameterValue (id))
                return a->load();
            return 0.0f;
        }

        // input dB -> output dB through the same soft-knee curve the DSP uses.
        float outputDb (float inDb) const
        {
            const float threshold = getParam ("threshold");
            const float ratio = juce::jmax (1.0f, getParam ("ratio"));
            const float knee = juce::jmax (0.0f, getParam ("knee"));
            const float makeup = getParam ("makeup");
            const float over = inDb - threshold;
            float gr = 0.0f;
            if (knee > 0.0f && over > -knee * 0.5f && over < knee * 0.5f)
            {
                const float x = over + knee * 0.5f;
                gr = -((1.0f - 1.0f / ratio) * (x * x) / (2.0f * knee));
            }
            else if (over >= knee * 0.5f)
            {
                gr = -(over * (1.0f - 1.0f / ratio));
            }
            return inDb + gr + makeup;
        }

        void drawTransfer (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (curveArea);

            const float lo = -60.0f, hi = 6.0f;
            auto xFor = [&] (float db) { return juce::jmap (db, lo, hi, (float) curveArea.getX(), (float) curveArea.getRight()); };
            auto yFor = [&] (float db) { return juce::jmap (db, lo, hi, (float) curveArea.getBottom(), (float) curveArea.getY()); };

            // grid + diagonal (unity)
            g.setColour (juce::Colour::fromRGB (40, 40, 48));
            for (float db = -54.0f; db <= hi; db += 12.0f)
            {
                g.drawHorizontalLine ((int) yFor (db), (float) curveArea.getX(), (float) curveArea.getRight());
                g.drawVerticalLine ((int) xFor (db), (float) curveArea.getY(), (float) curveArea.getBottom());
            }
            g.setColour (juce::Colour::fromRGB (60, 60, 70));
            g.drawLine (xFor (lo), yFor (lo), xFor (hi), yFor (hi), 1.0f); // unity diagonal

            // threshold marker
            const float th = getParam ("threshold");
            g.setColour (juce::Colour::fromRGB (255, 180, 60).withAlpha (0.5f));
            g.drawVerticalLine ((int) xFor (th), (float) curveArea.getY(), (float) curveArea.getBottom());

            // transfer curve
            juce::Path path;
            for (int x = curveArea.getX(); x <= curveArea.getRight(); ++x)
            {
                const float inDb = juce::jmap ((float) x, (float) curveArea.getX(), (float) curveArea.getRight(), lo, hi);
                const float y = juce::jlimit ((float) curveArea.getY(), (float) curveArea.getBottom(), yFor (outputDb (inDb)));
                if (x == curveArea.getX())
                    path.startNewSubPath ((float) x, y);
                else
                    path.lineTo ((float) x, y);
            }
            g.setColour (juce::Colour::fromRGB (0, 255, 210));
            g.strokePath (path, juce::PathStrokeType (2.0f));

            g.setColour (juce::Colour::fromRGB (90, 90, 100));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("in/out dB", curveArea.getX() + 3, curveArea.getY() + 2, 70, 12, juce::Justification::topLeft);
        }

        void drawMeter (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (meterArea);

            const float gr = juce::jlimit (-24.0f, 0.0f, comp.getGainReductionDb()); // <= 0
            const float frac = -gr / 24.0f; // 0..1
            auto bar = meterArea.toFloat();
            const float h = bar.getHeight() * frac;
            g.setColour (juce::Colour::fromRGB (255, 90, 60));
            g.fillRect (bar.withHeight (h)); // grows downward from top

            g.setColour (juce::Colour::fromRGB (60, 60, 70));
            g.drawRect (meterArea, 1);
            g.setColour (juce::Colour::fromRGB (150, 150, 160));
            g.setFont (juce::FontOptions (9.0f));
            g.drawText ("GR", meterArea.getX(), meterArea.getBottom() + 1, meterArea.getWidth(), 12, juce::Justification::centred);
            g.drawText (juce::String (gr, 1), meterArea.getX() - 4, meterArea.getY() - 13, meterArea.getWidth() + 8, 12, juce::Justification::centred);
        }

        static constexpr int topHeight = 200;

        CompressorProcessor& comp;
        std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
        juce::Viewport viewport;
        juce::Rectangle<int> topArea, curveArea, meterArea;
    };

} // namespace dcr::builtin

#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cmath>

namespace dcr::builtin
{

    // Custom editor for the Level Rider: a scrolling history of the applied rider
    // gain over time (so you can WATCH it ride the program) plus a bipolar
    // instantaneous gain meter (boost = green above 0, cut = red below 0), over
    // the full generic parameter list.
    class LevelRiderEditor : public juce::AudioProcessorEditor,
                             private juce::Timer
    {
    public:
        explicit LevelRiderEditor (LevelerProcessor& proc)
            : juce::AudioProcessorEditor (proc), rider (proc)
        {
            generic = std::make_unique<juce::GenericAudioProcessorEditor> (rider);
            viewport.setViewedComponent (generic.get(), false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);

            history.fill (0.0f);
            setSize (480, 440);
            startTimerHz (30);
        }

        ~LevelRiderEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            topArea = r.removeFromTop (topHeight);
            meterArea = topArea.removeFromRight (60).reduced (6);
            graphArea = topArea.reduced (8);
            viewport.setBounds (r);
            generic->setSize (r.getWidth() - 10, juce::jmax (generic->getHeight(), 5 * 26 + 30));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (14, 14, 18));
            drawGraph (g);
            drawMeter (g);
        }

    private:
        void timerCallback() override
        {
            // Push the latest rider gain into the scrolling history ring.  Guard
            // against a NaN/Inf readout freezing the graph (a bad sample would
            // otherwise persist in the ring and the curve would flatline).
            float g = rider.getRiderGainDb();
            if (!std::isfinite (g))
                g = 0.0f;
            history[(size_t) writePos] = g;
            writePos = (writePos + 1) % (int) history.size();
            // Repaint the top region by absolute bounds (not the cached topArea
            // rect, which can be momentarily empty/stale right after a resize) so
            // the graph + meter never stop animating.
            repaint (0, 0, getWidth(), topHeight);
        }

        float rangeDb() const
        {
            if (auto* a = rider.getValueTreeState().getRawParameterValue ("range"))
                return juce::jmax (1.0f, a->load());
            return 12.0f;
        }

        void drawGraph (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (graphArea);

            const float rng = rangeDb();
            const auto top = (float) graphArea.getY(), bot = (float) graphArea.getBottom();
            const auto lft = (float) graphArea.getX(), rgt = (float) graphArea.getRight();
            auto yFor = [&] (float db) { return juce::jmap (juce::jlimit (-rng, rng, db), rng, -rng, top, bot); };

            // Grid: 0 line (bright) + half-range guides (dim).
            g.setColour (juce::Colour::fromRGB (32, 32, 40));
            g.drawHorizontalLine ((int) yFor (rng * 0.5f), lft, rgt);
            g.drawHorizontalLine ((int) yFor (-rng * 0.5f), lft, rgt);
            g.setColour (juce::Colour::fromRGB (60, 60, 72));
            g.drawHorizontalLine ((int) yFor (0.0f), lft, rgt);

            // History polyline, oldest (left) -> newest (right).
            juce::Path path;
            const int n = (int) history.size();
            for (int i = 0; i < n; ++i)
            {
                const int idx = (writePos + i) % n; // writePos points at the oldest slot
                const float x = juce::jmap ((float) i, 0.0f, (float) (n - 1), lft, rgt);
                const float y = yFor (history[(size_t) idx]);
                if (i == 0)
                    path.startNewSubPath (x, y);
                else
                    path.lineTo (x, y);
            }
            g.setColour (juce::Colour::fromRGB (0, 255, 210));
            g.strokePath (path, juce::PathStrokeType (1.5f));

            g.setColour (juce::Colour::fromRGB (90, 90, 100));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("rider gain over time", graphArea.getX() + 4, graphArea.getY() + 2, 200, 12, juce::Justification::topLeft);
            g.setColour (juce::Colour::fromRGB (70, 70, 82));
            g.drawText ("+" + juce::String (rng, 0), graphArea.getX() + 4, graphArea.getY() + 2, 44, 12, juce::Justification::topRight);
            g.drawText ("-" + juce::String (rng, 0), graphArea.getX() + 4, graphArea.getBottom() - 14, 44, 12, juce::Justification::bottomRight);
        }

        void drawMeter (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (meterArea);

            const float rng = rangeDb();
            const float gain = juce::jlimit (-rng, rng, rider.getRiderGainDb());
            auto bar = meterArea.toFloat();
            const float zeroY = juce::jmap (0.0f, rng, -rng, bar.getY(), bar.getBottom());
            const float gainY = juce::jmap (gain, rng, -rng, bar.getY(), bar.getBottom());

            if (gain >= 0.0f) // boost -> green, fills upward from 0
            {
                g.setColour (juce::Colour::fromRGB (0, 220, 120));
                g.fillRect (juce::Rectangle<float> (bar.getX(), gainY, bar.getWidth(), zeroY - gainY));
            }
            else // cut -> red, fills downward from 0
            {
                g.setColour (juce::Colour::fromRGB (255, 90, 60));
                g.fillRect (juce::Rectangle<float> (bar.getX(), zeroY, bar.getWidth(), gainY - zeroY));
            }

            g.setColour (juce::Colour::fromRGB (120, 120, 130));
            g.drawHorizontalLine ((int) zeroY, bar.getX(), bar.getRight()); // unity line
            g.setColour (juce::Colour::fromRGB (60, 60, 70));
            g.drawRect (meterArea, 1);

            g.setColour (juce::Colour::fromRGB (150, 150, 160));
            g.setFont (juce::FontOptions (9.0f));
            g.drawText ("GAIN", meterArea.getX(), meterArea.getBottom() + 1, meterArea.getWidth(), 12, juce::Justification::centred);
            g.drawText (juce::String (gain, 1) + " dB", meterArea.getX() - 8, meterArea.getY() - 13, meterArea.getWidth() + 16, 12, juce::Justification::centred);
        }

        static constexpr int topHeight = 190;

        LevelerProcessor& rider;
        std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
        juce::Viewport viewport;
        juce::Rectangle<int> topArea, graphArea, meterArea;
        std::array<float, 256> history {};
        int writePos = 0;
    };

} // namespace dcr::builtin

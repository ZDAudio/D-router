#include "UI/LevelMeter.h"

#include <cmath>

namespace dcr
{

    LevelMeter::LevelMeter (Orientation o) : orientation (o) {}

    void LevelMeter::pushPeak (float p)
    {
        if (p > currentLevel)
            currentLevel = p;
    }

    int LevelMeter::computeActiveSegments() const noexcept
    {
        if (currentLevel < 1.0e-5f)
            return 0;
        const float db = 20.0f * std::log10 (currentLevel);
        const float t = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        return (int) std::round (t * (float) numSegments);
    }

    void LevelMeter::tickDecay (float decayFactor)
    {
        currentLevel *= decayFactor;
        if (currentLevel < 1.0e-4f)
            currentLevel = 0.0f;

        const int newActive = computeActiveSegments();
        if (newActive != lastDrawnSegments)
        {
            lastDrawnSegments = newActive;
            repaint(); // only when the visible state actually changed
        }
    }

    void LevelMeter::paint (juce::Graphics& g)
    {
        const auto r = getLocalBounds().toFloat();

        // Background (single fill - flat, no anti-aliased outline).
        g.fillAll (juce::Colour::fromRGB (10, 10, 12));

        const int active = juce::jmax (lastDrawnSegments, computeActiveSegments());
        if (active <= 0)
            return;

        const bool isHorizontal = (orientation == Orientation::Horizontal);
        const float gap = 1.0f;

        // Pre-compute segment colours - cheap, no allocations.
        auto colourForIndex = [] (int i) -> juce::Colour {
            // 0..2 = teal, 3 = amber, 4 = red (with 5 segments).
            if (i >= numSegments - 1)
                return juce::Colour::fromRGB (230, 40, 40);
            if (i >= numSegments - 2)
                return juce::Colour::fromRGB (220, 140, 20);
            return juce::Colour::fromRGB (0, 180, 160);
        };

        if (isHorizontal)
        {
            const float segW = (r.getWidth() - (float) (numSegments - 1) * gap) / (float) numSegments;
            for (int i = 0; i < active; ++i)
            {
                g.setColour (colourForIndex (i));
                g.fillRect (r.getX() + (float) i * (segW + gap), r.getY(), segW, r.getHeight());
            }
        }
        else
        {
            const float segH = (r.getHeight() - (float) (numSegments - 1) * gap) / (float) numSegments;
            for (int i = 0; i < active; ++i)
            {
                g.setColour (colourForIndex (i));
                g.fillRect (r.getX(), r.getBottom() - segH - (float) i * (segH + gap), r.getWidth(), segH);
            }
        }
    }

} // namespace dcr

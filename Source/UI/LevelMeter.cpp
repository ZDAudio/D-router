#include "UI/LevelMeter.h"

#include <cmath>

namespace dcr {

LevelMeter::LevelMeter (Orientation o) : orientation (o) {}

void LevelMeter::pushPeak (float p)
{
    if (p > currentLevel) currentLevel = p;
}

void LevelMeter::tickDecay (float decayFactor)
{
    currentLevel *= decayFactor;
    if (currentLevel < 1.0e-4f) currentLevel = 0.0f;
    repaint();
}

void LevelMeter::paint (juce::Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (juce::Colour::fromRGB (16, 16, 18));
    g.fillRoundedRectangle (r, 1.5f);

    if (currentLevel < 1.0e-5f)
    {
        g.setColour (juce::Colour::fromRGB (40, 40, 46));
        g.drawRoundedRectangle (r, 1.5f, 0.5f);
        return;
    }

    // Convert linear -> dB -> normalized 0..1 over [-60, +6] dB range.
    const float db = 20.0f * std::log10 (juce::jmax (currentLevel, 1.0e-6f));
    const float t  = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);

    juce::Colour low  = juce::Colour::fromRGB (40, 200, 80);     // green
    juce::Colour mid  = juce::Colour::fromRGB (230, 220, 30);    // yellow
    juce::Colour high = juce::Colour::fromRGB (230, 50, 40);     // red

    juce::Colour col;
    // db <= -6 => green; -6..0 => green->yellow; >0 => red
    if      (db <= -6.0f) col = low;
    else if (db <= 0.0f)  col = low.interpolatedWith (mid, (db + 6.0f) / 6.0f);
    else                  col = high;

    if (orientation == Orientation::Horizontal)
    {
        const float w = r.getWidth() * t;
        g.setColour (col);
        g.fillRoundedRectangle (r.withWidth (w), 1.5f);
    }
    else
    {
        const float h = r.getHeight() * t;
        g.setColour (col);
        g.fillRoundedRectangle (r.withTrimmedTop (r.getHeight() - h), 1.5f);
    }

    g.setColour (juce::Colour::fromRGB (60, 60, 70));
    g.drawRoundedRectangle (r, 1.5f, 0.5f);
}

} // namespace dcr

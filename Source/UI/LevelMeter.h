#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr {

// Simple peak meter.  All timing/decay is driven externally by the owner
// (typically MatrixView's timer) -- no per-instance juce::Timer so 1000+
// meters don't each fire their own callback.
class LevelMeter : public juce::Component
{
public:
    enum class Orientation { Horizontal, Vertical };

    explicit LevelMeter (Orientation o);

    // Push a new linear peak (>=0). Internal level = max (current, pushed).
    void pushPeak (float linearPeak);

    // Multiply current level by `decayFactor` (0..1) and trigger repaint.
    // Call once per UI tick at the rate configured in EngineSettings.
    void tickDecay (float decayFactor);

    void paint (juce::Graphics&) override;

private:
    Orientation orientation;
    float currentLevel = 0.0f;
};

} // namespace dcr

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr
{

    // Simple peak meter.  All timing/decay is driven externally by the owner
    // (typically MatrixView's timer) -- no per-instance juce::Timer so 1000+
    // meters don't each fire their own callback.
    class LevelMeter : public juce::Component
    {
    public:
        enum class Orientation { Horizontal,
            Vertical };

        explicit LevelMeter (Orientation o);

        // Push a new linear peak (>=0). Internal level = max (current, pushed).
        void pushPeak (float linearPeak);

        // Multiply current level by `decayFactor` (0..1).  Triggers repaint ONLY
        // when the discrete segment count actually changes - idle meters do zero
        // work this way.  Call once per UI tick at the rate configured in
        // EngineSettings.
        void tickDecay (float decayFactor);

        void paint (juce::Graphics&) override;

        // Fixed 5-segment LED bar.  At 5 segments most paint cost goes away; the
        // user can resort to inserting a metering AU plugin if a precise display
        // is needed.
        static constexpr int numSegments = 5;

    private:
        int computeActiveSegments() const noexcept;

        Orientation orientation;
        float currentLevel = 0.0f;
        int lastDrawnSegments = -1; // -1 forces first paint
    };

} // namespace dcr

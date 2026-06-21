#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace dcr
{

    // A single matrix crosspoint cell.
    //  - Click toggles route on/off (gain 0 -> 1, or 1 -> 0).
    //  - Vertical drag adjusts gain (-inf .. +12 dB).
    //  - Double-click resets to unity.
    //  - Right-click prompts for a numeric dB value.
    class Crosspoint : public juce::Component
    {
    public:
        Crosspoint();

        void setLinearGain (float g);
        float getLinearGain() const noexcept { return gainLinear; }

        // Called whenever the user changes gain. UI -> model.
        std::function<void (float linearGain)> onChange;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;

    private:
        void promptForDbValue();
        static float dbToLin (float db) noexcept;
        static float linToDb (float lin) noexcept;

        float gainLinear = 0.0f;
        float dragStartDb = 0.0f;
        bool draggedSinceMouseDown = false;
    };

} // namespace dcr

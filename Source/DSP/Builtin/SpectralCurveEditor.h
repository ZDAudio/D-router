#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{

    // Shared editor for the spectral plugins (Auto-EQ, Resonance Suppressor).
    // Draws the live magnitude spectrum plus the applied gain / reduction curve
    // over the full generic parameter list.  Proc must expose getDisplaySize(),
    // getDisplayMagDb(i), getDisplayGainDb(i) and be an AudioProcessor.
    template <typename Proc>
    class SpectralCurveEditor : public juce::AudioProcessorEditor,
                                private juce::Timer
    {
    public:
        explicit SpectralCurveEditor (Proc& p)
            : juce::AudioProcessorEditor (p), proc (p)
        {
            generic = std::make_unique<juce::GenericAudioProcessorEditor> (proc);
            viewport.setViewedComponent (generic.get(), false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);
            setSize (520, 470);
            startTimerHz (30);
        }
        ~SpectralCurveEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            graphArea = r.removeFromTop (topH);
            viewport.setBounds (r);
            generic->setSize (r.getWidth() - 10, juce::jmax (generic->getHeight(), 7 * 26 + 30));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (14, 14, 18));
            auto a = graphArea.reduced (8);
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (a);

            const int n = proc.getDisplaySize();
            if (n < 2)
                return;

            const float magLo = -90.0f, magHi = 0.0f; // dBFS
            const float gRange = 18.0f; // +/- dB for the gain curve
            const auto L = (float) a.getX(), R = (float) a.getRight();
            const auto T = (float) a.getY(), B = (float) a.getBottom();

            auto xFor = [&] (int i) { return juce::jmap ((float) i, 0.0f, (float) (n - 1), L, R); };
            auto yMag = [&] (float db) { return juce::jmap (juce::jlimit (magLo, magHi, db), magLo, magHi, B, T); };
            auto yGain = [&] (float db) { return juce::jmap (juce::jlimit (-gRange, gRange, db), gRange, -gRange, T, B); };

            // Grid + 0 dB gain centre line.
            g.setColour (juce::Colour::fromRGB (36, 36, 44));
            for (float db = magLo; db <= magHi + 0.1f; db += 18.0f)
                g.drawHorizontalLine ((int) yMag (db), L, R);
            g.setColour (juce::Colour::fromRGB (60, 60, 72));
            g.drawHorizontalLine ((int) yGain (0.0f), L, R);

            // Spectrum (filled).
            juce::Path spec;
            spec.startNewSubPath (L, B);
            for (int i = 0; i < n; ++i)
                spec.lineTo (xFor (i), yMag (proc.getDisplayMagDb (i)));
            spec.lineTo (R, B);
            spec.closeSubPath();
            g.setColour (juce::Colour::fromRGB (60, 120, 150).withAlpha (0.35f));
            g.fillPath (spec);
            g.setColour (juce::Colour::fromRGB (90, 170, 200));
            juce::Path specLine;
            for (int i = 0; i < n; ++i)
            {
                const float x = xFor (i), y = yMag (proc.getDisplayMagDb (i));
                if (i == 0)
                    specLine.startNewSubPath (x, y);
                else
                    specLine.lineTo (x, y);
            }
            g.strokePath (specLine, juce::PathStrokeType (1.0f));

            // Gain / reduction curve.
            juce::Path gainPath;
            for (int i = 0; i < n; ++i)
            {
                const float x = xFor (i), y = yGain (proc.getDisplayGainDb (i));
                if (i == 0)
                    gainPath.startNewSubPath (x, y);
                else
                    gainPath.lineTo (x, y);
            }
            g.setColour (juce::Colour::fromRGB (0, 255, 210));
            g.strokePath (gainPath, juce::PathStrokeType (2.0f));

            g.setColour (juce::Colour::fromRGB (90, 90, 100));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("spectrum + correction (low -> high)", a.getX() + 3, a.getY() + 2, 260, 12, juce::Justification::topLeft);
        }

    private:
        void timerCallback() override { repaint (graphArea); }

        static constexpr int topH = 200;

        Proc& proc;
        std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
        juce::Viewport viewport;
        juce::Rectangle<int> graphArea;
    };

} // namespace dcr::builtin

#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{

    // De-esser editor: a frequency axis (2 k .. 16 k) with the crossover marker,
    // a horizontal "sibilance band level" bar with the threshold tick, and a
    // vertical gain-reduction meter -- over the generic parameter list.
    class DeEsserEditor : public juce::AudioProcessorEditor,
                          private juce::Timer
    {
    public:
        explicit DeEsserEditor (DeEsserProcessor& proc)
            : juce::AudioProcessorEditor (proc), de (proc)
        {
            generic = std::make_unique<juce::GenericAudioProcessorEditor> (de);
            viewport.setViewedComponent (generic.get(), false);
            viewport.setScrollBarsShown (true, false);
            addAndMakeVisible (viewport);
            setSize (460, 380);
            startTimerHz (24);
        }

        ~DeEsserEditor() override { stopTimer(); }

        void resized() override
        {
            auto r = getLocalBounds();
            topArea = r.removeFromTop (topHeight);
            meterArea = topArea.removeFromRight (54).reduced (6);
            bandArea = topArea.reduced (8);
            viewport.setBounds (r);
            generic->setSize (r.getWidth() - 10, juce::jmax (generic->getHeight(), 5 * 26 + 30));
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour::fromRGB (14, 14, 18));
            drawBand (g);
            drawMeter (g);
        }

    private:
        // Repaint the whole top strip (band + meter).  Note: the member topArea
        // is shrunk in resized() to exclude meterArea, so repainting topArea
        // alone leaves the GR meter frozen -- cover the full strip explicitly.
        void timerCallback() override { repaint (0, 0, getWidth(), topHeight); }

        float getParam (const juce::String& id) const
        {
            if (auto* a = de.getValueTreeState().getRawParameterValue (id))
                return a->load();
            return 0.0f;
        }

        static float xForFreq (float f, juce::Rectangle<int> a)
        {
            return juce::jmap (std::log10 (juce::jlimit (2000.0f, 16000.0f, f)),
                std::log10 (2000.0f),
                std::log10 (16000.0f),
                (float) a.getX(),
                (float) a.getRight());
        }

        void drawBand (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (bandArea);

            // freq grid + labels
            g.setFont (juce::FontOptions (10.0f));
            for (float f : { 2000.0f, 4000.0f, 6000.0f, 8000.0f, 12000.0f, 16000.0f })
            {
                const float x = xForFreq (f, bandArea);
                g.setColour (juce::Colour::fromRGB (40, 40, 48));
                g.drawVerticalLine ((int) x, (float) bandArea.getY(), (float) bandArea.getBottom());
                g.setColour (juce::Colour::fromRGB (90, 90, 100));
                g.drawText (juce::String (f / 1000.0f, 0) + "k", (int) x + 2, bandArea.getBottom() - 13, 30, 12, juce::Justification::topLeft);
            }

            // Affected region depends on Mode: Split ducks a band around `freq`
            // (band-pass detector, Q~2 -> roughly 0.78x..1.28x the centre);
            // Wideband ducks the whole signal.  Listen auditions the band.
            const float fc = getParam ("freq");
            const bool wide = getParam ("mode") > 0.5f;
            const bool listen = getParam ("listen") > 0.5f;
            const float regTop = (float) bandArea.getY();
            const float regH = (float) bandArea.getHeight() * 0.62f; // above the level bar
            if (wide)
            {
                g.setColour (juce::Colour::fromRGBA (0, 200, 220, 36)); // whole signal
                g.fillRect ((float) bandArea.getX(), regTop, (float) bandArea.getWidth(), regH);
            }
            else
            {
                const float xlo = xForFreq (fc * 0.78f, bandArea);
                const float xhi = xForFreq (fc * 1.28f, bandArea);
                g.setColour (juce::Colour::fromRGBA (0, 200, 220, 70));
                g.fillRect (xlo, regTop, xhi - xlo, regH);
            }
            // centre marker
            const float fx = xForFreq (fc, bandArea);
            g.setColour (juce::Colour::fromRGB (0, 200, 220));
            g.fillRect (fx - 1.0f, regTop, 2.0f, (float) bandArea.getHeight());

            // mode / listen caption, top-left of the band
            g.setColour (juce::Colour::fromRGB (110, 190, 205));
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (juce::String (wide ? "WIDEBAND" : "SPLIT") + (listen ? "  -  LISTEN" : ""),
                bandArea.getX() + 4,
                bandArea.getY() + 2,
                bandArea.getWidth() - 8,
                12,
                juce::Justification::topLeft);

            // sibilance level bar (horizontal, mapped -60..0 dB across width) +
            // threshold tick.
            const float lvl = juce::jlimit (-60.0f, 0.0f, de.getBandLevelDb());
            const float th = juce::jlimit (-60.0f, 0.0f, getParam ("threshold"));
            auto levelToX = [&] (float db) { return juce::jmap (db, -60.0f, 0.0f, (float) bandArea.getX(), (float) bandArea.getRight()); };
            const float by = bandArea.getCentreY() + 10.0f;
            g.setColour (juce::Colour::fromRGB (40, 40, 50));
            g.fillRect ((float) bandArea.getX(), by, (float) bandArea.getWidth(), 12.0f);
            g.setColour (lvl > th ? juce::Colour::fromRGB (255, 90, 60) : juce::Colour::fromRGB (0, 200, 160));
            g.fillRect ((float) bandArea.getX(), by, levelToX (lvl) - bandArea.getX(), 12.0f);
            g.setColour (juce::Colour::fromRGB (255, 200, 80));
            g.fillRect (levelToX (th) - 1.0f, by - 2.0f, 2.0f, 16.0f); // threshold tick

            g.setColour (juce::Colour::fromRGB (130, 130, 140));
            g.setFont (juce::FontOptions (10.0f));
            g.drawText ("sibilance band level / threshold", bandArea.getX() + 2, (int) by - 14, bandArea.getWidth(), 12, juce::Justification::topLeft);
        }

        void drawMeter (juce::Graphics& g)
        {
            g.setColour (juce::Colour::fromRGB (20, 20, 26));
            g.fillRect (meterArea);
            const float gr = juce::jlimit (-24.0f, 0.0f, de.getGainReductionDb());
            const float frac = -gr / 24.0f;
            auto bar = meterArea.toFloat();
            g.setColour (juce::Colour::fromRGB (255, 90, 60));
            g.fillRect (bar.withHeight (bar.getHeight() * frac));
            g.setColour (juce::Colour::fromRGB (60, 60, 70));
            g.drawRect (meterArea, 1);
            g.setColour (juce::Colour::fromRGB (150, 150, 160));
            g.setFont (juce::FontOptions (9.0f));
            g.drawText ("GR", meterArea.getX(), meterArea.getBottom() + 1, meterArea.getWidth(), 12, juce::Justification::centred);
            g.drawText (juce::String (gr, 1), meterArea.getX() - 4, meterArea.getY() - 13, meterArea.getWidth() + 8, 12, juce::Justification::centred);
        }

        static constexpr int topHeight = 150;

        DeEsserProcessor& de;
        std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
        juce::Viewport viewport;
        juce::Rectangle<int> topArea, bandArea, meterArea;
    };

} // namespace dcr::builtin

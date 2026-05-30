#pragma once

#include "DSP/Builtin/BuiltinProcessors.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr::builtin
{

// Multiband compressor editor: four live gain-reduction meters (one per band,
// labelled with the band name + its frequency span) across the top, over the
// grouped parameter list (Crossovers / 4 bands / Output).
class MultibandCompEditor : public juce::AudioProcessorEditor,
                            private juce::Timer
{
public:
    explicit MultibandCompEditor (MultibandCompProcessor& proc)
        : juce::AudioProcessorEditor (proc), mb (proc)
    {
        generic = std::make_unique<juce::GenericAudioProcessorEditor> (mb);
        viewport.setViewedComponent (generic.get(), false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        setSize (560, 600);
        startTimerHz (24);
    }

    ~MultibandCompEditor() override { stopTimer(); }

    void resized() override
    {
        auto r = getLocalBounds();
        meterStrip = r.removeFromTop (topHeight);
        viewport.setBounds (r);
        generic->setSize (r.getWidth() - 10,
                          juce::jmax (generic->getHeight(),
                                      (3 + MultibandCompProcessor::kBands * 6 + 1) * 26 + 80));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour::fromRGB (14, 14, 18));
        drawMeters (g);
    }

private:
    void timerCallback() override { repaint (meterStrip); }

    void drawMeters (juce::Graphics& g)
    {
        const int   N = MultibandCompProcessor::kBands;
        const char* names[4] = { "Low", "Low-Mid", "High-Mid", "High" };
        auto area = meterStrip.reduced (10, 8);
        const int cellW = area.getWidth() / N;

        // band frequency spans from the crossovers
        const float xo[3] = { mb.getCrossover (0), mb.getCrossover (1), mb.getCrossover (2) };
        auto hz = [] (float f) { return f >= 1000.0f ? juce::String (f / 1000.0f, 1) + "k" : juce::String ((int) f); };
        juce::String spans[4] = {
            "< " + hz (xo[0]),
            hz (xo[0]) + "-" + hz (xo[1]),
            hz (xo[1]) + "-" + hz (xo[2]),
            "> " + hz (xo[2])
        };

        for (int b = 0; b < N; ++b)
        {
            auto cell = juce::Rectangle<int> (area.getX() + b * cellW, area.getY(), cellW, area.getHeight()).reduced (6, 0);
            auto barArea = cell.withTrimmedTop (16).withTrimmedBottom (24);

            g.setColour (juce::Colour::fromRGB (22, 22, 28));
            g.fillRect (barArea);

            const float gr = juce::jlimit (-24.0f, 0.0f, mb.getBandGrDb (b));   // <= 0
            auto bar = barArea.toFloat();
            g.setColour (juce::Colour::fromRGB (255, 110, 70));
            g.fillRect (bar.withHeight (bar.getHeight() * (-gr / 24.0f)));      // grows downward
            g.setColour (juce::Colour::fromRGB (60, 60, 70));
            g.drawRect (barArea, 1);

            g.setColour (juce::Colour::fromRGB (210, 210, 220));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (names[b], cell.getX(), cell.getY(), cell.getWidth(), 14, juce::Justification::centred);

            g.setColour (juce::Colour::fromRGB (130, 130, 140));
            g.setFont (juce::FontOptions (9.5f));
            g.drawText (spans[b], cell.getX(), barArea.getBottom() + 1, cell.getWidth(), 11, juce::Justification::centred);
            g.drawText (juce::String (gr, 1) + " dB", cell.getX(), barArea.getBottom() + 12, cell.getWidth(), 11,
                        juce::Justification::centred);
        }
    }

    static constexpr int topHeight = 150;

    MultibandCompProcessor& mb;
    std::unique_ptr<juce::GenericAudioProcessorEditor> generic;
    juce::Viewport viewport;
    juce::Rectangle<int> meterStrip;
};

} // namespace dcr::builtin

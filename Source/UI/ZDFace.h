#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr
{
    // The "ZD" easter-egg smiley: a ring with "Z" and "D" as the eyes and a
    // smile arc. Ported from atmosmeterppmbigsur's ZDReferenceMark (the friendly
    // `.faceZD` / happy variant). Shown next to the brand title once the hidden
    // Stereo Meter is unlocked. Non-interactive.
    class ZDFace : public juce::Component
    {
    public:
        ZDFace() { setInterceptsMouseClicks (false, false); }

        void paint (juce::Graphics& g) override
        {
            const auto bounds = getLocalBounds().toFloat();
            const float d = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 2.0f;
            if (d <= 4.0f)
                return;
            const auto area = juce::Rectangle<float> (d, d).withCentre (bounds.getCentre());
            const auto col = juce::Colour::fromFloatRGBA (0.86f, 0.62f, 0.18f, 1.0f); // bezel amber
            const float lw = juce::jmax (1.2f, d * 0.09f);

            g.setColour (col);
            g.drawEllipse (area.reduced (lw * 0.5f), lw);

            // Eyes: the two letters, upper half.
            g.setFont (juce::Font (juce::FontOptions (d * 0.42f, juce::Font::bold)));
            // Two "eyes" drawn as one tight, centred "ZD" — separate boxes read
            // as eyes set too far apart. A single string keeps natural (close)
            // letter spacing; the full-width box guarantees no "…" truncation.
            g.drawText ("ZD",
                        juce::Rectangle<float> (area.getX(), area.getY() + d * 0.16f, d, d * 0.44f),
                        juce::Justification::centred);

            // Mouth: upward smile arc (quadratic, control pulled down).
            juce::Path smile;
            smile.startNewSubPath (area.getX() + d * 0.34f, area.getY() + d * 0.72f);
            smile.quadraticTo (area.getX() + d * 0.50f, area.getY() + d * 0.88f,
                               area.getX() + d * 0.66f, area.getY() + d * 0.72f);
            g.strokePath (smile, juce::PathStrokeType (juce::jmax (1.2f, d * 0.10f),
                                                       juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
        }
    };

} // namespace dcr

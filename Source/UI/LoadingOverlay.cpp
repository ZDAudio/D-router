#include "UI/LoadingOverlay.h"

namespace dcr {

LoadingOverlay::LoadingOverlay()
{
    // Sit on top of everything else.  EAT every mouse event so the user
    // can't click through into a half-built matrix tree -- if they hit a
    // crosspoint while the engine is mid-reconfigure they can race
    // matrix.resize() and segfault.  This replaces a per-component
    // setEnabled(false/true) cycle that used to recursively repaint
    // hundreds of children, flooding the message thread right when we
    // were trying to settle.
    setInterceptsMouseClicks (true, false);
    setOpaque (false);
    setAlwaysOnTop (true);
}

void LoadingOverlay::paint (juce::Graphics& g)
{
    // Dimmed full-window backdrop so the (possibly half-built) matrix
    // behind the overlay isn't visually distracting.
    g.fillAll (juce::Colour::fromRGBA (8, 8, 12, 220));

    auto r = getLocalBounds().toFloat();

    // Title block in the center -- big bold brand name.
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (36.0f, juce::Font::bold));
    auto titleRect = r.withSizeKeepingCentre ((float) getWidth(), 60.0f)
                       .translated (0.0f, -30.0f);
    g.drawText (title, titleRect.toNearestInt(), juce::Justification::centred);

    // Subtitle / status line.
    g.setColour (juce::Colour::fromRGB (180, 200, 255));
    g.setFont (juce::FontOptions (13.0f, 0));
    auto subRect = r.withSizeKeepingCentre ((float) getWidth(), 28.0f)
                     .translated (0.0f, 20.0f);
    g.drawText (subtitle, subRect.toNearestInt(), juce::Justification::centred);

    // Progress bar (only when we have a definite total).
    if (progressTotal > 0)
    {
        const float barW = juce::jmin (320.0f, r.getWidth() * 0.5f);
        const float barH = 4.0f;
        auto barOuter = juce::Rectangle<float> (barW, barH)
                          .withCentre ({ r.getCentreX(), r.getCentreY() + 55.0f });
        g.setColour (juce::Colour::fromRGB (40, 40, 50));
        g.fillRect (barOuter);
        const float frac = juce::jlimit (0.0f, 1.0f,
                                         (float) progressDone / (float) progressTotal);
        auto barInner = barOuter.withWidth (barOuter.getWidth() * frac);
        g.setColour (juce::Colour::fromRGB (0, 200, 220));   // cyan accent
        g.fillRect (barInner);

        // "24 / 114" counter under the bar
        g.setColour (juce::Colour::fromRGB (130, 130, 140));
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, 0));
        const juce::String count = juce::String (progressDone) + " / " + juce::String (progressTotal);
        g.drawText (count,
                    barOuter.translated (0.0f, 14.0f).toNearestInt(),
                    juce::Justification::centred);
    }
}

void LoadingOverlay::resized() {}

void LoadingOverlay::setMessage (const juce::String& m)
{
    subtitle = m;
    repaint();
}

void LoadingOverlay::setProgress (int doneSteps, int totalSteps)
{
    progressDone  = juce::jmax (0, doneSteps);
    progressTotal = juce::jmax (0, totalSteps);
    repaint();
}

void LoadingOverlay::hideOverlay()
{
    setVisible (false);
}

void LoadingOverlay::showOverlay (const juce::String& m)
{
    subtitle      = m;
    progressDone  = 0;
    progressTotal = 0;
    setVisible (true);
    toFront (true);
    repaint();
}

} // namespace dcr

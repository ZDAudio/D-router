#include "UI/LookAndFeel.h"

#include "Engine/EngineSettings.h"

#include <cmath>

namespace dcr {

void LookAndFeel::applyTheme (const EngineSettings& s)
{
    accentColor   = juce::Colour (0xFF000000u | s.accentColorRGB);
    warningColor  = juce::Colour (0xFF000000u | s.warningColorRGB);
    criticalColor = juce::Colour (0xFF000000u | s.criticalColorRGB);

    // Update colour-keyed component IDs that read from findColour().
    setColour (juce::PopupMenu::highlightedTextColourId,    accentColor);
    setColour (juce::TextEditor::focusedOutlineColourId,    accentColor);
}

LookAndFeel::LookAndFeel()
{
    // Define global color palette mappings
    setColour (juce::TextButton::textColourOffId, juce::Colour::fromRGB (180, 180, 185));
    setColour (juce::TextButton::textColourOnId,  juce::Colour::fromRGB (255, 255, 255));
    
    setColour (juce::ComboBox::backgroundColourId, juce::Colour::fromRGB (20, 20, 24));
    setColour (juce::ComboBox::outlineColourId,    juce::Colour::fromRGB (48, 48, 56));
    setColour (juce::ComboBox::textColourId,       juce::Colour::fromRGB (200, 200, 205));
    
    setColour (juce::PopupMenu::backgroundColourId,          juce::Colour::fromRGB (14, 14, 18));
    setColour (juce::PopupMenu::textColourId,                juce::Colour::fromRGB (190, 190, 195));
    setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour::fromRGB (24, 48, 46));
    setColour (juce::PopupMenu::highlightedTextColourId,       juce::Colour::fromRGB (0, 255, 210));
    
    setColour (juce::TextEditor::backgroundColourId,   juce::Colour::fromRGB (14, 14, 16));
    setColour (juce::TextEditor::textColourId,         juce::Colour::fromRGB (220, 220, 225));
    setColour (juce::TextEditor::outlineColourId,      juce::Colour::fromRGB (36, 36, 42));
    setColour (juce::TextEditor::focusedOutlineColourId, juce::Colour::fromRGB (0, 255, 210));
}

LookAndFeel::~LookAndFeel() = default;

void LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& /*backgroundColour*/,
                                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat();
    
    // Flat hardware look
    juce::Colour baseColor;
    if (button.getName() == "slot")
    {
        if (button.getButtonText() == "+ insert")
            baseColor = juce::Colour::fromRGB (20, 20, 24);
        else if (button.getToggleState()) // bypassed slot
            baseColor = juce::Colour::fromRGB (60, 45, 20); // Amber-gold bypass tint
        else // active loaded slot
            baseColor = juce::Colour::fromRGB (25, 48, 42); // Dark teal-green active tint
    }
    else if (button.getName() == "fx")
    {
        // Honor the per-button colour MatrixView sets via setColour(buttonColourId).
        // States: dark grey (no plugin) / cyan (any active) / dim red (all bypassed).
        baseColor = button.findColour (juce::TextButton::buttonColourId);
        if (shouldDrawButtonAsDown)             baseColor = baseColor.darker  (0.30f);
        else if (shouldDrawButtonAsHighlighted) baseColor = baseColor.brighter (0.15f);
    }
    else if (shouldDrawButtonAsDown)
        baseColor = juce::Colour::fromRGB (16, 16, 20);
    else if (shouldDrawButtonAsHighlighted)
        baseColor = juce::Colour::fromRGB (42, 42, 48);
    else
        baseColor = juce::Colour::fromRGB (28, 28, 34);

    g.setColour (baseColor);
    g.fillRect (bounds);

    // Clean outline (flat 1-px rect; cheaper than anti-aliased rounded).
    g.setColour (juce::Colour::fromRGB (48, 48, 56));
    g.drawRect (bounds, 1.0f);
    
    // LED Indicator strip for toggled active states
    if (button.getToggleState())
    {
        juce::Colour ledColor = accentColor; // Default accent
        
        juce::String text = button.getButtonText();
        if (text == "M" || button.getName().containsIgnoreCase ("mute"))
            ledColor = juce::Colour::fromRGB (255, 59, 48); // Mute Red
        else if (text == "S" || button.getName().containsIgnoreCase ("solo"))
            ledColor = juce::Colour::fromRGB (255, 204, 0); // Solo Yellow
        else if (text.startsWith ("FX") || button.getName().containsIgnoreCase ("fx"))
            ledColor = juce::Colour::fromRGB (52, 199, 89); // FX Green

        // Tiny flat LED strip on the bottom edge.
        auto ledBar = bounds.removeFromBottom (2.5f).reduced (2.0f, 0.0f);
        g.setColour (ledColor);
        g.fillRect (ledBar);
    }
}

void LookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                   bool /*shouldDrawButtonAsHighlighted*/, bool /*shouldDrawButtonAsDown*/)
{
    g.setFont (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(), 10.5f, juce::Font::bold));
    g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                             : juce::TextButton::textColourOffId));
    
    // Draw all-caps text for an industrial engineering feel
    juce::String text = button.getButtonText();
    
    // Let's preserve M, S, FX but capitalize dialog trigger buttons
    if (text.length() > 2 && text.endsWith ("..."))
    {
        text = text.substring (0, text.length() - 3).toUpperCase() + "...";
    }
    else if (text.length() > 2)
    {
        text = text.toUpperCase();
    }
    
    g.drawText (text, button.getLocalBounds(), juce::Justification::centred, true);
}

void LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                     float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle,
                                     juce::Slider& slider)
{
    auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (2.0f);
    auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f;
    auto centreX = bounds.getCentreX();
    auto centreY = bounds.getCentreY();
    
    auto angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
    
    // Outer rim / knob background
    g.setColour (juce::Colour::fromRGB (16, 16, 20));
    g.fillEllipse (centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f);
    
    g.setColour (juce::Colour::fromRGB (48, 48, 56));
    g.drawEllipse (centreX - radius, centreY - radius, radius * 2.0f, radius * 2.0f, 1.0f);
    
    // Arc indicating value path
    juce::Path arc;
    arc.addCentredArc (centreX, centreY, radius - 2.5f, radius - 2.5f, 0.0f,
                       rotaryStartAngle, angle, true);
    
    g.setColour (slider.isEnabled() ? accentColor
                                    : juce::Colour::fromRGB (60, 80, 80));
    g.strokePath (arc, juce::PathStrokeType (1.5f));
    
    // Inner machined circle dial
    auto innerRadius = radius * 0.55f;
    g.setColour (juce::Colour::fromRGB (28, 28, 34));
    g.fillEllipse (centreX - innerRadius, centreY - innerRadius, innerRadius * 2.0f, innerRadius * 2.0f);
    
    // Needle pointer line
    juce::Path needle;
    needle.startNewSubPath (centreX, centreY - innerRadius + 1.0f);
    needle.lineTo (centreX, centreY - radius + 1.0f);
    
    g.setColour (slider.isEnabled() ? juce::Colours::white : juce::Colours::grey);
    g.strokePath (needle, juce::PathStrokeType (1.5f), juce::AffineTransform::rotation (angle, centreX, centreY));
}

void LookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                     float sliderPos, float /*minSliderPos*/, float /*maxSliderPos*/,
                                     const juce::Slider::SliderStyle style, juce::Slider& slider)
{
    g.fillAll (juce::Colour::fromRGB (20, 20, 24));

    const bool isVertical = (style == juce::Slider::LinearVertical || style == juce::Slider::LinearBarVertical);

    // Track line.
    g.setColour (juce::Colour::fromRGB (36, 36, 42));
    if (isVertical)
    {
        const float trackX = (float) x + (float) width * 0.5f;
        g.drawLine (trackX, (float) y, trackX, (float) (y + height), 2.0f);

        const float handleW = 16.0f;
        const float handleH = 10.0f;
        const float handleX = trackX - handleW * 0.5f;
        const float handleY = sliderPos - handleH * 0.5f;

        g.setColour (juce::Colour::fromRGB (40, 40, 48));
        g.fillRect (handleX, handleY, handleW, handleH);
        g.setColour (juce::Colour::fromRGB (60, 60, 72));
        g.drawRect (handleX, handleY, handleW, handleH, 1.0f);

        g.setColour (slider.isEnabled() ? accentColor : juce::Colours::grey);
        g.drawLine (handleX + 1.0f, handleY + handleH * 0.5f,
                    handleX + handleW - 1.0f, handleY + handleH * 0.5f, 1.5f);
    }
    else
    {
        const float trackY = (float) y + (float) height * 0.5f;
        g.drawLine ((float) x, trackY, (float) (x + width), trackY, 2.0f);

        const float handleW = 10.0f;
        const float handleH = 16.0f;
        const float handleX = sliderPos - handleW * 0.5f;
        const float handleY = trackY - handleH * 0.5f;

        g.setColour (juce::Colour::fromRGB (40, 40, 48));
        g.fillRect (handleX, handleY, handleW, handleH);
        g.setColour (juce::Colour::fromRGB (60, 60, 72));
        g.drawRect (handleX, handleY, handleW, handleH, 1.0f);

        g.setColour (slider.isEnabled() ? accentColor : juce::Colours::grey);
        g.drawLine (handleX + handleW * 0.5f, handleY + 1.0f,
                    handleX + handleW * 0.5f, handleY + handleH - 1.0f, 1.5f);
    }
}

void LookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool /*isButtonDown*/,
                                 int buttonX, int buttonY, int buttonW, int buttonH,
                                 juce::ComboBox& box)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat();
    
    g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
    g.fillRect (bounds);

    g.setColour (box.findColour (juce::ComboBox::outlineColourId));
    g.drawRect (bounds, 1.0f);
    
    // Draw dropdown caret triangle
    g.setColour (juce::Colour::fromRGB (160, 160, 165));
    float arrowX = (float) buttonX + (float) buttonW * 0.5f;
    float arrowY = (float) buttonY + (float) buttonH * 0.5f;
    float arrowSize = 4.0f;
    
    juce::Path p;
    p.startNewSubPath (arrowX - arrowSize, arrowY - arrowSize * 0.5f);
    p.lineTo (arrowX + arrowSize, arrowY - arrowSize * 0.5f);
    p.lineTo (arrowX, arrowY + arrowSize * 0.5f);
    p.closeSubPath();
    
    g.fillPath (p);
}

void LookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    g.fillAll (juce::Colour::fromRGB (14, 14, 18));

    // Accent-tinted border
    g.setColour (accentColor.withAlpha (0.4f));
    g.drawRect (0, 0, width, height, 1);
}

void LookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area,
                                      bool isSeparator, bool isActive, bool isHighlighted, bool isChecked,
                                      bool /*hasSubMenu*/, const juce::String& text, const juce::String& shortcutKeyText,
                                      const juce::Drawable* /*icon*/, const juce::Colour* textColour)
{
    if (isSeparator)
    {
        g.setColour (juce::Colour::fromRGB (28, 28, 34));
        g.drawLine ((float) area.getX(), (float) area.getCentreY(), (float) area.getRight(), (float) area.getCentreY(), 1.0f);
        return;
    }
    
    auto r = area.reduced (1);
    
    if (isHighlighted && isActive)
    {
        g.setColour (juce::Colour::fromRGB (24, 48, 46));
        g.fillRect (r);
    }
    
    g.setFont (juce::FontOptions (juce::Font::getDefaultSansSerifFontName(), 11.0f, 0));
    
    juce::Colour txtCol;
    if (! isActive)
        txtCol = juce::Colour::fromRGB (80, 80, 85);
    else if (isHighlighted)
        txtCol = accentColor;
    else if (textColour != nullptr)
        txtCol = *textColour;
    else
        txtCol = juce::Colour::fromRGB (190, 190, 195);
        
    g.setColour (txtCol);
    
    auto textRect = r.withTrimmedLeft (24);
    g.drawText (text, textRect, juce::Justification::centredLeft, true);
    
    if (shortcutKeyText.isNotEmpty())
    {
        g.setColour (txtCol.withAlpha (0.5f));
        g.drawText (shortcutKeyText, r.withTrimmedRight (8), juce::Justification::centredRight, true);
    }
    
    if (isChecked)
    {
        auto dotSize = 5.0f;
        g.setColour (accentColor);
        g.fillEllipse ((float) r.getX() + 10.0f - dotSize * 0.5f, (float) r.getCentreY() - dotSize * 0.5f, dotSize, dotSize);
    }
}

void LookAndFeel::drawScrollbarButton (juce::Graphics& /*g*/, juce::ScrollBar& /*scrollbar*/, int /*width*/, int /*height*/,
                                        int /*buttonDirection*/, bool /*isScrollbarVertical*/, bool /*isMouseOverButton*/,
                                        bool /*isButtonDown*/)
{
    // No arrows on scrollbars for a clean modern flat style
}

void LookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height,
                                  bool isScrollbarVertical, int thumbPosition, int thumbSize,
                                  bool isMouseOver, bool /*isMouseDown*/)
{
    juce::ignoreUnused (scrollbar);
    
    // Background track is simple and blends in
    g.fillAll (juce::Colour::fromRGB (14, 14, 16));
    
    auto trackRect = juce::Rectangle<int> (x, y, width, height).toFloat();
    
    // Thumb layout
    juce::Rectangle<float> thumbRect;
    if (isScrollbarVertical)
    {
        float thumbW = 4.0f;
        float thumbX = trackRect.getCentreX() - thumbW * 0.5f;
        thumbRect = juce::Rectangle<float> (thumbX, (float) thumbPosition, thumbW, (float) thumbSize).reduced (0.0f, 1.0f);
    }
    else
    {
        float thumbH = 4.0f;
        float thumbY = trackRect.getCentreY() - thumbH * 0.5f;
        thumbRect = juce::Rectangle<float> ((float) thumbPosition, thumbY, (float) thumbSize, thumbH).reduced (1.0f, 0.0f);
    }
    
    g.setColour (isMouseOver ? juce::Colour::fromRGB (60, 60, 68) : juce::Colour::fromRGB (40, 40, 46));
    g.fillRect (thumbRect);
}

void LookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& textEditor)
{
    auto bounds = juce::Rectangle<int> (0, 0, width, height).toFloat();
    
    if (textEditor.hasKeyboardFocus (true))
    {
        g.setColour (textEditor.findColour (juce::TextEditor::focusedOutlineColourId));
        g.drawRect (bounds, 1.0f);
    }
    else
    {
        g.setColour (textEditor.findColour (juce::TextEditor::outlineColourId));
        g.drawRect (bounds, 1.0f);
    }
}

} // namespace dcr

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dcr
{

    struct EngineSettings;

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();
        ~LookAndFeel() override;

        // Pull accent / warning / critical colours from settings.  Called from
        // MainComponent on launch and whenever Settings is applied.
        void applyTheme (const EngineSettings& s);

        juce::Colour getAccent() const noexcept { return accentColor; }
        juce::Colour getWarning() const noexcept { return warningColor; }
        juce::Colour getCritical() const noexcept { return criticalColor; }

        void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

        void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPosProportional, float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override;

        void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minSliderPos, float maxSliderPos, const juce::Slider::SliderStyle style, juce::Slider& slider) override;

        void drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox& box) override;

        void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override;

        void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted, bool isChecked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override;

        void drawScrollbarButton (juce::Graphics& g, juce::ScrollBar& scrollbar, int width, int height, int buttonDirection, bool isScrollbarVertical, bool isMouseOverButton, bool isButtonDown) override;

        void drawScrollbar (juce::Graphics& g, juce::ScrollBar& scrollbar, int x, int y, int width, int height, bool isScrollbarVertical, int thumbPosition, int thumbSize, bool isMouseOver, bool isMouseDown) override;

        void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& textEditor) override;

    private:
        juce::Colour accentColor { 0xFF00FFD2 };
        juce::Colour warningColor { 0xFFFFCC00 };
        juce::Colour criticalColor { 0xFFFF3B30 };
    };

} // namespace dcr

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/**
    How a console's controls look, after the desk itself:
    - EMI TG12345: grey metalwork; black bakelite pointer knobs with fluted skirts, the scale printed
      in black; grey push buttons that light red when on; black legend plates for the switches.
    - Harrison 4032: a black panel; grey knobs whose coloured caps say what they do (red gains,
      blue frequencies, green Q, yellow aux and orange cue sends, white levels); illuminated square
      switches, lit in their function's colour; white printing.
    Pop-up menus keep the app's look. Set it on a console editor; its children inherit it.
*/
class ConsoleLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    enum class Style
    {
        emi,
        harrison
    };

    explicit ConsoleLookAndFeel(Style style);

    [[nodiscard]] Style getStyle() const noexcept { return style_; }

    /** A Harrison knob's cap colour for what it controls (its parameter ID). */
    [[nodiscard]] static juce::Colour harrisonCap(const juce::String& parameterId);
    /** The colour a Harrison switch lights in. */
    [[nodiscard]] static juce::Colour harrisonLamp(const juce::String& parameterId);
    /** The parameter ID behind a control (its ParameterControl's), or empty. */
    [[nodiscard]] static juce::String parameterIdOf(const juce::Component& control);

    // Knobs
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPosProportional, float rotaryStartAngle,
                          float rotaryEndAngle, juce::Slider& slider) override;

    // Switches, page keys and the console's other buttons
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour, bool highlighted,
                              bool down) override;
    void drawButtonText(juce::Graphics& g, juce::TextButton& button, bool highlighted, bool down) override;
    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

    // Selector switches (legend plates)
    void drawComboBox(juce::Graphics& g, int width, int height, bool isButtonDown, int buttonX, int buttonY, int buttonW, int buttonH,
                      juce::ComboBox& box) override;
    juce::Font getComboBoxFont(juce::ComboBox& box) override;
    void positionComboBoxText(juce::ComboBox& box, juce::Label& label) override;

    // Menus: the app's
    void drawPopupMenuBackground(juce::Graphics& g, int width, int height) override;
    void drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive, bool isHighlighted,
                           bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcutKeyText,
                           const juce::Drawable* icon, const juce::Colour* textColour) override;
    void drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName) override;
    juce::Font getPopupMenuFont() override;
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                   int& idealHeight) override;
    int getPopupMenuBorderSize() override;
    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;

private:
    /** The app's look (for menus), unless this is it. */
    [[nodiscard]] juce::LookAndFeel* app();

    void drawEmiKnob(juce::Graphics& g, juce::Rectangle<float> area, float angle, float startAngle, float endAngle, bool enabled) const;
    void drawHarrisonKnob(juce::Graphics& g, juce::Rectangle<float> area, float angle, float startAngle, float endAngle, juce::Colour cap,
                          bool enabled) const;

    Style style_;
};

} // namespace dojo::ui

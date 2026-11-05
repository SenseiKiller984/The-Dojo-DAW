#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/**
    The Dojo's dark look-and-feel.

    - Installs the dark colour scheme for every stock JUCE widget.
    - Maps the default sans-serif / monospaced font requests onto the embedded Inter and
      JetBrains Mono typefaces, so text metrics are identical on Windows and Linux.
    - Draws flat, low-contrast controls with a vermilion accent, and a custom (non-native) window
      title bar so the chrome is the same on both platforms too.

    Install once at start-up with juce::LookAndFeel::setDefaultLookAndFeel().
*/
class DojoLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    DojoLookAndFeel();
    ~DojoLookAndFeel() override;

    juce::Typeface::Ptr getTypefaceForFont(const juce::Font& font) override;

    // Buttons
    void drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& backgroundColour,
                              bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;
    juce::Font getTextButtonFont(juce::TextButton& button, int buttonHeight) override;

    // Sliders (vertical faders)
    void drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPos,
                          float minSliderPos, float maxSliderPos, juce::Slider::SliderStyle style,
                          juce::Slider& slider) override;
    int getSliderThumbRadius(juce::Slider& slider) override;

    /** Knobs. Sliders whose range is symmetric around zero (pan) draw their arc from the centre. */
    void drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float sliderPosProportional,
                          float rotaryStartAngle, float rotaryEndAngle, juce::Slider& slider) override;

    // Combo boxes and menus
    juce::Font getComboBoxFont(juce::ComboBox& box) override;
    juce::Font getMenuBarFont(juce::MenuBarComponent& bar, int itemIndex,
                              const juce::String& itemText) override;
    void drawMenuBarBackground(juce::Graphics& g, int width, int height, bool isMouseOverBar,
                               juce::MenuBarComponent& bar) override;

    // Text
    juce::Font getLabelFont(juce::Label& label) override;
    juce::Font getPopupMenuFont() override;

    // Window chrome
    void drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g, int w, int h,
                                    int titleSpaceX, int titleSpaceW, const juce::Image* icon,
                                    bool drawTitleTextOnLeft) override;
    juce::Button* createDocumentWindowButton(int buttonType) override;

    /** Draws the ensō brand mark (a brushed Zen circle) centred in `area`. */
    static void drawBrandMark(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour);

private:
    juce::Typeface::Ptr sansRegular_;
    juce::Typeface::Ptr sansSemiBold_;
    juce::Typeface::Ptr monoMedium_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DojoLookAndFeel)
};

} // namespace dojo::ui

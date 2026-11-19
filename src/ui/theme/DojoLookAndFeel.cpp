#include "ui/theme/DojoLookAndFeel.h"

#include "ui/theme/DojoTheme.h"

#include <DojoBinaryData.h>

#include <cmath>

namespace dojo::ui
{
namespace
{
juce::Typeface::Ptr loadEmbeddedTypeface(const char* data, int size)
{
    return juce::Typeface::createSystemTypefaceFor(data, static_cast<size_t>(size));
}

/** Minimal, monochrome title-bar button (minimise / maximise-restore / close). */
class WindowButton final : public juce::Button
{
public:
    WindowButton(const juce::String& name, int buttonType)
        : juce::Button(name)
        , type_(buttonType)
    {
        setWantsKeyboardFocus(false);
    }

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                     bool shouldDrawButtonAsDown) override
    {
        using namespace theme;

        const bool isClose = type_ == juce::DocumentWindow::closeButton;
        const auto bounds = getLocalBounds().toFloat();

        if (shouldDrawButtonAsHighlighted || shouldDrawButtonAsDown)
        {
            g.setColour(isClose ? colour::accent.withAlpha(shouldDrawButtonAsDown ? 1.0f : 0.85f)
                                : colour::controlHover);
            g.fillRect(bounds);
        }

        const auto icon = bounds.withSizeKeepingCentre(10.0f, 10.0f);
        g.setColour(isClose && shouldDrawButtonAsHighlighted ? colour::textPrimary : colour::textSecondary);

        if (type_ == juce::DocumentWindow::minimiseButton)
        {
            g.fillRect(icon.withSizeKeepingCentre(icon.getWidth(), 1.2f));
        }
        else if (type_ == juce::DocumentWindow::maximiseButton)
        {
            if (getToggleState()) // restore: two overlapping frames
            {
                g.drawRect(icon.withTrimmedTop(3.0f).withTrimmedRight(3.0f), 1.2f);
                g.drawRect(icon.withTrimmedBottom(3.0f).withTrimmedLeft(3.0f), 1.2f);
            }
            else
            {
                g.drawRect(icon, 1.2f);
            }
        }
        else
        {
            g.drawLine({icon.getTopLeft(), icon.getBottomRight()}, 1.3f);
            g.drawLine({icon.getBottomLeft(), icon.getTopRight()}, 1.3f);
        }
    }

private:
    int type_;
};
} // namespace

DojoLookAndFeel::DojoLookAndFeel()
    : sansRegular_(
          loadEmbeddedTypeface(DojoBinaryData::InterRegular_ttf, DojoBinaryData::InterRegular_ttfSize))
    , sansSemiBold_(
          loadEmbeddedTypeface(DojoBinaryData::InterSemiBold_ttf, DojoBinaryData::InterSemiBold_ttfSize))
    , monoMedium_(loadEmbeddedTypeface(DojoBinaryData::JetBrainsMonoMedium_ttf,
                                       DojoBinaryData::JetBrainsMonoMedium_ttfSize))
{
    refreshColours();
}

void DojoLookAndFeel::refreshColours()
{
    using namespace theme;

    setColourScheme({
        colour::windowBackground, // windowBackground
        colour::panel,            // widgetBackground
        colour::panelRaised,      // menuBackground
        colour::outline,          // outline
        colour::textPrimary,      // defaultText
        colour::control,          // defaultFill
        colour::textPrimary,      // highlightedText
        colour::accent,           // highlightedFill
        colour::textPrimary,      // menuText
    });

    setColour(juce::ResizableWindow::backgroundColourId, colour::windowBackground);
    setColour(juce::DocumentWindow::textColourId, colour::textSecondary);

    setColour(juce::TextButton::buttonColourId, colour::control);
    setColour(juce::TextButton::buttonOnColourId, colour::accent);
    setColour(juce::TextButton::textColourOffId, colour::textSecondary);
    setColour(juce::TextButton::textColourOnId, colour::textPrimary);

    setColour(juce::Label::textColourId, colour::textPrimary);

    setColour(juce::Slider::backgroundColourId, colour::meterBackground);
    setColour(juce::Slider::trackColourId, colour::outlineStrong);
    setColour(juce::Slider::thumbColourId, colour::textPrimary);
    setColour(juce::Slider::textBoxTextColourId, colour::textPrimary);
    setColour(juce::Slider::textBoxBackgroundColourId, colour::panel);
    setColour(juce::Slider::textBoxOutlineColourId, colour::outline);

    setColour(juce::PopupMenu::backgroundColourId, colour::panelRaised);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, colour::accent);
    setColour(juce::TooltipWindow::backgroundColourId, colour::panelHeader);
    setColour(juce::TooltipWindow::textColourId, colour::textPrimary);
    setColour(juce::TooltipWindow::outlineColourId, colour::outlineStrong);
    setColour(juce::ScrollBar::thumbColourId, colour::outlineStrong);

    setColour(juce::ComboBox::backgroundColourId, colour::control);
    setColour(juce::ComboBox::outlineColourId, colour::outlineStrong);
    setColour(juce::ComboBox::textColourId, colour::textPrimary);
    setColour(juce::ComboBox::arrowColourId, colour::textSecondary);
    setColour(juce::ComboBox::focusedOutlineColourId, colour::accent);

    setColour(juce::TextEditor::backgroundColourId, colour::meterBackground);
    setColour(juce::TextEditor::textColourId, colour::textPrimary);
    setColour(juce::TextEditor::highlightColourId, colour::accent.withAlpha(0.45f));
    setColour(juce::TextEditor::highlightedTextColourId, colour::textPrimary);
    setColour(juce::TextEditor::outlineColourId, colour::outlineStrong);
    setColour(juce::TextEditor::focusedOutlineColourId, colour::accent);
    setColour(juce::CaretComponent::caretColourId, colour::accent);

    setColour(juce::ListBox::backgroundColourId, colour::meterBackground);
    setColour(juce::ListBox::outlineColourId, colour::outline);
    setColour(juce::ToggleButton::textColourId, colour::textPrimary);
    setColour(juce::ToggleButton::tickColourId, colour::accent);
    setColour(juce::ToggleButton::tickDisabledColourId, colour::textMuted);

    setColour(juce::AlertWindow::backgroundColourId, colour::panelRaised);
    setColour(juce::AlertWindow::textColourId, colour::textPrimary);
    setColour(juce::AlertWindow::outlineColourId, colour::outlineStrong);

    setColour(juce::PopupMenu::textColourId, colour::textPrimary);
    setColour(juce::PopupMenu::headerTextColourId, colour::textMuted);
    setColour(juce::PopupMenu::highlightedTextColourId, colour::textPrimary);

    setColour(juce::Slider::rotarySliderFillColourId, colour::accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, colour::meterBackground);
}

DojoLookAndFeel::~DojoLookAndFeel() = default;

//==================================================================================================
// Fonts

juce::Typeface::Ptr DojoLookAndFeel::getTypefaceForFont(const juce::Font& font)
{
    const auto& name = font.getTypefaceName();

    if (name == juce::Font::getDefaultMonospacedFontName())
        return monoMedium_;

    if (name == juce::Font::getDefaultSansSerifFontName())
        return font.isBold() ? sansSemiBold_ : sansRegular_;

    return LookAndFeel_V4::getTypefaceForFont(font);
}

void DojoLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool shouldDrawButtonAsHighlighted,
                                     bool shouldDrawButtonAsDown)
{
    const bool on = button.getToggleState();
    const auto fill = button.findColour(on ? juce::TextButton::buttonOnColourId : juce::TextButton::buttonColourId);
    const auto wanted = button.findColour(on ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId);
    const auto colour = fill.isOpaque() || fill.getAlpha() > 160 ? theme::readableOn(fill, wanted) : wanted;

    // As LookAndFeel_V4 draws it, with the colour chosen above.
    const auto font = getTextButtonFont(button, button.getHeight());
    g.setFont(font);
    g.setColour(colour.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));
    const int yIndent = std::min(4, button.proportionOfHeight(0.3f));
    const int cornerSize = std::min(button.getHeight(), button.getWidth()) / 2;
    const int fontHeight = juce::roundToInt(font.getHeight() * 0.6f);
    const int leftIndent = std::min(fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent = std::min(fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
    const int textWidth = button.getWidth() - leftIndent - rightIndent;
    if (textWidth > 0)
        g.drawFittedText(button.getButtonText(), leftIndent, yIndent, textWidth, button.getHeight() - yIndent * 2,
                         juce::Justification::centred, 2);
    juce::ignoreUnused(shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
}

juce::Font DojoLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return theme::sansBold(juce::jmin(13.0f, static_cast<float>(buttonHeight) * 0.5f));
}

juce::Font DojoLookAndFeel::getLabelFont(juce::Label& label)
{
    return label.getFont();
}

juce::Font DojoLookAndFeel::getPopupMenuFont()
{
    return theme::sans(14.0f);
}

//==================================================================================================
// Buttons

void DojoLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                           const juce::Colour& backgroundColour,
                                           bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto fill = backgroundColour;

    if (!button.isEnabled())
        fill = fill.withMultipliedAlpha(0.5f);
    else if (shouldDrawButtonAsDown)
        fill = fill.brighter(0.15f);
    else if (shouldDrawButtonAsHighlighted)
        fill = fill.brighter(0.08f);

    g.setColour(fill);
    g.fillRoundedRectangle(bounds, theme::metrics::cornerRadius);

    g.setColour(button.getToggleState() ? fill.brighter(0.25f) : theme::colour::outlineStrong);
    g.drawRoundedRectangle(bounds, theme::metrics::cornerRadius, 1.0f);
}

//==================================================================================================
// Faders

int DojoLookAndFeel::getSliderThumbRadius(juce::Slider& slider)
{
    return slider.isVertical() ? 14 : LookAndFeel_V4::getSliderThumbRadius(slider);
}

void DojoLookAndFeel::drawLinearSlider(juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPos, float minSliderPos, float maxSliderPos,
                                       juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearVertical)
    {
        LookAndFeel_V4::drawLinearSlider(g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style,
                                         slider);
        return;
    }

    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float centreX = area.getCentreX();

    // Groove.
    const auto groove =
        juce::Rectangle<float>(4.0f, area.getHeight()).withCentre({centreX, area.getCentreY()});
    g.setColour(theme::colour::meterBackground);
    g.fillRoundedRectangle(groove, 2.0f);
    g.setColour(theme::colour::outline);
    g.drawRoundedRectangle(groove, 2.0f, 1.0f);

    // Fader cap: a flat, grippy block with a centre index line.
    const float capWidth = juce::jmin(area.getWidth() - 4.0f, 34.0f);
    const auto cap = juce::Rectangle<float>(capWidth, 22.0f).withCentre({centreX, sliderPos});

    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillRoundedRectangle(cap.translated(0.0f, 2.0f), 3.0f);

    juce::ColourGradient body(theme::colour::controlHover.brighter(0.15f), cap.getTopLeft(),
                              theme::colour::control.darker(0.2f), cap.getBottomLeft(), false);
    g.setGradientFill(body);
    g.fillRoundedRectangle(cap, 3.0f);

    g.setColour(theme::colour::outlineStrong.brighter(0.2f));
    g.drawRoundedRectangle(cap, 3.0f, 1.0f);

    g.setColour(slider.isEnabled() ? theme::colour::accent : theme::colour::textMuted);
    g.fillRect(cap.withSizeKeepingCentre(capWidth - 8.0f, 2.0f));
}

void DojoLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height,
                                       float sliderPosProportional, float rotaryStartAngle,
                                       float rotaryEndAngle, juce::Slider& slider)
{
    const auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat().reduced(2.0f);
    const float radius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
    const auto centre = bounds.getCentre();
    const float trackRadius = radius - 2.0f;
    const float angle = rotaryStartAngle + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);

    const auto range = slider.getRange();
    const bool bipolar = juce::approximatelyEqual(range.getStart(), -range.getEnd());
    const float zeroAngle = bipolar ? (rotaryStartAngle + rotaryEndAngle) * 0.5f : rotaryStartAngle;

    juce::Path track;
    track.addCentredArc(centre.x, centre.y, trackRadius, trackRadius, 0.0f, rotaryStartAngle, rotaryEndAngle,
                        true);
    g.setColour(theme::colour::meterBackground);
    g.strokePath(track,
                 juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    if (std::abs(angle - zeroAngle) > 0.01f)
    {
        juce::Path value;
        value.addCentredArc(centre.x, centre.y, trackRadius, trackRadius, 0.0f, juce::jmin(zeroAngle, angle),
                            juce::jmax(zeroAngle, angle), true);
        g.setColour(slider.isEnabled() ? theme::colour::accent : theme::colour::textMuted);
        g.strokePath(value,
                     juce::PathStrokeType(3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    const float capRadius = trackRadius - 4.0f;
    g.setColour(theme::colour::control);
    g.fillEllipse(juce::Rectangle<float>(capRadius * 2.0f, capRadius * 2.0f).withCentre(centre));
    g.setColour(theme::colour::outlineStrong);
    g.drawEllipse(juce::Rectangle<float>(capRadius * 2.0f, capRadius * 2.0f).withCentre(centre), 1.0f);

    const juce::Point<float> tip(centre.x + std::sin(angle) * (capRadius - 2.0f),
                                 centre.y - std::cos(angle) * (capRadius - 2.0f));
    g.setColour(theme::colour::textPrimary);
    g.drawLine({centre, tip}, 2.0f);
}

juce::Font DojoLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return theme::sans(13.0f);
}

juce::Font DojoLookAndFeel::getMenuBarFont(juce::MenuBarComponent&, int, const juce::String&)
{
    return theme::sans(13.5f);
}

void DojoLookAndFeel::drawMenuBarBackground(juce::Graphics& g, int width, int height, bool,
                                            juce::MenuBarComponent&)
{
    g.fillAll(theme::colour::panel);
    g.setColour(theme::colour::outline);
    g.fillRect(0, height - 1, width, 1);
}

//==================================================================================================
// Window chrome

void DojoLookAndFeel::drawBrandMark(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour)
{
    // An ensō: a single brushed circle left slightly open.
    const auto square = area.withSizeKeepingCentre(juce::jmin(area.getWidth(), area.getHeight()),
                                                   juce::jmin(area.getWidth(), area.getHeight()));
    const float radius = square.getWidth() * 0.5f - 1.5f;
    const auto centre = square.getCentre();

    juce::Path stroke;
    constexpr float start = juce::MathConstants<float>::pi * 0.15f;
    constexpr float end = juce::MathConstants<float>::twoPi * 0.94f;
    constexpr int segments = 48;

    // Vary the brush width along the stroke: heavy at the start, tapering to a dry tail.
    for (int i = 0; i < segments; ++i)
    {
        const float t0 = static_cast<float>(i) / static_cast<float>(segments);
        const float t1 = static_cast<float>(i + 1) / static_cast<float>(segments);
        const float a0 = start + (end - start) * t0;
        const float a1 = start + (end - start) * t1;
        const float thickness = radius * (0.30f - 0.22f * t0);

        juce::Path arc;
        arc.addCentredArc(centre.x, centre.y, radius, radius, 0.0f, a0, a1, true);

        juce::Path brushed;
        juce::PathStrokeType(thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded)
            .createStrokedPath(brushed, arc);
        stroke.addPath(brushed);
    }

    g.setColour(colour);
    g.fillPath(stroke);
}

juce::Button* DojoLookAndFeel::createDocumentWindowButton(int buttonType)
{
    switch (buttonType)
    {
    case juce::DocumentWindow::minimiseButton:
        return new WindowButton("minimise", buttonType);
    case juce::DocumentWindow::maximiseButton:
        return new WindowButton("maximise", buttonType);
    case juce::DocumentWindow::closeButton:
        return new WindowButton("close", buttonType);
    default:
        break;
    }

    jassertfalse;
    return nullptr;
}

void DojoLookAndFeel::drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g, int w,
                                                 int h, int titleSpaceX, int titleSpaceW, const juce::Image*,
                                                 bool)
{
    const bool active = window.isActiveWindow();

    g.setColour(theme::colour::panel);
    g.fillRect(0, 0, w, h);
    g.setColour(theme::colour::outline);
    g.fillRect(0, h - 1, w, 1);

    const auto markArea = juce::Rectangle<float>(static_cast<float>(titleSpaceX) + 4.0f, 0.0f,
                                                 static_cast<float>(h), static_cast<float>(h))
                              .reduced(6.0f);
    drawBrandMark(g, markArea, active ? theme::colour::accent : theme::colour::textMuted);

    g.setColour(active ? theme::colour::textSecondary : theme::colour::textMuted);
    g.setFont(theme::sansBold(13.0f));
    g.drawText(window.getName(),
               juce::Rectangle<int>(titleSpaceX + h + 8, 0, juce::jmax(0, titleSpaceW - h - 8), h),
               juce::Justification::centredLeft, true);
}

} // namespace dojo::ui

#include "ui/devices/ConsoleLookAndFeel.h"

#include "ui/devices/HardwarePainting.h"
#include "ui/devices/PagedEditor.h"
#include "ui/devices/ParameterControl.h"
#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

// EMI: grey metalwork, black ink, grey and red buttons.
const juce::Colour kEmiInk{0xff1b1c1c};
const juce::Colour kEmiValue{0xff303231};
const juce::Colour kEmiButton{0xffb9bdbb};
const juce::Colour kEmiRed{0xffd23c2e};
const juce::Colour kEmiPlate{0xff161717};
const juce::Colour kEmiFrame{0xff7d8280};
const juce::Colour kEmiLegend{0xfff0ece0};

// Harrison: white printing on black, coloured caps, illuminated switches.
const juce::Colour kHarrisonPrint{0xffe9e5d9};
const juce::Colour kHarrisonValue{0xffdac89c};
const juce::Colour kHarrisonPlate{0xff0e0f10};
const juce::Colour kHarrisonFrame{0xff44474c};
const juce::Colour kCream{0xfff0e3c0};
const juce::Colour kAmber{0xfff2b03a};

juce::Point<float> pointAt(juce::Point<float> centre, float angle, float radius)
{
    return {centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle)};
}

juce::Rectangle<float> circle(juce::Point<float> centre, float radius)
{
    return juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(centre);
}

enum class Kind
{
    pageKey,
    parameterSwitch,
    other
};

Kind kindOf(const juce::Button& button)
{
    if (button.getRadioGroupId() == PagedEditor::kTabGroup)
        return Kind::pageKey;
    if (dynamic_cast<const ParameterControl*>(button.getParentComponent()) != nullptr)
        return Kind::parameterSwitch;
    return Kind::other;
}
} // namespace

ConsoleLookAndFeel::ConsoleLookAndFeel(Style style)
    : style_(style)
{
    const bool emi = style_ == Style::emi;
    setColour(ParameterControl::captionColourId, emi ? kEmiInk : kHarrisonPrint);
    setColour(ParameterControl::valueColourId, emi ? kEmiValue : kHarrisonValue);
    setColour(juce::ComboBox::textColourId, emi ? kEmiLegend : kCream);
    setColour(juce::ComboBox::arrowColourId, emi ? kEmiLegend : kCream);
    setColour(juce::ComboBox::backgroundColourId, emi ? kEmiPlate : kHarrisonPlate);
    setColour(juce::ComboBox::outlineColourId, emi ? kEmiFrame : kHarrisonFrame);
}

juce::Colour ConsoleLookAndFeel::harrisonCap(const juce::String& id)
{
    if (id.contains("gain") || id == "linetrim")
        return juce::Colour(0xffc8392d); // red: gains
    if (id.contains("freq") || id == "hpf" || id == "lpf")
        return juce::Colour(0xff2f62b5); // blue: frequencies
    if (id == "lmfq" || id == "hmfq")
        return juce::Colour(0xff2f8a56); // green: Q
    if (id.startsWith("aux"))
        return juce::Colour(0xffe2b12f); // yellow: aux sends
    if (id.startsWith("cue"))
        return juce::Colour(0xffe0782b); // orange: cue sends
    if (id.contains("pan") || id == "frontback")
        return juce::Colour(0xff35383d); // black: panning
    if (id.contains("level") || id == "fader" || id == "direct")
        return juce::Colour(0xffe6e2d8); // white: levels
    return juce::Colour(0xff70747a);
}

juce::Colour ConsoleLookAndFeel::harrisonLamp(const juce::String& id)
{
    if (id == "phantom")
        return juce::Colour(0xffec4130);
    if (id == "phase" || id.endsWith("pre"))
        return juce::Colour(0xfff3d03c);
    if (id == "eqin" || id == "filtersin")
        return juce::Colour(0xff4fd27a);
    if (id == "mix")
        return juce::Colour(0xff4f9bf0);
    if (id == "pad" || id.endsWith("shelf") || id == "transformer" || id == "noise")
        return juce::Colour(0xffefeadc);
    return kAmber;
}

juce::String ConsoleLookAndFeel::parameterIdOf(const juce::Component& control)
{
    for (auto* c = control.getParentComponent(); c != nullptr; c = c->getParentComponent())
        if (auto* parameterControl = dynamic_cast<ParameterControl*>(c))
        {
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(&parameterControl->getParameter()))
                return withId->paramID;
            return {};
        }
    return {};
}

//==================================================================================================
// Knobs

void ConsoleLookAndFeel::drawRotarySlider(juce::Graphics& g, int x, int y, int width, int height, float position, float startAngle,
                                          float endAngle, juce::Slider& slider)
{
    const auto area = juce::Rectangle<int>(x, y, width, height).toFloat();
    const float angle = startAngle + position * (endAngle - startAngle);
    if (style_ == Style::emi)
        drawEmiKnob(g, area, angle, startAngle, endAngle, slider.isEnabled());
    else
        drawHarrisonKnob(g, area, angle, startAngle, endAngle, harrisonCap(parameterIdOf(slider)), slider.isEnabled());
}

void ConsoleLookAndFeel::drawEmiKnob(juce::Graphics& g, juce::Rectangle<float> area, float angle, float startAngle, float endAngle,
                                     bool enabled) const
{
    const auto centre = area.getCentre();
    const float radius = std::min(area.getWidth(), area.getHeight()) * 0.5f - 4.5f;
    if (radius < 4.0f)
        return;

    // The scale, printed in black on the panel.
    g.setColour(kEmiInk.withAlpha(0.85f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = startAngle + (endAngle - startAngle) * static_cast<float>(i) / 10.0f;
        const bool major = i % 5 == 0;
        g.drawLine(juce::Line<float>(pointAt(centre, a, radius + 1.2f), pointAt(centre, a, radius + (major ? 4.2f : 3.0f))), major ? 1.3f : 0.8f);
    }

    // The fluted skirt and the bakelite body.
    g.setColour(juce::Colours::black.withAlpha(0.35f));
    g.fillEllipse(circle(centre.translated(0.8f, 1.4f), radius));
    juce::ColourGradient skirt(juce::Colour(0xff4c4c4c), centre.x, centre.y - radius, juce::Colour(0xff0c0c0c), centre.x, centre.y + radius, false);
    g.setGradientFill(skirt);
    g.fillEllipse(circle(centre, radius));
    g.setColour(juce::Colours::black.withAlpha(0.6f));
    for (int k = 0; k < 28; ++k)
    {
        const float a = angle + static_cast<float>(k) * kTwoPi / 28.0f;
        g.drawLine(juce::Line<float>(pointAt(centre, a, radius * 0.86f), pointAt(centre, a, radius * 0.99f)), 1.0f);
    }
    const float body = radius * 0.74f;
    juce::ColourGradient bakelite(juce::Colour(0xff3d3d3d), centre.x - body * 0.45f, centre.y - body * 0.55f, juce::Colour(0xff060606),
                                  centre.x + body * 0.7f, centre.y + body * 0.8f, true);
    g.setGradientFill(bakelite);
    g.fillEllipse(circle(centre, body));
    g.setColour(juce::Colours::white.withAlpha(0.09f));
    g.fillEllipse(circle(centre.translated(-body * 0.2f, -body * 0.28f), body * 0.55f));

    // The pointer: a white line inlaid across the top.
    g.setColour(juce::Colour(0xfff3f0e7).withAlpha(enabled ? 1.0f : 0.5f));
    g.drawLine(juce::Line<float>(pointAt(centre, angle, body * 0.15f), pointAt(centre, angle, radius * 0.95f)), 2.0f);
}

void ConsoleLookAndFeel::drawHarrisonKnob(juce::Graphics& g, juce::Rectangle<float> area, float angle, float startAngle, float endAngle,
                                          juce::Colour cap, bool enabled) const
{
    const auto centre = area.getCentre();
    const float radius = std::min(area.getWidth(), area.getHeight()) * 0.5f - 4.5f;
    if (radius < 4.0f)
        return;

    // The scale: white dots printed on the black panel.
    g.setColour(kHarrisonPrint.withAlpha(0.85f));
    for (int i = 0; i <= 10; ++i)
    {
        const float a = startAngle + (endAngle - startAngle) * static_cast<float>(i) / 10.0f;
        const float size = i % 5 == 0 ? 2.2f : 1.5f;
        g.fillEllipse(circle(pointAt(centre, a, radius + 3.0f), size * 0.5f));
    }

    // The grey knob with its ridged skirt.
    g.setColour(juce::Colours::black.withAlpha(0.5f));
    g.fillEllipse(circle(centre.translated(0.8f, 1.5f), radius));
    juce::ColourGradient grey(juce::Colour(0xffc0c4c9), centre.x, centre.y - radius, juce::Colour(0xff4b4f54), centre.x, centre.y + radius, false);
    g.setGradientFill(grey);
    g.fillEllipse(circle(centre, radius));
    g.setColour(juce::Colours::black.withAlpha(0.22f));
    for (int k = 0; k < 24; ++k)
    {
        const float a = angle + static_cast<float>(k) * kTwoPi / 24.0f;
        g.drawLine(juce::Line<float>(pointAt(centre, a, radius * 0.72f), pointAt(centre, a, radius * 0.98f)), 0.8f);
    }
    g.setColour(juce::Colours::black.withAlpha(0.55f));
    g.drawEllipse(circle(centre, radius), 0.8f);

    // The index line on the skirt, then the coloured cap.
    g.setColour(juce::Colour(0xfffafaf6).withAlpha(enabled ? 1.0f : 0.5f));
    g.drawLine(juce::Line<float>(pointAt(centre, angle, radius * 0.5f), pointAt(centre, angle, radius * 0.97f)), 1.8f);
    const float capRadius = radius * 0.56f;
    juce::ColourGradient colour(cap.brighter(0.5f), centre.x - capRadius * 0.4f, centre.y - capRadius * 0.6f, cap.darker(0.35f),
                                centre.x + capRadius * 0.5f, centre.y + capRadius * 0.7f, false);
    g.setGradientFill(colour);
    g.fillEllipse(circle(centre, capRadius));
    g.setColour(cap.darker(0.7f));
    g.drawEllipse(circle(centre, capRadius), 0.7f);
    g.setColour(juce::Colours::white.withAlpha(0.22f));
    g.fillEllipse(circle(centre.translated(-capRadius * 0.25f, -capRadius * 0.3f), capRadius * 0.38f));
}

//==================================================================================================
// Buttons

void ConsoleLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down)
{
    auto area = button.getLocalBounds().toFloat().reduced(0.5f);
    const bool on = button.getToggleState();
    const auto kind = kindOf(button);

    if (kind == Kind::pageKey)
    {
        // A legend: black, lit cream while its page shows.
        hardware::paintLamp(g, area.reduced(0.5f, 1.5f), kCream, on, {}, 2.0f);
        if (highlighted && !on)
        {
            g.setColour(juce::Colours::white.withAlpha(0.07f));
            g.fillRoundedRectangle(area.reduced(1.5f, 2.5f), 2.0f);
        }
        return;
    }

    if (style_ == Style::emi)
    {
        // A push button: a grey cap, lit red when on.
        g.setColour(juce::Colour(0xff202121));
        g.fillRoundedRectangle(area, 3.0f);
        auto cap = area.reduced(1.6f);
        if (down)
            cap = cap.reduced(0.5f).translated(0.0f, 0.6f);
        const auto base = on ? kEmiRed : kEmiButton;
        juce::ColourGradient body(base.brighter(on ? 0.4f : 0.25f), 0.0f, cap.getY(), base.darker(0.32f), 0.0f, cap.getBottom(), false);
        g.setGradientFill(body);
        g.fillRoundedRectangle(cap, 2.0f);
        if (on)
        {
            g.setColour(kEmiRed.withAlpha(0.3f));
            g.drawRoundedRectangle(area.expanded(0.8f), 4.0f, 1.6f);
        }
        g.setColour(juce::Colours::white.withAlpha(highlighted && !down ? 0.38f : 0.25f));
        g.drawHorizontalLine(juce::roundToInt(cap.getY() + 1.0f), cap.getX() + 2.0f, cap.getRight() - 2.0f);
        return;
    }

    // Harrison: an illuminated square switch, in its function's colour.
    const auto lamp = kind == Kind::parameterSwitch ? harrisonLamp(parameterIdOf(button)) : kAmber;
    hardware::paintLamp(g, down ? area.reduced(0.6f) : area, lamp, on, {}, 2.0f);
    if (highlighted && !on)
    {
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        g.fillRoundedRectangle(area.reduced(1.5f), 2.0f);
    }
}

void ConsoleLookAndFeel::drawButtonText(juce::Graphics& g, juce::TextButton& button, bool, bool down)
{
    const bool on = button.getToggleState();
    const auto kind = kindOf(button);
    juce::Colour colour;
    if (kind == Kind::pageKey)
        colour = on ? juce::Colour(0xff1a1712) : (style_ == Style::emi ? juce::Colour(0xffc9c6bc) : juce::Colour(0xffb9b5aa));
    else if (style_ == Style::emi)
        colour = on ? juce::Colours::white : kEmiInk;
    else
    {
        const auto lamp = kind == Kind::parameterSwitch ? harrisonLamp(parameterIdOf(button)) : kAmber;
        colour = on ? theme::textOn(lamp.brighter(0.3f)) : lamp.interpolatedWith(juce::Colours::white, 0.35f).withAlpha(0.8f);
    }
    g.setFont(getTextButtonFont(button, button.getHeight()));
    g.setColour(colour.withMultipliedAlpha(button.isEnabled() ? 1.0f : 0.5f));
    auto area = button.getLocalBounds().reduced(3, 1);
    if (down)
        area.translate(0, 1);
    g.drawFittedText(button.getButtonText(), area, juce::Justification::centred, 2, 0.75f);
}

juce::Font ConsoleLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return theme::label(std::min(10.0f, static_cast<float>(buttonHeight) * 0.45f));
}

//==================================================================================================
// Selector switches

void ConsoleLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto area = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    const bool emi = style_ == Style::emi;
    // A black legend plate in its frame.
    g.setColour(emi ? kEmiFrame : kHarrisonFrame);
    g.fillRoundedRectangle(area, 3.0f);
    const auto plate = area.reduced(1.3f);
    juce::ColourGradient black(juce::Colour(emi ? 0xff2a2b2b : 0xff1d1e20), 0.0f, plate.getY(), emi ? kEmiPlate : kHarrisonPlate, 0.0f,
                               plate.getBottom(), false);
    g.setGradientFill(black);
    g.fillRoundedRectangle(plate, 2.2f);
    g.setColour(juce::Colours::white.withAlpha(0.08f));
    g.drawHorizontalLine(juce::roundToInt(plate.getY() + 1.0f), plate.getX() + 2.0f, plate.getRight() - 2.0f);

    const auto arrow = juce::Rectangle<float>(static_cast<float>(width) - 13.0f, 0.0f, 9.0f, static_cast<float>(height)).withSizeKeepingCentre(7.0f, 4.0f);
    juce::Path p;
    p.addTriangle(arrow.getX(), arrow.getY(), arrow.getRight(), arrow.getY(), arrow.getCentreX(), arrow.getBottom());
    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withAlpha(box.isEnabled() ? 0.85f : 0.4f));
    g.fillPath(p);
}

juce::Font ConsoleLookAndFeel::getComboBoxFont(juce::ComboBox& box)
{
    return theme::label(std::min(10.0f, static_cast<float>(box.getHeight()) * 0.44f));
}

void ConsoleLookAndFeel::positionComboBoxText(juce::ComboBox& box, juce::Label& label)
{
    label.setBounds(2, 1, std::max(0, box.getWidth() - 16), std::max(0, box.getHeight() - 2));
    label.setFont(getComboBoxFont(box));
    label.setJustificationType(juce::Justification::centred);
}

//==================================================================================================
// Menus: the app's look

juce::LookAndFeel* ConsoleLookAndFeel::app()
{
    auto& base = juce::LookAndFeel::getDefaultLookAndFeel();
    return &base == this ? nullptr : &base;
}

void ConsoleLookAndFeel::drawPopupMenuBackground(juce::Graphics& g, int width, int height)
{
    if (auto* a = app())
        a->drawPopupMenuBackground(g, width, height);
    else
        LookAndFeel_V4::drawPopupMenuBackground(g, width, height);
}

void ConsoleLookAndFeel::drawPopupMenuItem(juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                           bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                           const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour)
{
    if (auto* a = app())
        a->drawPopupMenuItem(g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu, text, shortcutKeyText, icon, textColour);
    else
        LookAndFeel_V4::drawPopupMenuItem(g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu, text, shortcutKeyText, icon,
                                          textColour);
}

void ConsoleLookAndFeel::drawPopupMenuSectionHeader(juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& sectionName)
{
    if (auto* a = app())
        a->drawPopupMenuSectionHeader(g, area, sectionName);
    else
        LookAndFeel_V4::drawPopupMenuSectionHeader(g, area, sectionName);
}

juce::Font ConsoleLookAndFeel::getPopupMenuFont()
{
    if (auto* a = app())
        return a->getPopupMenuFont();
    return LookAndFeel_V4::getPopupMenuFont();
}

void ConsoleLookAndFeel::getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight, int& idealWidth,
                                                   int& idealHeight)
{
    if (auto* a = app())
        a->getIdealPopupMenuItemSize(text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
    else
        LookAndFeel_V4::getIdealPopupMenuItemSize(text, isSeparator, standardMenuItemHeight, idealWidth, idealHeight);
}

int ConsoleLookAndFeel::getPopupMenuBorderSize()
{
    if (auto* a = app())
        return a->getPopupMenuBorderSize();
    return LookAndFeel_V4::getPopupMenuBorderSize();
}

juce::Typeface::Ptr ConsoleLookAndFeel::getTypefaceForFont(const juce::Font& font)
{
    if (auto* a = app())
        return a->getTypefaceForFont(font);
    return LookAndFeel_V4::getTypefaceForFont(font);
}

} // namespace dojo::ui

#include "ui/components/IconButton.h"

#include "ui/theme/DojoTheme.h"

namespace dojo::ui
{

IconButton::IconButton(const juce::String& name, Icon icon, juce::Colour activeColour)
    : juce::Button(name)
    , icon_(icon)
    , activeColour_(activeColour)
{
    setWantsKeyboardFocus(false);
    setMouseCursor(juce::MouseCursor::PointingHandCursor);
}

void IconButton::setIcon(Icon icon)
{
    if (icon_ != icon)
    {
        icon_ = icon;
        repaint();
    }
}

void IconButton::setLit(bool lit)
{
    if (lit_ != lit)
    {
        lit_ = lit;
        repaint();
    }
}

void IconButton::paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted,
                             bool shouldDrawButtonAsDown)
{
    using namespace theme;

    const auto bounds = getLocalBounds().toFloat().reduced(1.0f);
    const bool active = lit_ || getToggleState();

    auto background = active ? activeColour_.withAlpha(0.16f) : colour::control;
    if (shouldDrawButtonAsDown)
        background = background.brighter(0.12f);
    else if (shouldDrawButtonAsHighlighted)
        background = background.brighter(0.06f);

    g.setColour(background);
    g.fillRoundedRectangle(bounds, metrics::cornerRadius);
    g.setColour(active ? activeColour_.withAlpha(0.6f) : colour::outlineStrong);
    g.drawRoundedRectangle(bounds, metrics::cornerRadius, 1.0f);

    auto glyphColour = active ? activeColour_ : colour::textSecondary;
    if (!isEnabled())
        glyphColour = colour::textMuted.withAlpha(0.6f);
    else if (shouldDrawButtonAsHighlighted && !active)
        glyphColour = colour::textPrimary;

    const float glyphSize = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.42f;
    g.setColour(glyphColour);
    g.fillPath(createGlyph(icon_, bounds.withSizeKeepingCentre(glyphSize, glyphSize)));
}

juce::Path IconButton::createGlyph(Icon icon, juce::Rectangle<float> r)
{
    juce::Path p;

    switch (icon)
    {
    case Icon::play:
        p.addTriangle(r.getX() + r.getWidth() * 0.12f, r.getY(), r.getX() + r.getWidth() * 0.12f,
                      r.getBottom(), r.getRight(), r.getCentreY());
        break;

    case Icon::pause:
    {
        const float barWidth = r.getWidth() * 0.32f;
        p.addRoundedRectangle(r.withWidth(barWidth), 1.0f);
        p.addRoundedRectangle(r.withLeft(r.getRight() - barWidth), 1.0f);
        break;
    }

    case Icon::stop:
        p.addRoundedRectangle(r.reduced(r.getWidth() * 0.06f), 1.5f);
        break;

    case Icon::record:
        p.addEllipse(r.reduced(r.getWidth() * 0.02f));
        break;

    case Icon::returnToStart:
    {
        const float barWidth = r.getWidth() * 0.16f;
        p.addRectangle(r.withWidth(barWidth));
        p.addTriangle(r.getRight(), r.getY(), r.getRight(), r.getBottom(), r.getX() + barWidth,
                      r.getCentreY());
        break;
    }

    case Icon::loop:
    {
        juce::Path outline;
        const auto body = r.reduced(0.0f, r.getHeight() * 0.18f);
        outline.addRoundedRectangle(body, body.getHeight() * 0.45f);

        juce::Path stroked;
        juce::PathStrokeType(r.getHeight() * 0.13f).createStrokedPath(stroked, outline);
        p.addPath(stroked);

        const float arrow = r.getHeight() * 0.30f;
        const float ax = body.getCentreX() + arrow * 0.2f;
        p.addTriangle(ax, body.getY() - arrow * 0.6f, ax, body.getY() + arrow * 0.6f, ax + arrow,
                      body.getY());
        break;
    }
    }

    return p;
}

} // namespace dojo::ui

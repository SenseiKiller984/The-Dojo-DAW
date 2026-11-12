#include "ui/components/SurroundPanner.h"

#include "ui/theme/DojoTheme.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
/** Where a speaker sits on the pad: x -1 (left) .. +1 (right), y -1 (front) .. +1 (rear). */
juce::Point<float> normalisedPosition(engine::Speaker speaker, const engine::ChannelLayout& layout)
{
    const bool hasRears = layout.has(engine::Speaker::leftRear);
    switch (speaker)
    {
    case engine::Speaker::left:          return {-1.0f, -1.0f};
    case engine::Speaker::right:         return {1.0f, -1.0f};
    case engine::Speaker::centre:        return {0.0f, -1.0f};
    case engine::Speaker::lfe:           return {0.45f, -0.55f};
    case engine::Speaker::leftSurround:  return {-1.0f, hasRears ? 0.0f : 1.0f};
    case engine::Speaker::rightSurround: return {1.0f, hasRears ? 0.0f : 1.0f};
    case engine::Speaker::leftRear:      return {-1.0f, 1.0f};
    case engine::Speaker::rightRear:     return {1.0f, 1.0f};
    }
    return {};
}
} // namespace

SurroundPanner::SurroundPanner()
{
    setTooltip("Surround panner: drag to place the track (left/right, front/rear). Right-click for the centre speaker "
               "amount; double-click to reset.");
}

void SurroundPanner::setLayout(const engine::ChannelLayout& layout)
{
    if (layout == layout_)
        return;
    layout_ = layout;
    repaint();
}

void SurroundPanner::setPosition(double pan, double frontRear, double centre)
{
    pan_ = juce::jlimit(-1.0, 1.0, pan);
    frontRear_ = juce::jlimit(-1.0, 1.0, frontRear);
    centre_ = juce::jlimit(0.0, 1.0, centre);
    repaint();
}

juce::Rectangle<float> SurroundPanner::padArea() const
{
    return getLocalBounds().toFloat().reduced(6.0f, 5.0f);
}

juce::Point<float> SurroundPanner::toPoint(double pan, double frontRear) const
{
    const auto area = padArea();
    return {area.getCentreX() + static_cast<float>(pan) * area.getWidth() * 0.5f,
            area.getCentreY() + static_cast<float>(frontRear) * area.getHeight() * 0.5f};
}

std::pair<double, double> SurroundPanner::fromPoint(juce::Point<float> point) const
{
    const auto area = padArea();
    const double x = (point.x - area.getCentreX()) / std::max(1.0f, area.getWidth() * 0.5f);
    const double y = (point.y - area.getCentreY()) / std::max(1.0f, area.getHeight() * 0.5f);
    return {juce::jlimit(-1.0, 1.0, x), juce::jlimit(-1.0, 1.0, y)};
}

juce::Point<float> SurroundPanner::getSpeakerPosition(engine::Speaker speaker) const
{
    const auto p = normalisedPosition(speaker, layout_);
    return toPoint(p.x, p.y);
}

void SurroundPanner::paint(juce::Graphics& g)
{
    using namespace theme;
    const auto area = padArea();

    g.setColour(colour::meterBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f);
    g.setColour(colour::outline);
    g.drawRoundedRectangle(area, 3.0f, 1.0f);
    g.drawLine(area.getCentreX(), area.getY(), area.getCentreX(), area.getBottom(), 0.5f);
    g.drawLine(area.getX(), area.getCentreY(), area.getRight(), area.getCentreY(), 0.5f);

    // Speakers (the LFE as a small square).
    for (int i = 0; i < layout_.numChannels; ++i)
    {
        const auto speaker = layout_.speakers[static_cast<std::size_t>(i)];
        const auto at = getSpeakerPosition(speaker);
        g.setColour(colour::textSecondary);
        if (speaker == engine::Speaker::lfe)
            g.fillRect(juce::Rectangle<float>(4.0f, 4.0f).withCentre(at));
        else
            g.fillEllipse(juce::Rectangle<float>(5.0f, 5.0f).withCentre(at));
        if (speaker == engine::Speaker::centre && centre_ > 0.0)
        {
            const float ring = 4.0f + 5.0f * static_cast<float>(centre_);
            g.setColour(colour::accent.withAlpha(0.8f));
            g.drawEllipse(juce::Rectangle<float>(ring * 2.0f, ring * 2.0f).withCentre(at), 1.2f);
        }
    }

    // The puck.
    const auto puck = toPoint(pan_, frontRear_);
    g.setColour(colour::accent);
    g.fillEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(puck));
    g.setColour(colour::textPrimary);
    g.drawEllipse(juce::Rectangle<float>(9.0f, 9.0f).withCentre(puck), 1.0f);
}

void SurroundPanner::moveTo(juce::Point<float> point)
{
    const auto [pan, frontRear] = fromPoint(point);
    pan_ = pan;
    frontRear_ = frontRear;
    repaint();
    if (onMove)
        onMove(pan_, frontRear_);
}

void SurroundPanner::mouseDown(const juce::MouseEvent& e)
{
    if (!isEnabled())
        return;

    if (e.mods.isPopupMenu())
    {
        juce::Component::SafePointer<SurroundPanner> safe(this);
        juce::PopupMenu menu;
        menu.addSectionHeader("Centre speaker");
        for (const int percent : {0, 25, 50, 75, 100})
            menu.addItem(juce::String(percent) + " %", true, std::abs(centre_ * 100.0 - percent) < 0.5,
                         [safe, percent]
                         {
                             if (safe == nullptr)
                                 return;
                             safe->centre_ = percent / 100.0;
                             safe->repaint();
                             if (safe->onCentreChange)
                                 safe->onCentreChange(safe->centre_);
                         });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
        return;
    }

    dragging_ = true;
    if (onDragStart)
        onDragStart();
    moveTo(e.position);
}

void SurroundPanner::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging_)
        moveTo(e.position);
}

void SurroundPanner::mouseUp(const juce::MouseEvent&)
{
    if (!dragging_)
        return;
    dragging_ = false;
    if (onDragEnd)
        onDragEnd();
}

void SurroundPanner::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu() || !isEnabled())
        return;
    pan_ = 0.0;
    frontRear_ = -1.0;
    repaint();
    if (onReset)
        onReset();
}

} // namespace dojo::ui

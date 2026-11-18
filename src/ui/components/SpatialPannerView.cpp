#include "ui/components/SpatialPannerView.h"

#include "ui/theme/DojoTheme.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace dojo::ui
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

engine::Vec3 sub(const engine::Vec3& a, const engine::Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
engine::Vec3 mul(const engine::Vec3& a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(const engine::Vec3& a, const engine::Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
engine::Vec3 cross(const engine::Vec3& a, const engine::Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
engine::Vec3 unit(const engine::Vec3& a)
{
    const double l = std::sqrt(dot(a, a));
    return l > 0.0 ? mul(a, 1.0 / l) : engine::Vec3{0.0, 1.0, 0.0};
}

juce::String targetOf(std::size_t k)
{
    switch (k)
    {
    case 0: return session::automation::azimuthTarget();
    case 1: return session::automation::elevationTarget();
    case 2: return session::automation::distanceTarget();
    default: break;
    }
    return session::automation::sizeTarget();
}
double valueOf(const SpatialPosition& p, std::size_t k) { return k == 0 ? p.azimuth : k == 1 ? p.elevation : k == 2 ? p.distance : p.size; }
void setValue(SpatialPosition& p, std::size_t k, double v)
{
    (k == 0 ? p.azimuth : k == 1 ? p.elevation : k == 2 ? p.distance : p.size) = v;
}
} // namespace

// --- Positions ---------------------------------------------------------------------------------

engine::Vec3 SpatialPosition::toVector() const noexcept
{
    return mul(engine::directionVector(azimuth, elevation), std::clamp(distance, 0.0, 1.0));
}

SpatialPosition SpatialPosition::fromVector(const engine::Vec3& p, const SpatialPosition& keep) noexcept
{
    SpatialPosition result = keep;
    const double length = std::sqrt(dot(p, p));
    result.distance = std::min(1.0, length);
    if (length > 1.0e-6)
    {
        const auto [azimuth, elevation] = engine::directionAngles(p);
        result.azimuth = azimuth;
        result.elevation = elevation;
    }
    return result;
}

// --- The view ----------------------------------------------------------------------------------

SpatialPannerView::SpatialPannerView(bool compact)
    : compact_(compact)
{
    if (compact_)
        projection_ = Projection::top;
    setTooltip(compact_ ? "3D panner (seen from above): drag to place the track. Double-click for the full 3D panner."
                        : "Drag the source to place it; drag a keyframe to move it. Perspective: right-drag (or Alt-drag) turns "
                          "the room. Double-click the source: back ahead at the speakers.");
}

void SpatialPannerView::setLayout(const engine::ChannelLayout& layout)
{
    if (layout == layout_)
        return;
    layout_ = layout;
    repaint();
}

void SpatialPannerView::setProjection(Projection projection)
{
    projection_ = projection;
    repaint();
}

void SpatialPannerView::setPosition(const SpatialPosition& position)
{
    position_ = position;
    repaint();
}

void SpatialPannerView::setLivePosition(std::optional<SpatialPosition> live)
{
    live_ = live;
    repaint();
}

void SpatialPannerView::setTrajectory(std::vector<Keyframe> keyframes, std::vector<SpatialPosition> path)
{
    keyframes_ = std::move(keyframes);
    path_ = std::move(path);
    repaint();
}

void SpatialPannerView::setPlayheadPosition(std::optional<SpatialPosition> position)
{
    playhead_ = position;
    repaint();
}

juce::Rectangle<float> SpatialPannerView::viewArea() const
{
    return getLocalBounds().toFloat().reduced(compact_ ? 2.0f : 8.0f);
}

double SpatialPannerView::scale() const
{
    const auto area = viewArea();
    return std::min(area.getWidth(), area.getHeight()) * 0.5 / (compact_ ? 1.08 : 1.18);
}

juce::Point<float> SpatialPannerView::project(const engine::Vec3& p) const
{
    const auto area = viewArea();
    const double s = scale();
    const double cx = area.getCentreX();
    const double cy = area.getCentreY();
    switch (projection_)
    {
    case Projection::top:  return {static_cast<float>(cx + p.x * s), static_cast<float>(cy - p.y * s)};
    case Projection::side: return {static_cast<float>(cx + p.y * s), static_cast<float>(cy - p.z * s)};
    case Projection::rear: return {static_cast<float>(cx + p.x * s), static_cast<float>(cy - p.z * s)};
    case Projection::perspective: break;
    }
    // From behind and above the listener, the room turned by yaw.
    const double yaw = yaw_ * kPi / 180.0;
    const engine::Vec3 q{p.x * std::cos(yaw) - p.y * std::sin(yaw), p.x * std::sin(yaw) + p.y * std::cos(yaw), p.z};
    const engine::Vec3 camera{0.0, -3.3, 2.0};
    const auto forward = unit(sub(engine::Vec3{0.0, 0.2, -0.25}, camera));
    const auto right = unit(cross(forward, engine::Vec3{0.0, 0.0, 1.0}));
    const auto up = cross(right, forward);
    const auto r = sub(q, camera);
    const double depth = std::max(0.1, dot(r, forward));
    const double focal = s * 3.1;
    return {static_cast<float>(cx + dot(r, right) / depth * focal), static_cast<float>(cy + 0.08 * s - dot(r, up) / depth * focal)};
}

engine::Vec3 SpatialPannerView::unproject(juce::Point<float> screen, const engine::Vec3& keep) const
{
    const auto area = viewArea();
    const double s = scale();
    const double u = (screen.x - area.getCentreX()) / s;
    const double v = -(screen.y - area.getCentreY()) / s;
    const auto clamp = [](double value) { return std::clamp(value, -1.0, 1.0); };
    switch (projection_)
    {
    case Projection::top:  return {clamp(u), clamp(v), keep.z};
    case Projection::side: return {keep.x, clamp(u), clamp(v)};
    case Projection::rear: return {clamp(u), keep.y, clamp(v)};
    case Projection::perspective: break;
    }
    return keep;
}

juce::Point<float> SpatialPannerView::getKeyframePoint(int index) const
{
    return index >= 0 && index < static_cast<int>(keyframes_.size()) ? project(keyframes_[static_cast<std::size_t>(index)].position.toVector())
                                                                      : juce::Point<float>{};
}

void SpatialPannerView::drawRoom(juce::Graphics& g) const
{
    using namespace theme;
    const auto line = [&](const engine::Vec3& a, const engine::Vec3& b, float thickness)
    { g.drawLine(juce::Line<float>(project(a), project(b)), thickness); };

    g.setColour(colour::outlineStrong);
    if (projection_ == Projection::perspective)
    {
        // The room's box, the floor grid and the ear-level ring.
        for (const double a : {-1.0, 1.0})
            for (const double b : {-1.0, 1.0})
            {
                line({a, b, -1.0}, {a, b, 1.0}, 1.0f);
                line({a, -1.0, b}, {a, 1.0, b}, 1.0f);
                line({-1.0, a, b}, {1.0, a, b}, 1.0f);
            }
        g.setColour(colour::gridBar);
        for (double v = -0.5; v <= 0.51; v += 0.5)
        {
            line({v, -1.0, -1.0}, {v, 1.0, -1.0}, 0.6f);
            line({-1.0, v, -1.0}, {1.0, v, -1.0}, 0.6f);
        }
    }
    else
    {
        const auto a = project({-1.0, -1.0, -1.0});
        const auto b = project({1.0, 1.0, 1.0});
        g.drawRect(juce::Rectangle<float>(a, b), 1.0f);
        g.setColour(colour::gridBar);
        const auto centre = project({0.0, 0.0, 0.0});
        g.drawLine(juce::Rectangle<float>(a, b).getX(), centre.y, juce::Rectangle<float>(a, b).getRight(), centre.y, 0.6f);
        g.drawLine(centre.x, juce::Rectangle<float>(a, b).getY(), centre.x, juce::Rectangle<float>(a, b).getBottom(), 0.6f);
    }

    // The speakers' sphere: its ear-level ring (perspective, top) or its outline (side, rear).
    juce::Path ring;
    for (int k = 0; k <= 64; ++k)
    {
        const double t = 2.0 * kPi * k / 64.0;
        const engine::Vec3 p = projection_ == Projection::side   ? engine::Vec3{0.0, std::cos(t), std::sin(t)}
                               : projection_ == Projection::rear ? engine::Vec3{std::cos(t), 0.0, std::sin(t)}
                                                                 : engine::Vec3{std::cos(t), std::sin(t), 0.0};
        const auto point = project(p);
        if (k == 0)
            ring.startNewSubPath(point);
        else
            ring.lineTo(point);
    }
    g.setColour(colour::textMuted.withAlpha(0.6f));
    const float dashes[] = {3.0f, 3.0f};
    juce::PathStrokeType(0.8f).createDashedStroke(ring, ring, dashes, 2);
    g.fillPath(ring);

    // The listener, facing the front.
    const auto head = project({0.0, 0.0, 0.0});
    const float r = compact_ ? 2.5f : 4.0f;
    g.setColour(colour::textSecondary);
    g.drawEllipse(juce::Rectangle<float>(r * 2.0f, r * 2.0f).withCentre(head), 1.0f);
    if (projection_ != Projection::rear)
        g.drawLine(juce::Line<float>(head, project({0.0, 0.12, 0.0})), 1.0f);
}

void SpatialPannerView::drawSpeakers(juce::Graphics& g) const
{
    using namespace theme;
    std::vector<std::pair<juce::Point<float>, juce::String>> labels; // (speakers on the same spot share one)
    for (int c = 0; c < layout_.numChannels; ++c)
    {
        const auto speaker = layout_.speakers[static_cast<std::size_t>(c)];
        if (speaker == engine::Speaker::lfe)
            continue;
        const auto d = engine::directionOf(layout_, speaker);
        const auto at = project(engine::directionVector(d.azimuth, d.elevation));
        const float size = compact_ ? 4.0f : 7.0f;
        const auto box = juce::Rectangle<float>(size, size).withCentre(at);
        g.setColour(colour::textSecondary);
        if (engine::isHeight(speaker))
            g.drawRect(box, 1.0f); // (the top layer: hollow)
        else
            g.fillRect(box);
        const auto same = std::find_if(labels.begin(), labels.end(), [at](const auto& l) { return l.first.getDistanceFrom(at) < 1.5f; });
        if (same != labels.end())
            same->second << "/" << engine::shortName(speaker);
        else
            labels.emplace_back(at, engine::shortName(speaker));
    }
    if (compact_)
        return;
    g.setColour(colour::textMuted);
    g.setFont(theme::label(9.0f));
    for (const auto& [at, text] : labels)
        g.drawText(text, juce::Rectangle<float>(64.0f, 11.0f).withCentre(at.translated(0.0f, -10.0f)), juce::Justification::centred, false);
}

void SpatialPannerView::drawSource(juce::Graphics& g, const SpatialPosition& p, bool faint) const
{
    using namespace theme;
    const auto at = project(p.toVector());
    // A line down to the floor (depth), and the spread as a halo.
    if (projection_ != Projection::top)
    {
        auto floor = p.toVector();
        floor.z = -1.0;
        g.setColour(colour::accent.withAlpha(faint ? 0.15f : 0.35f));
        g.drawLine(juce::Line<float>(at, project(floor)), 1.0f);
    }
    const double spread = engine::SpatialPanner::effectiveSpread(p.size, p.distance);
    const float radius = (compact_ ? 3.5f : 6.5f) + static_cast<float>(spread) * (compact_ ? 10.0f : 34.0f);
    g.setColour(colour::accent.withAlpha(faint ? 0.06f : 0.16f));
    g.fillEllipse(juce::Rectangle<float>(radius * 2.0f, radius * 2.0f).withCentre(at));
    const float dot = compact_ ? 7.0f : 12.0f;
    if (faint)
    {
        g.setColour(colour::accent.withAlpha(0.5f));
        g.drawEllipse(juce::Rectangle<float>(dot, dot).withCentre(at), 1.2f);
        return;
    }
    g.setColour(colour::accent);
    g.fillEllipse(juce::Rectangle<float>(dot, dot).withCentre(at));
    g.setColour(colour::textPrimary);
    g.drawEllipse(juce::Rectangle<float>(dot, dot).withCentre(at), 1.0f);
}

void SpatialPannerView::paint(juce::Graphics& g)
{
    using namespace theme;
    g.setColour(colour::meterBackground);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), 4.0f);
    drawRoom(g);
    drawSpeakers(g);

    // The trajectory: the path, its keyframes, and where it is at the playhead.
    if (path_.size() >= 2)
    {
        juce::Path path;
        path.startNewSubPath(project(path_.front().toVector()));
        for (std::size_t i = 1; i < path_.size(); ++i)
            path.lineTo(project(path_[i].toVector()));
        g.setColour(colour::gold.withAlpha(0.8f));
        g.strokePath(path, juce::PathStrokeType(compact_ ? 1.0f : 1.6f));
    }
    if (!compact_)
        for (const auto& k : keyframes_)
        {
            const auto at = project(k.position.toVector());
            juce::Path diamond;
            diamond.addQuadrilateral(at.x, at.y - 5.0f, at.x + 5.0f, at.y, at.x, at.y + 5.0f, at.x - 5.0f, at.y);
            g.setColour(colour::gold);
            g.fillPath(diamond);
            g.setColour(colour::meterBackground);
            g.strokePath(diamond, juce::PathStrokeType(1.0f));
        }

    if (live_.has_value())
    {
        drawSource(g, position_, true); // (its own setting, under the automation)
        drawSource(g, *live_, false);
    }
    else
    {
        drawSource(g, position_, false);
    }
    if (playhead_.has_value() && !live_.has_value())
    {
        const auto at = project(playhead_->toVector());
        g.setColour(colour::playhead);
        g.drawEllipse(juce::Rectangle<float>(16.0f, 16.0f).withCentre(at), 1.5f);
    }

    if (!compact_)
    {
        g.setColour(colour::textSecondary);
        g.setFont(theme::label(10.0f));
        const char* names[] = {"PERSPECTIVE", "TOP  (X / Y)", "SIDE  (Y / Z)", "REAR  (X / Z)"};
        g.drawText(names[static_cast<int>(projection_)], viewArea().reduced(4.0f).removeFromTop(14.0f), juce::Justification::topLeft, false);
        const auto& p = live_.has_value() ? *live_ : position_;
        const auto degree = juce::String::fromUTF8("\xc2\xb0");
        const auto signedText = [](double v) { return (v >= 0.0 ? "+" : "") + juce::String(juce::roundToInt(v)); };
        g.drawText("AZ " + signedText(p.azimuth) + degree + "   EL " + signedText(p.elevation) + degree + "   DIST " + juce::String(p.distance, 2)
                       + "   SIZE " + juce::String(juce::roundToInt(p.size * 100.0)) + "%",
                   viewArea().reduced(4.0f).removeFromBottom(14.0f), juce::Justification::bottomLeft, false);
    }
    else if (std::abs(position_.elevation) >= 0.5)
    {
        g.setColour(colour::textMuted);
        g.setFont(theme::label(8.0f));
        g.drawText((position_.elevation >= 0.0 ? "+" : "") + juce::String(juce::roundToInt(position_.elevation)) + juce::String::fromUTF8("\xc2\xb0"),
                   viewArea().removeFromBottom(10.0f), juce::Justification::bottomRight, false);
    }
}

int SpatialPannerView::keyframeAt(juce::Point<float> point) const
{
    if (compact_)
        return -1;
    for (int i = static_cast<int>(keyframes_.size()) - 1; i >= 0; --i)
        if (getKeyframePoint(i).getDistanceFrom(point) <= 7.0f)
            return i;
    return -1;
}

SpatialPosition SpatialPannerView::dragTo(const SpatialPosition& from, juce::Point<float> screen, juce::Point<float> delta) const
{
    if (projection_ == Projection::perspective)
    {
        // Turn the source round you: left/right is azimuth, up/down elevation.
        SpatialPosition p = from;
        p.azimuth = std::fmod(from.azimuth - delta.x * 0.6 + 540.0, 360.0) - 180.0;
        p.elevation = std::clamp(from.elevation - delta.y * 0.45, -90.0, 90.0);
        return p;
    }
    return SpatialPosition::fromVector(unproject(screen, from.toVector()), from);
}

void SpatialPannerView::mouseDown(const juce::MouseEvent& e)
{
    if (!isEnabled())
        return;
    dragStart_ = e.position;
    drag_ = Drag::none;
    dragMoved_ = false;
    if (const int k = keyframeAt(e.position); k >= 0)
    {
        if (e.mods.isPopupMenu())
        {
            if (onKeyframeMenu)
                onKeyframeMenu(k);
            return;
        }
        drag_ = Drag::keyframe;
        dragKeyframe_ = k;
        dragFrom_ = keyframes_[static_cast<std::size_t>(k)].position;
        if (onDragStart)
            onDragStart();
        return;
    }
    if (projection_ == Projection::perspective && (e.mods.isPopupMenu() || e.mods.isAltDown()))
    {
        drag_ = Drag::orbit;
        dragYaw_ = yaw_;
        return;
    }
    if (e.mods.isPopupMenu())
        return;
    drag_ = Drag::source;
    dragFrom_ = position_;
    if (onDragStart)
        onDragStart();
    // In a flat view a click places the source there at once (the compact view waits for a drag:
    // a double-click there opens the full panner).
    if (projection_ != Projection::perspective && !compact_)
    {
        position_ = dragTo(dragFrom_, e.position, {});
        dragFrom_ = position_;
        repaint();
        if (onMove)
            onMove(position_);
    }
}

void SpatialPannerView::mouseDrag(const juce::MouseEvent& e)
{
    const auto delta = e.position - dragStart_;
    switch (drag_)
    {
    case Drag::none:
        break;
    case Drag::orbit:
        yaw_ = dragYaw_ + delta.x * 0.5;
        repaint();
        break;
    case Drag::source:
        if (compact_ && delta.getDistanceFromOrigin() < 3.0f && !dragMoved_)
            break; // (not yet a drag)
        dragMoved_ = true;
        position_ = dragTo(dragFrom_, e.position, delta);
        repaint();
        if (onMove)
            onMove(position_);
        break;
    case Drag::keyframe:
    {
        const auto moved = dragTo(dragFrom_, e.position, delta);
        if (dragKeyframe_ >= 0 && dragKeyframe_ < static_cast<int>(keyframes_.size()))
            keyframes_[static_cast<std::size_t>(dragKeyframe_)].position = moved;
        repaint();
        if (onKeyframeMove)
            onKeyframeMove(dragKeyframe_, moved);
        break;
    }
    }
}

void SpatialPannerView::mouseUp(const juce::MouseEvent&)
{
    if ((drag_ == Drag::source || drag_ == Drag::keyframe) && onDragEnd)
        onDragEnd();
    drag_ = Drag::none;
}

void SpatialPannerView::mouseDoubleClick(const juce::MouseEvent&)
{
    if (compact_)
    {
        if (onOpen)
            onOpen();
        return;
    }
    // Back ahead, at the speakers.
    if (onDragStart)
        onDragStart();
    position_.azimuth = 0.0;
    position_.elevation = 0.0;
    position_.distance = 1.0;
    repaint();
    if (onMove)
        onMove(position_);
    if (onDragEnd)
        onDragEnd();
}

// --- The editor ----------------------------------------------------------------------------------

SpatialPannerEditor::SpatialPannerEditor(session::Session& session, juce::ValueTree track)
    : session_(session)
    , track_(std::move(track))
{
    using namespace theme;
    addAndMakeVisible(view_);

    const std::array<juce::String, 4> names{"Azimuth", "Elevation", "Distance", "Size"};
    const std::array<juce::Slider*, 4> sliders{&azimuth, &elevation, &distance, &size};
    for (std::size_t k = 0; k < 4; ++k)
    {
        auto& s = *sliders[k];
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
        s.setColour(juce::Slider::trackColourId, colour::accent);
        addAndMakeVisible(s);
        labels_[k].setText(names[k], juce::dontSendNotification);
        labels_[k].setFont(theme::label(11.0f));
        labels_[k].setColour(juce::Label::textColourId, colour::textSecondary);
        addAndMakeVisible(labels_[k]);
    }
    azimuth.setRange(-180.0, 180.0, 0.1);
    azimuth.setTextValueSuffix(juce::String::fromUTF8("\xc2\xb0"));
    azimuth.setTooltip("Azimuth: -180 to +180 degrees around you (positive to the left, 0 ahead, 180 behind)");
    elevation.setRange(-90.0, 90.0, 0.1);
    elevation.setTextValueSuffix(juce::String::fromUTF8("\xc2\xb0"));
    elevation.setTooltip("Elevation: -90 (below) to +90 degrees (overhead)");
    distance.setRange(0.0, 1.0, 0.01);
    distance.setTooltip("Distance: 1 at the speakers, 0 at your head (a closer source surrounds you more)");
    size.setRange(0.0, 100.0, 0.1);
    size.setTextValueSuffix(" %");
    size.setTooltip("Size: from a point to all around you; its share plays diffuse (decorrelated) from the neighbouring speakers");

    for (auto* s : sliders)
    {
        s->onDragStart = [this] { session_.beginTransaction("3D Panner"); };
        s->onValueChange = [this, s]
        {
            if (updating_)
                return;
            if (!s->isMouseButtonDown())
                session_.beginTransaction("3D Panner");
            auto* um = &session_.getUndoManager();
            if (s == &azimuth)
                session::track::setAzimuth(track_, azimuth.getValue(), um);
            else if (s == &elevation)
                session::track::setElevation(track_, elevation.getValue(), um);
            else if (s == &distance)
                session::track::setDistance(track_, distance.getValue(), um);
            else
                session::track::setSize(track_, size.getValue() / 100.0, um);
        };
    }

    const std::array<juce::TextButton*, 4> views{&perspective, &top, &side, &rear};
    const std::array<SpatialPannerView::Projection, 4> projections{SpatialPannerView::Projection::perspective, SpatialPannerView::Projection::top,
                                                                   SpatialPannerView::Projection::side, SpatialPannerView::Projection::rear};
    for (std::size_t k = 0; k < 4; ++k)
    {
        auto& b = *views[k];
        b.setClickingTogglesState(true);
        b.setRadioGroupId(3801);
        b.setWantsKeyboardFocus(false);
        b.onClick = [this, k, projections] { view_.setProjection(projections[k]); };
        addAndMakeVisible(b);
    }
    perspective.setToggleState(true, juce::dontSendNotification);
    perspective.setTooltip("Perspective view of the room");
    top.setTooltip("Top view (X / Y): place it around you");
    side.setTooltip("Side view (Y / Z): front to back and height");
    rear.setTooltip("Rear view (X / Z): left to right and height");

    addKeyframeButton.setTooltip("Write the position into the track's 3D automation at the playhead (a keyframe of its trajectory)");
    addKeyframeButton.onClick = [this] { addKeyframe(playhead_); };
    clearButton.setTooltip("Remove the 3D automation (the trajectory)");
    clearButton.onClick = [this] { clearTrajectory(); };
    for (auto* b : {&addKeyframeButton, &clearButton})
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(b);
    }
    hint_.setText("Drag the source, or a keyframe. Add Keyframe writes the position at the playhead.", juce::dontSendNotification);
    hint_.setFont(theme::sans(11.0f));
    hint_.setColour(juce::Label::textColourId, colour::textMuted);
    addAndMakeVisible(hint_);

    view_.onDragStart = [this] { session_.beginTransaction("Move 3D Panner"); };
    view_.onMove = [this](const SpatialPosition& p) { setOwnPosition(p); };
    view_.onKeyframeMove = [this](int index, const SpatialPosition& p) { moveKeyframe(index, p); };
    view_.onKeyframeMenu = [this](int index)
    {
        juce::PopupMenu menu;
        juce::Component::SafePointer<SpatialPannerEditor> safe(this);
        menu.addItem("Delete Keyframe", [safe, index]
                     {
                         if (safe != nullptr)
                             safe->deleteKeyframe(index);
                     });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&view_));
    };

    session_.getState().addListener(this);
    refresh();
}

SpatialPannerEditor::~SpatialPannerEditor()
{
    session_.getState().removeListener(this);
}

void SpatialPannerEditor::setDestinationLayout(const engine::ChannelLayout& layout)
{
    view_.setLayout(layout);
}

SpatialPosition SpatialPannerEditor::getOwnPosition() const
{
    return {session::track::getAzimuth(track_), session::track::getElevation(track_), session::track::getDistance(track_),
            session::track::getSize(track_)};
}

void SpatialPannerEditor::setOwnPosition(const SpatialPosition& p)
{
    auto* um = &session_.getUndoManager();
    session::track::setAzimuth(track_, p.azimuth, um);
    session::track::setElevation(track_, p.elevation, um);
    session::track::setDistance(track_, p.distance, um);
}

SpatialPosition SpatialPannerEditor::positionAt(double time) const
{
    auto p = getOwnPosition();
    for (std::size_t k = 0; k < 4; ++k)
    {
        const auto lane = session_.getAutomation(track_, targetOf(k), false);
        if (lane.isValid() && lane.getNumChildren() > 0)
            setValue(p, k, session::automation::spatialFromNormalised(targetOf(k), session::automation::toCurve(lane).valueAt(time)));
    }
    return p;
}

std::vector<SpatialPannerView::Keyframe> SpatialPannerEditor::getKeyframes() const
{
    std::set<double> times;
    for (std::size_t k = 0; k < 4; ++k)
        if (const auto lane = session_.getAutomation(track_, targetOf(k), false); lane.isValid())
            for (const auto& point : lane)
                times.insert(static_cast<double>(point.getProperty(session::ids::time)));
    std::vector<SpatialPannerView::Keyframe> keyframes;
    for (const double t : times)
        keyframes.push_back({t, positionAt(t)});
    return keyframes;
}

namespace
{
/** Sets the lane's point at `time` (within a microsecond) to `value`, or adds one. */
void setPointAt(session::Session& session, juce::ValueTree lane, double time, double value)
{
    for (auto point : lane)
        if (std::abs(static_cast<double>(point.getProperty(session::ids::time)) - time) < 1.0e-6)
        {
            session.moveAutomationPoint(point, time, value);
            return;
        }
    session.addAutomationPoint(lane, time, value);
}
} // namespace

void SpatialPannerEditor::addKeyframe(double time)
{
    session_.beginTransaction("Add 3D Keyframe");
    const auto own = getOwnPosition();
    for (std::size_t k = 0; k < 4; ++k)
    {
        const auto target = targetOf(k);
        auto lane = session_.getAutomation(track_, target, k < 3, session::automation::spatialToNormalised(target, valueOf(own, k)));
        if (lane.isValid())
            setPointAt(session_, lane, std::max(0.0, time), session::automation::spatialToNormalised(target, valueOf(own, k)));
    }
    session::track::setShowingAutomation(track_, true, nullptr);
}

void SpatialPannerEditor::moveKeyframe(int index, const SpatialPosition& p)
{
    const auto keyframes = getKeyframes();
    if (index < 0 || index >= static_cast<int>(keyframes.size()))
        return;
    const double time = keyframes[static_cast<std::size_t>(index)].time;
    for (std::size_t k = 0; k < 3; ++k) // (azimuth, elevation, distance: what a drag moves)
    {
        const auto target = targetOf(k);
        auto lane = session_.getAutomation(track_, target, true, session::automation::spatialToNormalised(target, valueOf(getOwnPosition(), k)));
        setPointAt(session_, lane, time, session::automation::spatialToNormalised(target, valueOf(p, k)));
    }
}

void SpatialPannerEditor::deleteKeyframe(int index)
{
    const auto keyframes = getKeyframes();
    if (index < 0 || index >= static_cast<int>(keyframes.size()))
        return;
    const double time = keyframes[static_cast<std::size_t>(index)].time;
    session_.beginTransaction("Delete 3D Keyframe");
    for (std::size_t k = 0; k < 4; ++k)
    {
        auto lane = session_.getAutomation(track_, targetOf(k), false);
        if (!lane.isValid())
            continue;
        for (int i = lane.getNumChildren() - 1; i >= 0; --i)
            if (std::abs(static_cast<double>(lane.getChild(i).getProperty(session::ids::time)) - time) < 1.0e-6)
                session_.removeAutomationPoint(lane.getChild(i));
        if (lane.getNumChildren() == 0)
            session_.removeAutomation(lane);
    }
}

void SpatialPannerEditor::clearTrajectory()
{
    session_.beginTransaction("Clear 3D Trajectory");
    for (std::size_t k = 0; k < 4; ++k)
        if (auto lane = session_.getAutomation(track_, targetOf(k), false); lane.isValid())
            session_.removeAutomation(lane);
}

void SpatialPannerEditor::refresh()
{
    if (!track_.isValid() || !track_.getParent().isValid())
        return;
    const juce::ScopedValueSetter<bool> guard(updating_, true);
    const auto own = getOwnPosition();
    view_.setPosition(own);
    azimuth.setValue(own.azimuth, juce::dontSendNotification);
    elevation.setValue(own.elevation, juce::dontSendNotification);
    distance.setValue(own.distance, juce::dontSendNotification);
    size.setValue(own.size * 100.0, juce::dontSendNotification);

    auto keyframes = getKeyframes();
    std::vector<SpatialPosition> path;
    if (keyframes.size() >= 2)
    {
        const double from = keyframes.front().time;
        const double to = keyframes.back().time;
        for (int i = 0; i <= 160; ++i)
            path.push_back(positionAt(from + (to - from) * i / 160.0));
    }
    view_.setTrajectory(std::move(keyframes), std::move(path));
    clearButton.setEnabled(view_.getNumKeyframes() > 0);
}

void SpatialPannerEditor::showLive(const std::array<float, 4>& moving, double playheadSeconds)
{
    playhead_ = playheadSeconds;
    bool moves = false;
    auto live = getOwnPosition();
    for (std::size_t k = 0; k < 4; ++k)
        if (!std::isnan(moving[k]))
        {
            setValue(live, k, static_cast<double>(moving[k]));
            moves = true;
        }
    view_.setLivePosition(moves ? std::optional<SpatialPosition>(live) : std::nullopt);
    view_.setPlayheadPosition(view_.getNumKeyframes() > 0 ? std::optional<SpatialPosition>(positionAt(playheadSeconds)) : std::nullopt);
}

void SpatialPannerEditor::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::panel);
}

void SpatialPannerEditor::resized()
{
    auto area = getLocalBounds().reduced(10);
    auto buttons = area.removeFromTop(26);
    for (auto* b : {&perspective, &top, &side, &rear})
    {
        b->setBounds(buttons.removeFromLeft(64));
        buttons.removeFromLeft(4);
    }
    clearButton.setBounds(buttons.removeFromRight(120));
    buttons.removeFromRight(6);
    addKeyframeButton.setBounds(buttons.removeFromRight(110));
    area.removeFromTop(6);
    hint_.setBounds(area.removeFromBottom(18));
    auto controls = area.removeFromBottom(4 * 26);
    area.removeFromBottom(6);
    view_.setBounds(area);
    const std::array<juce::Slider*, 4> sliders{&azimuth, &elevation, &distance, &size};
    for (std::size_t k = 0; k < 4; ++k)
    {
        auto row = controls.removeFromTop(26);
        labels_[k].setBounds(row.removeFromLeft(80));
        sliders[k]->setBounds(row);
    }
}

// --- The window --------------------------------------------------------------------------------

SpatialPannerWindow::SpatialPannerWindow(session::Session& session, juce::ValueTree track, const engine::ChannelLayout& destination)
    : juce::DocumentWindow("3D Panner - " + session::track::getName(track), theme::colour::panel, juce::DocumentWindow::closeButton)
{
    auto editor = std::make_unique<SpatialPannerEditor>(session, std::move(track));
    editor->setDestinationLayout(destination);
    editor_ = editor.get();
    setUsingNativeTitleBar(true);
    setContentOwned(editor.release(), false);
    setResizable(true, false);
    setResizeLimits(420, 420, 2000, 2000);
    centreWithSize(720, 640);
    setVisible(true);
}

void SpatialPannerWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}

} // namespace dojo::ui

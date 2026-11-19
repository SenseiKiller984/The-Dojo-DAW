#pragma once

#include "engine/ChannelLayout.h"
#include "engine/SpatialPanner.h"
#include "session/Session.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace dojo::ui
{

/** Where a 3D-panned source is: its direction (degrees), distance (0 at the listener .. 1 at the
    speakers) and size (0..1). */
struct SpatialPosition
{
    double azimuth = 0.0;
    double elevation = 0.0;
    double distance = 1.0;
    double size = 0.0;

    /** Its point in the room (x right, y front, z up; the speakers' sphere has radius 1). */
    [[nodiscard]] engine::Vec3 toVector() const noexcept;
    /** The position of a point in the room (beyond the sphere: on it), keeping `keep`'s size, and
        its direction at the very centre. */
    [[nodiscard]] static SpatialPosition fromVector(const engine::Vec3& p, const SpatialPosition& keep) noexcept;
};

/**
    The 3D panner's room view: a wireframe box (the room) with the destination's speakers (heights
    hollow), the listener in the middle, and the source: a handle you drag, with a line down to the
    floor, a halo as wide as its spread, and, when it is automated, its trajectory with draggable
    keyframe nodes and where it is at the playhead.

    Four views (drawn as vectors):
      - Perspective: from behind and above; drag the source to turn it round you (left/right is
        azimuth, up/down elevation); right-drag (or Alt-drag) to turn the room.
      - Top (X/Y), Side (Y/Z: front to the right) and Rear (X/Z: seen from behind): orthographic;
        dragging places the source on the two axes you see, keeping the third (precise elevation
        in Side and Rear).
    Double-click the source to put it back ahead at the speakers; in the compact view (the mixer
    strip's), double-click opens the full panner.
*/
class SpatialPannerView final : public juce::Component, public juce::SettableTooltipClient
{
public:
    enum class Projection
    {
        perspective,
        top,
        side,
        rear,
    };

    explicit SpatialPannerView(bool compact = false);

    void setLayout(const engine::ChannelLayout& layout);
    void setProjection(Projection projection);
    [[nodiscard]] Projection getProjection() const noexcept { return projection_; }
    void setPosition(const SpatialPosition& position);
    [[nodiscard]] const SpatialPosition& getPosition() const noexcept { return position_; }
    /** Where the engine has it right now, when automation or an LFO moves it (drawn as the source;
        the handle then edits its own setting, shown faint). */
    void setLivePosition(std::optional<SpatialPosition> live);

    struct Keyframe
    {
        double time = 0.0;
        SpatialPosition position;
    };
    /** The trajectory: the keyframes (automation points) and positions along it, in time order. */
    void setTrajectory(std::vector<Keyframe> keyframes, std::vector<SpatialPosition> path);
    void setPlayheadPosition(std::optional<SpatialPosition> position);
    /** A tag in the view's corner (an ADM object: "OBJ"); empty for none. */
    void setBadge(const juce::String& badge);
    [[nodiscard]] const juce::String& getBadge() const noexcept { return badge_; }

    [[nodiscard]] juce::Point<float> project(const engine::Vec3& p) const;
    /** An orthographic view's point in the room: its two axes from `screen`, the third from `keep`. */
    [[nodiscard]] engine::Vec3 unproject(juce::Point<float> screen, const engine::Vec3& keep) const;
    [[nodiscard]] juce::Point<float> getSourcePoint() const { return project(position_.toVector()); }
    [[nodiscard]] juce::Point<float> getKeyframePoint(int index) const;
    [[nodiscard]] int getNumKeyframes() const noexcept { return static_cast<int>(keyframes_.size()); }

    std::function<void()> onDragStart;
    std::function<void(const SpatialPosition&)> onMove;
    std::function<void()> onDragEnd;
    std::function<void(int index, const SpatialPosition&)> onKeyframeMove;
    std::function<void(int index)> onKeyframeMenu; ///< right-click on a keyframe
    std::function<void()> onOpen;                  ///< double-click in the compact view

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    [[nodiscard]] juce::Rectangle<float> viewArea() const;
    [[nodiscard]] double scale() const;
    void drawRoom(juce::Graphics& g) const;
    void drawSpeakers(juce::Graphics& g) const;
    void drawSource(juce::Graphics& g, const SpatialPosition& p, bool faint) const;
    [[nodiscard]] SpatialPosition dragTo(const SpatialPosition& from, juce::Point<float> screen, juce::Point<float> delta) const;
    [[nodiscard]] int keyframeAt(juce::Point<float> point) const;

    bool compact_;
    engine::ChannelLayout layout_ = engine::ChannelLayout::surround714();
    Projection projection_ = Projection::perspective;
    SpatialPosition position_;
    std::optional<SpatialPosition> live_;
    std::vector<Keyframe> keyframes_;
    std::vector<SpatialPosition> path_;
    std::optional<SpatialPosition> playhead_;
    juce::String badge_;
    double yaw_ = 0.0; ///< perspective: the room turned (degrees)

    enum class Drag
    {
        none,
        source,
        keyframe,
        orbit,
    };
    Drag drag_ = Drag::none;
    int dragKeyframe_ = -1;
    SpatialPosition dragFrom_;
    juce::Point<float> dragStart_;
    bool dragMoved_ = false;
    double dragYaw_ = 0.0;
};

/**
    The full 3D panner for a track: the room view with Perspective / Top / Side / Rear buttons, the
    position's controls (Azimuth, Elevation, Distance, Size), and the trajectory: Add Keyframe
    writes the current position into the track's azimuth / elevation / distance (and size) lanes at
    the playhead; dragging a keyframe node moves it there; right-click a node to delete it; Clear
    Trajectory removes the lanes. Every gesture is one undo step.
*/
class SpatialPannerEditor final : public juce::Component, private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    SpatialPannerEditor(session::Session& session, juce::ValueTree track);
    ~SpatialPannerEditor() override;

    void setDestinationLayout(const engine::ChannelLayout& layout);
    /** The engine's moving values (azimuth, elevation, distance, size; NaN where they are the
        track's own) and the playhead, about 30 times a second. */
    void showLive(const std::array<float, 4>& moving, double playheadSeconds);

    [[nodiscard]] SpatialPannerView& getView() noexcept { return view_; }
    [[nodiscard]] const juce::ValueTree& getTrack() const noexcept { return track_; }

    // The trajectory (the four lanes' points, as keyframes).
    [[nodiscard]] std::vector<SpatialPannerView::Keyframe> getKeyframes() const;
    void addKeyframe(double time);
    void moveKeyframe(int index, const SpatialPosition& position);
    void deleteKeyframe(int index);
    void clearTrajectory();
    /** The track's own position (its settings). */
    [[nodiscard]] SpatialPosition getOwnPosition() const;
    void setOwnPosition(const SpatialPosition& position);

    juce::Slider azimuth, elevation, distance, size;
    juce::TextButton perspective{"3D"}, top{"Top"}, side{"Side"}, rear{"Rear"};
    juce::TextButton addKeyframeButton{"Add Keyframe"}, clearButton{"Clear Trajectory"};

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void refresh();
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refresh(); }
    [[nodiscard]] SpatialPosition positionAt(double time) const;

    session::Session& session_;
    juce::ValueTree track_;
    SpatialPannerView view_;
    std::array<juce::Label, 4> labels_;
    juce::Label hint_;
    double playhead_ = 0.0;
    bool updating_ = false;
};

/** A window holding a track's 3D panner. */
class SpatialPannerWindow final : public juce::DocumentWindow
{
public:
    SpatialPannerWindow(session::Session& session, juce::ValueTree track, const engine::ChannelLayout& destination);
    [[nodiscard]] SpatialPannerEditor& getEditor() noexcept { return *editor_; }
    void closeButtonPressed() override;
    std::function<void()> onClose;

private:
    SpatialPannerEditor* editor_ = nullptr;
};

} // namespace dojo::ui

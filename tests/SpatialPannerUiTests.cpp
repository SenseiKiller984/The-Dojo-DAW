#include "engine/ChannelLayout.h"
#include "session/Session.h"
#include "ui/EditorState.h"
#include "ui/components/MixerPanel.h"
#include "ui/components/SpatialPannerView.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>

using namespace dojo;
using namespace dojo::session;
using engine::ChannelLayout;
using Catch::Matchers::WithinAbs;
using Projection = ui::SpatialPannerView::Projection;

namespace
{
juce::MouseEvent mouse(juce::Component& c, juce::Point<float> p, juce::ModifierKeys mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                       int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods, juce::MouseInputSource::defaultPressure,
                            juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                            juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, p, now, clicks, false);
}

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
} // namespace

TEST_CASE("The 3D panner view: top, side, rear and perspective views; dragging places the source", "[ui][immersive][panner]")
{
    juce::MessageManager::getInstance();
    ui::SpatialPannerView view;
    view.setSize(400, 400);
    view.setLayout(ChannelLayout::surround714());
    const auto centre = view.project({0.0, 0.0, 0.0});

    // Top: left is left, the front is up.
    view.setProjection(Projection::top);
    view.setPosition({90.0, 0.0, 1.0, 0.0});
    CHECK(view.getSourcePoint().x < centre.x - 100.0f);
    view.setPosition({0.0, 0.0, 1.0, 0.0});
    CHECK(view.getSourcePoint().y < centre.y - 100.0f);
    // Side: up is up, the front to the right; Rear: up is up, left on the left.
    view.setProjection(Projection::side);
    view.setPosition({0.0, 60.0, 1.0, 0.0});
    CHECK(view.getSourcePoint().y < centre.y - 100.0f);
    CHECK(view.getSourcePoint().x > centre.x);
    view.setProjection(Projection::rear);
    view.setPosition({90.0, 30.0, 1.0, 0.0});
    CHECK(view.getSourcePoint().x < centre.x - 100.0f);
    CHECK(view.getSourcePoint().y < centre.y);
    // An orthographic view's point back into the room: its two axes, the third kept.
    const engine::Vec3 p{0.3, -0.4, 0.5};
    const auto back = view.unproject(view.project(p), {9.0, -0.4, 9.0});
    CHECK_THAT(back.x, WithinAbs(0.3, 1e-4));
    CHECK_THAT(back.y, WithinAbs(-0.4, 1e-9));
    CHECK_THAT(back.z, WithinAbs(0.5, 1e-4));

    ui::SpatialPosition moved;
    int moves = 0, starts = 0, ends = 0;
    view.onMove = [&](const ui::SpatialPosition& position)
    {
        moved = position;
        ++moves;
    };
    view.onDragStart = [&] { ++starts; };
    view.onDragEnd = [&] { ++ends; };

    // Top: a click at the right edge puts it at the right (azimuth -90), on the speakers' sphere.
    view.setProjection(Projection::top);
    view.setPosition({0.0, 0.0, 1.0, 0.0});
    const auto right = view.project({1.0, 0.0, 0.0});
    view.mouseDown(mouse(view, right));
    view.mouseUp(mouse(view, right));
    CHECK_THAT(moved.azimuth, WithinAbs(-90.0, 0.5));
    CHECK_THAT(moved.elevation, WithinAbs(0.0, 0.5));
    CHECK_THAT(moved.distance, WithinAbs(1.0, 0.01));
    CHECK(starts == 1);
    CHECK(ends == 1);

    // Rear: dragging it up raises it (the depth kept): from the left side to half way up.
    view.setProjection(Projection::rear);
    view.setPosition({90.0, 0.0, 1.0, 0.0});
    const auto from = view.getSourcePoint();
    const auto to = view.project({-0.5, 0.0, 0.5});
    view.mouseDown(mouse(view, from));
    view.mouseDrag(mouse(view, to));
    view.mouseUp(mouse(view, to));
    CHECK_THAT(moved.azimuth, WithinAbs(90.0, 0.5));
    CHECK_THAT(moved.elevation, WithinAbs(45.0, 0.5));
    CHECK_THAT(moved.distance, WithinAbs(std::sqrt(0.5), 0.01));

    // Perspective: dragging right turns it to the right, up raises it.
    view.setProjection(Projection::perspective);
    view.setPosition({0.0, 0.0, 1.0, 0.0});
    const auto source = view.getSourcePoint();
    view.mouseDown(mouse(view, source));
    view.mouseDrag(mouse(view, source + juce::Point<float>(50.0f, -40.0f)));
    view.mouseUp(mouse(view, source + juce::Point<float>(50.0f, -40.0f)));
    CHECK_THAT(moved.azimuth, WithinAbs(-30.0, 0.1));
    CHECK_THAT(moved.elevation, WithinAbs(18.0, 0.1));

    // Double-click: back ahead, at the speakers.
    view.mouseDoubleClick(mouse(view, view.getSourcePoint(), juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 2));
    CHECK_THAT(moved.azimuth, WithinAbs(0.0, 1e-9));
    CHECK_THAT(moved.elevation, WithinAbs(0.0, 1e-9));
    CHECK_THAT(moved.distance, WithinAbs(1.0, 1e-9));

    // It paints in every view (with a trajectory and a live position too).
    view.setTrajectory({{0.0, {0.0, 0.0, 1.0, 0.0}}, {2.0, {90.0, 30.0, 1.0, 0.2}}},
                       {{0.0, 0.0, 1.0, 0.0}, {45.0, 15.0, 1.0, 0.1}, {90.0, 30.0, 1.0, 0.2}});
    view.setLivePosition(ui::SpatialPosition{45.0, 15.0, 0.8, 0.5});
    for (const auto projection : {Projection::perspective, Projection::top, Projection::side, Projection::rear})
    {
        view.setProjection(projection);
        juce::Image image(juce::Image::ARGB, 400, 400, true);
        {
            juce::Graphics g(image); // (on Windows the drawing lands in the image when its context ends: Direct2D)
            view.paint(g);
        }
        CHECK(image.getPixelAt(static_cast<int>(view.getSourcePoint().x), static_cast<int>(view.getSourcePoint().y)).getAlpha() > 0);
    }
    CHECK(moves >= 4);
}

TEST_CASE("The 3D panner editor: its controls set the track, keyframes become its trajectory", "[ui][immersive][panner]")
{
    juce::MessageManager::getInstance();
    Session session;
    auto t = session.addTrack(TrackKind::audio, "Object");
    track::setPan3d(t, true, nullptr);
    ui::SpatialPannerEditor editor(session, t);
    editor.setSize(720, 640);

    // The controls (each change one undo step).
    session.beginTransaction("setup");
    editor.azimuth.setValue(45.0, juce::sendNotificationSync);
    editor.elevation.setValue(20.0, juce::sendNotificationSync);
    editor.size.setValue(50.0, juce::sendNotificationSync);
    CHECK_THAT(track::getAzimuth(t), WithinAbs(45.0, 1e-9));
    CHECK_THAT(track::getElevation(t), WithinAbs(20.0, 1e-9));
    CHECK_THAT(track::getSize(t), WithinAbs(0.5, 1e-9));
    session.getUndoManager().undo();
    CHECK_THAT(track::getSize(t), WithinAbs(0.0, 1e-9));
    CHECK_THAT(track::getElevation(t), WithinAbs(20.0, 1e-9));

    // Keyframes: the position, written at the playhead into the azimuth / elevation / distance lanes.
    CHECK(editor.getKeyframes().empty());
    editor.setOwnPosition({-60.0, 10.0, 0.8, 0.0});
    editor.addKeyframe(2.0);
    editor.setOwnPosition({120.0, 40.0, 0.5, 0.0});
    editor.addKeyframe(4.0);
    auto keyframes = editor.getKeyframes();
    REQUIRE(keyframes.size() == 3); // (a lane starts with its first point at 0)
    CHECK(keyframes[1].time == 2.0);
    CHECK_THAT(keyframes[1].position.azimuth, WithinAbs(-60.0, 0.01));
    CHECK_THAT(keyframes[2].position.azimuth, WithinAbs(120.0, 0.01));
    CHECK_THAT(keyframes[2].position.elevation, WithinAbs(40.0, 0.01));
    CHECK_THAT(keyframes[2].position.distance, WithinAbs(0.5, 0.01));
    CHECK(session.getAutomation(t, automation::azimuthTarget(), false).isValid());
    CHECK_FALSE(session.getAutomation(t, automation::sizeTarget(), false).isValid());
    REQUIRE(test::pumpMessagesUntil([&] { return editor.getView().getNumKeyframes() == 3; }));
    // In the top view the keyframes sit where they point.
    editor.getView().setProjection(Projection::top);
    CHECK(editor.getView().getKeyframePoint(2).x < editor.getView().project({0.0, 0.0, 0.0}).x); // (120 degrees: left)

    // Dragging a keyframe node moves it; right-click deletes it; undo brings it back.
    session.beginTransaction("Move");
    editor.moveKeyframe(1, {30.0, 45.0, 1.0, 0.0});
    keyframes = editor.getKeyframes();
    CHECK_THAT(keyframes[1].position.azimuth, WithinAbs(30.0, 0.01));
    CHECK_THAT(keyframes[1].position.elevation, WithinAbs(45.0, 0.01));
    editor.deleteKeyframe(1);
    CHECK(editor.getKeyframes().size() == 2);
    session.getUndoManager().undo();
    CHECK(editor.getKeyframes().size() == 3);

    // The live position (automation moving it) and the playhead on the trajectory.
    editor.showLive({90.0f, kNaN, kNaN, kNaN}, 3.0);

    // Clear Trajectory removes the lanes (one undo step).
    editor.clearTrajectory();
    CHECK(editor.getKeyframes().empty());
    CHECK_FALSE(session.getAutomation(t, automation::azimuthTarget(), false).isValid());
    session.getUndoManager().undo();
    CHECK(editor.getKeyframes().size() == 3);
}

TEST_CASE("The mixer strip's 3D panner: the 3D switch, the compact view, the full panner", "[ui][immersive][panner][mixer]")
{
    juce::MessageManager::getInstance();
    Session session;
    ui::EditorState editorState;
    session.setMasterFormat(TrackFormat::surround714);
    auto vocal = session.addTrack(TrackKind::audio, "Vocal");
    ui::ChannelStrip strip(session, editorState, vocal, [] { return juce::StringArray{"1", "2"}; }, [] {});
    strip.setSize(ui::MixerPanel::kStripWidth, 520);

    // A stereo track into a 7.1.4 master: the surround pad, with "3D" in its corner.
    REQUIRE(strip.isSurroundPanning());
    CHECK(strip.getSurroundPanner().isVisible());
    CHECK(strip.get3dButton().isVisible());
    CHECK_FALSE(strip.getSpatialPanner().isVisible());

    // "3D": the compact 3D panner (the room from above) takes the pad's place. One undo step.
    REQUIRE(strip.get3dButton().onClick != nullptr);
    strip.get3dButton().onClick();
    CHECK(track::isPan3d(vocal));
    REQUIRE(test::pumpMessagesUntil([&] { return strip.getSpatialPanner().isVisible(); }));
    CHECK_FALSE(strip.getSurroundPanner().isVisible());
    auto& view = strip.getSpatialPanner();
    CHECK(view.getProjection() == Projection::top);
    REQUIRE(view.getWidth() > 20);

    // A click alone does nothing (a double-click opens the full panner); a drag to the room's left
    // places it there.
    const auto middle = view.project({0.0, 0.0, 0.0});
    const auto left = view.project({-1.0, 0.0, 0.0});
    view.mouseDown(mouse(view, middle));
    view.mouseUp(mouse(view, middle));
    CHECK_THAT(track::getDistance(vocal), WithinAbs(1.0, 1e-9));
    view.mouseDown(mouse(view, middle));
    view.mouseDrag(mouse(view, left));
    view.mouseUp(mouse(view, left));
    CHECK_THAT(track::getAzimuth(vocal), WithinAbs(90.0, 1.0));
    CHECK_THAT(track::getDistance(vocal), WithinAbs(1.0, 0.02));

    // A double-click opens the full panner.
    juce::ValueTree opened;
    strip.onOpenSpatialPanner = [&](const juce::ValueTree& t) { opened = t; };
    view.mouseDoubleClick(mouse(view, left, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), 2));
    CHECK(opened == vocal);

    // While automation moves it, the compact view shows where it is.
    strip.showMovingSpatial({-90.0f, 30.0f, kNaN, kNaN});
    strip.showMovingSpatial({kNaN, kNaN, kNaN, kNaN});

    session.getUndoManager().undo(); // (the move)
    session.getUndoManager().undo(); // (3D off again)
    CHECK_FALSE(track::isPan3d(vocal));
    REQUIRE(test::pumpMessagesUntil([&] { return strip.getSurroundPanner().isVisible(); }));
}

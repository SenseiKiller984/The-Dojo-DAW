// Warp markers and hitpoints: the map from a clip's time to its source, kept through edits; the
// attacks found in audio; markers dragged, added and deleted on the arranger; quantized to the grid;
// and the engine playing the source through them.

#include "core/WarpMap.h"
#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Project.h"
#include "session/TempoDetector.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using Catch::Matchers::WithinAbs;
namespace clip = session::clip;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

/** Percussive hits: a burst of decaying noise at each time (seconds), `level` peak. */
std::vector<float> hits(const std::vector<double>& times, double seconds, float level = 0.6f, std::uint32_t seed = 7)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * kRate), 0.0f);
    juce::Random random(seed);
    for (const double t : times)
    {
        const auto start = static_cast<std::size_t>(std::llround(t * kRate));
        for (std::size_t i = 0; i < static_cast<std::size_t>(0.08 * kRate) && start + i < x.size(); ++i)
            x[start + i] += level * std::exp(-static_cast<float>(i) / static_cast<float>(0.012 * kRate)) * (random.nextFloat() * 2.0f - 1.0f);
    }
    return x;
}

/** The time of every attack found in `x` (seconds). */
std::vector<double> attacks(const std::vector<float>& x, double sensitivity = 0.5)
{
    return session::tempo::detectHitpoints(x.data(), static_cast<int>(x.size()), kRate, sensitivity);
}

/** The attack nearest to `t`, or a large value. */
double nearest(const std::vector<double>& found, double t)
{
    double best = 1.0e9;
    for (const double f : found)
        if (std::abs(f - t) < std::abs(best - t))
            best = f;
    return best;
}

/** An arranger over an audio track with one clip, for drags and clicks. */
struct ArrangerRig
{
    ArrangerRig()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); })
    {
        juce::MessageManager::getInstance();
        auto& session = project.getSession();
        track = session.addTrack(session::TrackKind::audio, "Drums");
        c = session.addClip(track, juce::File("/audio/drums.wav"), 2.0, 8.0);
        arranger = std::make_unique<ui::ArrangerView>(project, controller, editor);
        arranger->setSize(1600, 600);
    }

    juce::MouseEvent event(juce::Point<float> at, juce::ModifierKeys mods, juce::Point<float> downAt, int clicks = 1) const
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, arranger.get(),
                                arranger.get(), now, downAt, now, clicks, at != downAt);
    }

    /** The x of a time on the arranger, and the middle of the clip's body at it. */
    juce::Point<float> at(double seconds) const
    {
        const auto area = arranger->getClipArea(c).toFloat();
        const float x = area.getX() + static_cast<float>((seconds - clip::getStart(c)) / clip::getLength(c)) * area.getWidth();
        return {x, area.getCentreY() + 6.0f};
    }

    void drag(juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys extra = {})
    {
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier | extra.getRawFlags());
        arranger->mouseDown(event(from, mods, from));
        arranger->mouseDrag(event((from + to) * 0.5f, mods, from));
        arranger->mouseDrag(event(to, mods, from));
        arranger->mouseUp(event(to, mods, from));
    }

    void doubleClick(juce::Point<float> position, juce::ModifierKeys extra = {})
    {
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | extra.getRawFlags());
        arranger->mouseDoubleClick(event(position, mods, position, 2));
    }

    engine::AudioEngine engine{0};
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    session::Project project;
    ui::EditorState editor;
    std::unique_ptr<ui::ArrangerView> arranger;
    juce::ValueTree track, c;
};
} // namespace

TEST_CASE("Warp map: the source through the markers, back again, and the speed between them", "[warp]")
{
    // No markers: the plain stretch.
    CHECK_THAT(warp::sourceAt(nullptr, 0, 1.0, 2.0, 4.0), WithinAbs(3.0, 1e-12));
    CHECK_THAT(warp::clipTimeOf(nullptr, 0, 1.0, 2.0, 3.0), WithinAbs(4.0, 1e-12));
    CHECK_THAT(warp::rateAt(nullptr, 0, 1.0, 2.0, 4.0), WithinAbs(0.5, 1e-12));

    // Through (0, 0.5) [the start], (2, 1.5) and (4, 2.0), then at the stretch (1).
    const std::vector<WarpPoint> markers{{2.0, 1.5}, {4.0, 2.0}};
    const auto source = [&](double t) { return warp::sourceAt(markers.data(), markers.size(), 0.5, 1.0, t); };
    const auto clipTime = [&](double s) { return warp::clipTimeOf(markers.data(), markers.size(), 0.5, 1.0, s); };
    CHECK_THAT(source(0.0), WithinAbs(0.5, 1e-12));
    CHECK_THAT(source(1.0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(source(2.0), WithinAbs(1.5, 1e-12));
    CHECK_THAT(source(3.0), WithinAbs(1.75, 1e-12));
    CHECK_THAT(source(5.0), WithinAbs(3.0, 1e-12));
    CHECK_THAT(source(-1.0), WithinAbs(-0.5, 1e-12));
    for (const double t : {0.0, 0.7, 2.0, 3.3, 4.0, 6.5})
        CHECK_THAT(clipTime(source(t)), WithinAbs(t, 1e-9));
    CHECK_THAT(warp::rateAt(markers.data(), markers.size(), 0.5, 1.0, 1.0), WithinAbs(0.5, 1e-12));
    CHECK_THAT(warp::rateAt(markers.data(), markers.size(), 0.5, 1.0, 3.0), WithinAbs(0.25, 1e-12));
    CHECK_THAT(warp::rateAt(markers.data(), markers.size(), 0.5, 1.0, 5.0), WithinAbs(1.0, 1e-12));
}

TEST_CASE("Warp markers on clips: in order and in reach, through splits", "[warp][session]")
{
    session::Session session;
    auto t = session.addTrack();
    auto c = session.addClip(t, juce::File("/audio/a.wav"), 1.0, 8.0);
    clip::setOffset(c, 0.5, nullptr);
    CHECK_FALSE(clip::isWarped(c));
    CHECK_THAT(clip::sourceAt(c, 2.0), WithinAbs(2.5, 1e-12));

    // Sorted; a marker that would run backwards, sit on another, or play faster than 4x is left out.
    clip::setWarpMarkers(c, {{5.0, 4.0}, {2.0, 1.5}, {2.0005, 1.6}, {3.0, 1.2}, {6.0, 9.0}}, nullptr);
    const auto markers = clip::getWarpMarkers(c);
    REQUIRE(markers.size() == 2);
    CHECK(markers[0].clipTime == 2.0);
    CHECK(markers[1].clipTime == 5.0);
    CHECK(clip::isWarped(c));
    CHECK_THAT(clip::sourceAt(c, 1.0), WithinAbs(1.0, 1e-9));
    CHECK_THAT(clip::clipTimeOf(c, 3.0), WithinAbs(2.0 + (3.0 - 1.5) * 3.0 / 2.5, 1e-9));

    // A split: both parts play what the clip did, where it did.
    std::vector<double> before;
    for (double x = 0.0; x < 8.0; x += 0.25)
        before.push_back(clip::sourceAt(c, x));
    session.beginTransaction("Split");
    auto right = session.splitClip(c, 1.0 + 3.5);
    REQUIRE(right.isValid());
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        const double x = 0.25 * static_cast<double>(i);
        const double after = x < 3.5 ? clip::sourceAt(c, x) : clip::sourceAt(right, x - 3.5);
        CHECK_THAT(after, WithinAbs(before[i], 1e-6));
    }
    // The left part ends with a marker at the split; the right part keeps the one after it.
    CHECK(clip::getWarpMarkers(c).back().clipTime == 3.5);
    CHECK(clip::getWarpMarkers(right).size() == 1);
    session.getUndoManager().undo();
    CHECK(clip::getWarpMarkers(c).size() == 2);

    // Clearing them.
    clip::setWarpMarkers(c, {}, nullptr);
    CHECK_FALSE(clip::isWarped(c));

    // Hitpoints (source seconds) are kept as they are.
    clip::setHitpoints(c, {0.75, 1.25, 2.5}, nullptr);
    CHECK(clip::getHitpoints(c) == std::vector<double>{0.75, 1.25, 2.5});
}

TEST_CASE("Hitpoints: the attacks of percussive audio, to the millisecond", "[warp][tempo]")
{
    const std::vector<double> times{0.25, 0.6, 1.0, 1.37, 1.8, 2.15, 2.6};
    const auto found = attacks(hits(times, 3.0));
    REQUIRE(found.size() == times.size());
    for (std::size_t i = 0; i < times.size(); ++i)
        CHECK_THAT(found[i], WithinAbs(times[i], 0.002));

    // Soft hits among loud ones: found when more sensitive.
    auto mixed = hits({0.3, 1.3, 2.3}, 3.0, 0.8f);
    const auto soft = hits({0.8, 1.8}, 3.0, 0.006f, 11);
    for (std::size_t i = 0; i < mixed.size(); ++i)
        mixed[i] += soft[i];
    CHECK(attacks(mixed, 0.1).size() < attacks(mixed, 0.95).size());
    CHECK(std::abs(nearest(attacks(mixed, 0.95), 0.8) - 0.8) < 0.003);

    // Silence has none; a held tone has (at most) its start.
    CHECK(attacks(std::vector<float>(static_cast<std::size_t>(kRate), 0.0f)).empty());
    CHECK(attacks(test::sine(220.0, kRate, 96000, 0.3f)).size() <= 1);
}

TEST_CASE("Clip > Warp: hitpoints detected from the file, markers at them, quantized to the grid, cleared", "[warp][ui]")
{
    test::TempFolder temp;
    // Hits a little off a 0.25 s grid (eighths at 120 BPM), file starting at 0.
    const std::vector<double> played{0.0 + 0.012, 0.25 - 0.018, 0.5 + 0.009, 0.75 + 0.02, 1.0 - 0.011, 1.25 + 0.015};
    const auto file = test::writeWav(temp.dir.getChildFile("loop.wav"), hits(played, 1.6), kRate);
    session::Session session;
    ui::EditorState editor;
    auto t = session.addTrack();
    auto c = session.addClip(t, file, 4.0, 1.6);
    editor.selectClip(clip::getId(c), false);

    juce::String error;
    REQUIRE(ui::actions::detectSelectedClipsHitpoints(session, editor, 0.5, error) == 1);
    const auto hitpoints = clip::getHitpoints(c);
    REQUIRE(hitpoints.size() == played.size());
    for (std::size_t i = 0; i < played.size(); ++i)
        CHECK_THAT(hitpoints[i], WithinAbs(played[i], 0.002));
    CHECK(ui::actions::selectionHasHitpoints(session, editor));
    CHECK_FALSE(ui::actions::selectionHasWarpMarkers(session, editor));

    // Markers at the hitpoints: where they play now (nothing moves).
    REQUIRE(ui::actions::warpSelectedClipsAtHitpoints(session, editor));
    auto markers = clip::getWarpMarkers(c);
    CHECK(markers.size() == played.size());
    for (const auto& marker : markers)
        CHECK_THAT(marker.clipTime, WithinAbs(marker.source, 1e-9)); // (unstretched: the same)

    // Quantized to 0.25 s: each attack lands on the grid (the first, 12 ms in, on the clip's start:
    // the start already plays it there, so it needs no marker).
    REQUIRE(ui::actions::quantizeSelectedClipsWarp(session, editor, 0.25));
    markers = clip::getWarpMarkers(c);
    REQUIRE(markers.size() == played.size() - 1);
    for (std::size_t i = 0; i < markers.size(); ++i)
    {
        const double at = clip::getStart(c) + markers[i].clipTime;
        CHECK_THAT(at, WithinAbs(std::round(at / 0.25) * 0.25, 1e-9));
        CHECK_THAT(markers[i].source, WithinAbs(played[i + 1], 0.002));
    }
    session.getUndoManager().undo();
    CHECK_THAT(clip::getWarpMarkers(c)[0].clipTime, WithinAbs(played[0], 0.002));

    // Half strength: half way there.
    REQUIRE(ui::actions::quantizeSelectedClipsWarp(session, editor, 0.25, 0.5));
    CHECK_THAT(clip::getWarpMarkers(c)[1].clipTime, WithinAbs((played[1] + 0.25) * 0.5, 0.002));

    // A clip with only hitpoints is quantized straight away (markers at them first).
    REQUIRE(ui::actions::clearSelectedClipsWarp(session, editor));
    CHECK_FALSE(clip::isWarped(c));
    REQUIRE(ui::actions::quantizeSelectedClipsWarp(session, editor, 0.25));
    CHECK(clip::getWarpMarkers(c).size() == played.size() - 1);
}

TEST_CASE("The engine plays the source through the warp markers", "[warp][engine]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("hits.wav"), hits({0.5, 1.0, 1.5, 2.0}, 2.5), kRate);
    session::Session session;
    session::AudioAssetCache assets;
    session::DeviceManager devices(session, [](const juce::ValueTree& d, double, int, juce::String&)
                                   { return devices::createBuiltinDevice(session::device::getType(d)); });
    session::GraphBuilder builder;
    engine::AudioEngine engine(2);
    engine.prepare(kRate, kBlock, 2);
    devices.setPlayHead(engine.getPlayHead());
    devices.prepareAll(kRate, kBlock);
    builder.setRenderFormat(kRate, kBlock);
    juce::String error;
    REQUIRE(assets.loadNow(file, error) != nullptr);
    auto t = session.addTrack();
    auto c = session.addClip(t, file, 0.0, 2.5);

    const auto render = [&]
    {
        auto graph = builder.build(session, assets, &devices);
        REQUIRE(graph->tracks.size() == 1);
        graph->serial = 1;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        std::vector<float> out;
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(2.5 * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            engine.releaseRetiredGraphs();
            out.insert(out.end(), l.begin(), l.end());
        }
        REQUIRE(engine.post(engine::command::SetPlaying{false}));
        engine.process(nullptr, 0, channels, 2, kBlock);
        return attacks(out);
    };

    // The hit at 1.0 s of the source now plays at 1.2 s; the one before it at 0.6 s (that stretch
    // slowed down), those after it 0.2 s later (the stretch after the last marker is the clip's).
    clip::setWarpMarkers(c, {{1.2, 1.0}}, nullptr);
    const auto warped = render();
    CHECK(std::abs(nearest(warped, 0.6) - 0.6) < 0.006);
    CHECK(std::abs(nearest(warped, 1.2) - 1.2) < 0.006);
    CHECK(std::abs(nearest(warped, 1.7) - 1.7) < 0.006);
    CHECK(std::abs(nearest(warped, 2.2) - 2.2) < 0.006);
    CHECK(std::abs(nearest(warped, 1.0) - 1.0) > 0.05); // (nothing left where it was)
}

TEST_CASE("Warp markers on the arranger: drawn, dragged, added and deleted; trims keep the audio in place", "[warp][ui]")
{
    ArrangerRig rig;
    auto& c = rig.c;
    clip::setHitpoints(c, {1.0, 3.0, 5.0}, nullptr);
    clip::setWarpMarkers(c, {{2.0, 2.0}, {4.0, 4.0}}, nullptr);
    rig.editor.selectClip(clip::getId(c), false);
    (void) rig.arranger->createComponentSnapshot(rig.arranger->getLocalBounds()); // (painting the markers and ticks)

    // Dragging the marker at clip time 2 s (timeline 4 s) half a second later (Alt: no snapping).
    const auto from = rig.at(4.0);
    rig.drag(from, rig.at(4.5));
    auto markers = clip::getWarpMarkers(c);
    REQUIRE(markers.size() == 2);
    CHECK_THAT(markers[0].clipTime, WithinAbs(2.5, 0.02));
    CHECK_THAT(markers[0].source, WithinAbs(2.0, 1e-9)); // (the same moment of the source)
    // Not past its neighbour.
    rig.drag(rig.at(2.0 + markers[0].clipTime), rig.at(7.5));
    markers = clip::getWarpMarkers(c);
    CHECK(markers[0].clipTime < markers[1].clipTime);
    rig.project.getSession().getUndoManager().undo();
    rig.project.getSession().getUndoManager().undo();
    CHECK_THAT(clip::getWarpMarkers(c)[0].clipTime, WithinAbs(2.0, 1e-9));

    // Double-click on a marker deletes it; Shift + double-click adds one; on a hitpoint's tick, there.
    rig.doubleClick(rig.at(4.0));
    REQUIRE(clip::getWarpMarkers(c).size() == 1);
    rig.doubleClick(rig.at(5.0), juce::ModifierKeys::shiftModifier);
    markers = clip::getWarpMarkers(c);
    REQUIRE(markers.size() == 2);
    CHECK_THAT(markers[0].clipTime, WithinAbs(3.0, 0.02));
    const auto area = rig.arranger->getClipArea(c).toFloat();
    rig.doubleClick({rig.at(2.0 + 5.0).x + 1.0f, area.getBottom() - 3.0f}); // (the hitpoint at source 5 s)
    markers = clip::getWarpMarkers(c);
    REQUIRE(markers.size() == 3);
    CHECK_THAT(markers[2].source, WithinAbs(5.0, 1e-6));

    // Trimming the start keeps every moment of the source where it played.
    std::vector<double> before;
    for (double x = 3.0; x < 9.5; x += 0.25)
        before.push_back(clip::sourceAt(c, x - 2.0));
    const auto leftEdge = juce::Point<float>(area.getX() + 2.0f, area.getBottom() - 6.0f);
    rig.drag(leftEdge, leftEdge.translated(static_cast<float>(area.getWidth() / 8.0), 0.0f)); // (1 s in)
    REQUIRE(clip::getStart(c) > 2.9);
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        const double x = 3.0 + 0.25 * static_cast<double>(i);
        if (x >= clip::getStart(c))
            CHECK_THAT(clip::sourceAt(c, x - clip::getStart(c)), WithinAbs(before[i], 1e-6));
    }
    (void) rig.arranger->createComponentSnapshot(rig.arranger->getLocalBounds());
}

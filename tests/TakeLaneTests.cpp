// Take lanes and comping: takes in lanes under a track, swiped into the comp with quick
// crossfades; recording over earlier material and loop recording into lanes; auditioning a lane;
// the arranger's lanes, T and S buttons; and lanes kept through copies and saves.

#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;
constexpr double kHalfFade = 0.005; ///< half the default comp crossfade (10 ms)

const juce::File fileA("/takes/a.wav");
const juce::File fileB("/takes/b.wav");
const juce::File fileC("/takes/c.wav");

/** A track's comp: its clips in time order. */
std::vector<juce::ValueTree> compOf(const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& c : track)
        if (Session::isClip(c))
            clips.push_back(c);
    std::stable_sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) { return clip::getStart(a) < clip::getStart(b); });
    return clips;
}

/** A new take lane holding one take: `length` seconds of `file` at `start`. */
juce::ValueTree laneWith(Session& session, juce::ValueTree track, const juce::File& file, double start, double length, double offset = 0.0)
{
    auto lane = session.addTakeLane(track);
    session.addClip(lane, file, start, length, offset);
    return lane;
}

struct MessageThread
{
    MessageThread() { juce::MessageManager::getInstance(); } // this thread is the message thread
};

/** The application's object graph without an audio device (the test drives process()). */
struct App
{
    App()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); })
    {
    }

    MessageThread messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    Project project;
};

std::vector<float> decodeLeft(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    juce::AudioBuffer<float> audio(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
    return {audio.getReadPointer(0), audio.getReadPointer(0) + audio.getNumSamples()};
}

double at(const std::vector<float>& x, double seconds)
{
    return x[std::min(x.size() - 1, static_cast<std::size_t>(seconds * kRate))];
}
} // namespace

TEST_CASE("Comping a take: quick crossfades centred on the edits, the same take joined, no drift", "[takes][session]")
{
    Session session;
    auto t = session.addTrack("Vocal");
    auto take1 = laneWith(session, t, fileA, 0.0, 10.0);
    auto take2 = laneWith(session, t, fileB, 0.0, 10.0);
    CHECK(takelane::getName(take1) == "Take 1");
    CHECK(takelane::getName(take2) == "Take 2");
    CHECK_THAT(session.getCompCrossfade(), WithinAbs(0.010, 1e-12));
    CHECK(session.isRecordingTakesToLanes());
    CHECK_FALSE(session.isLoopRecording());
    CHECK(compOf(t).empty()); // (takes do not play)

    session.beginTransaction("Comp");
    session.compTake(take1, 0.0, 10.0);
    auto comp = compOf(t);
    REQUIRE(comp.size() == 1);
    CHECK(clip::getFile(comp[0]) == fileA);
    CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(10.0, 1e-9));
    CHECK(session.getTakeLaneOf(comp[0]) == take1);

    // Take 2 between 4 and 6 s: each edit crossfades over 10 ms centred on it.
    session.beginTransaction("Comp");
    session.compTake(take2, 4.0, 6.0);
    comp = compOf(t);
    REQUIRE(comp.size() == 3);
    CHECK(clip::getFile(comp[0]) == fileA);
    CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(4.0 + kHalfFade, 1e-9));
    CHECK_THAT(clip::getFadeOut(comp[0]), WithinAbs(0.0, 1e-12)); // (the overlap is the crossfade)
    CHECK(clip::getFile(comp[1]) == fileB);
    CHECK_THAT(clip::getStart(comp[1]), WithinAbs(4.0 - kHalfFade, 1e-9));
    CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(6.0 + kHalfFade, 1e-9));
    CHECK_THAT(clip::getOffset(comp[1]), WithinAbs(4.0 - kHalfFade, 1e-9)); // (the take's own time)
    CHECK(session.getTakeLaneOf(comp[1]) == take2);
    CHECK(clip::getFile(comp[2]) == fileA);
    CHECK_THAT(clip::getStart(comp[2]), WithinAbs(6.0 - kHalfFade, 1e-9));
    CHECK_THAT(clip::getOffset(comp[2]), WithinAbs(6.0 - kHalfFade, 1e-9));
    CHECK(session.getTakeLaneOf(comp[2]) == take1);

    const auto ranges = Session::getCompedRanges(take2);
    REQUIRE(ranges.size() == 1);
    CHECK_THAT(ranges[0].first, WithinAbs(4.0 - kHalfFade, 1e-9));
    CHECK(Session::getCompedRanges(take1).size() == 2);

    // The same swipe again changes nothing (no edit creeps).
    const auto snapshot = [&t]
    {
        std::vector<std::pair<double, double>> s;
        for (const auto& c : compOf(t))
            s.emplace_back(clip::getStart(c), clip::getEnd(c));
        return s;
    };
    const auto before = snapshot();
    session.beginTransaction("Comp");
    session.compTake(take2, 4.0, 6.0);
    session.compTake(take2, 4.2, 5.8);
    const auto after = snapshot();
    REQUIRE(after.size() == before.size());
    for (std::size_t i = 0; i < before.size(); ++i)
    {
        CHECK_THAT(after[i].first, WithinAbs(before[i].first, 1e-9));
        CHECK_THAT(after[i].second, WithinAbs(before[i].second, 1e-9));
    }

    // Extending the stretch of take 2 to 7 s: one clip still, its first edit (and crossfade) as it
    // was, a new crossfade at 7 s.
    session.beginTransaction("Comp");
    session.compTake(take2, 5.5, 7.0);
    comp = compOf(t);
    REQUIRE(comp.size() == 3);
    CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(4.0 + kHalfFade, 1e-9));
    CHECK_THAT(clip::getStart(comp[1]), WithinAbs(4.0 - kHalfFade, 1e-9));
    CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(7.0 + kHalfFade, 1e-9));
    CHECK_THAT(clip::getStart(comp[2]), WithinAbs(7.0 - kHalfFade, 1e-9));
    CHECK_THAT(clip::getOffset(comp[2]), WithinAbs(7.0 - kHalfFade, 1e-9));

    // A third take swiped across an earlier edit: the crossfade it replaces leaves nothing behind.
    auto take3 = laneWith(session, t, fileC, 0.0, 10.0);
    session.beginTransaction("Comp");
    session.compTake(take3, 3.0, 4.0);
    comp = compOf(t);
    REQUIRE(comp.size() == 4);
    CHECK(clip::getFile(comp[0]) == fileA);
    CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(3.0 + kHalfFade, 1e-9));
    CHECK(clip::getFile(comp[1]) == fileC);
    CHECK_THAT(clip::getStart(comp[1]), WithinAbs(3.0 - kHalfFade, 1e-9));
    CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(4.0 + kHalfFade, 1e-9));
    CHECK(clip::getFile(comp[2]) == fileB);
    CHECK_THAT(clip::getStart(comp[2]), WithinAbs(4.0 - kHalfFade, 1e-9)); // take 2's edit, kept as it was
    CHECK(clip::getFile(comp[3]) == fileA);

    // Each comp is one undo step.
    REQUIRE(session.getUndoManager().undo());
    CHECK(compOf(t).size() == 3);
}

TEST_CASE("Comping: plain cuts within a take, quick fades into silence, locked clips left alone", "[takes][session]")
{
    Session session;
    auto t = session.addTrack("Guitar");
    auto take1 = laneWith(session, t, fileA, 0.0, 10.0);
    session.compTake(take1, 0.0, 10.0);

    SECTION("the comp goes on with the same material: an equal-gain crossfade (linear fades)")
    {
        auto whole = compOf(t)[0];
        clip::setGainDb(whole, -3.0, nullptr); // (changed by hand: it no longer joins the take)
        session.compTake(take1, 4.0, 6.0);
        const auto comp = compOf(t);
        REQUIRE(comp.size() == 3);
        CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(4.0 + kHalfFade, 1e-9));
        CHECK_THAT(clip::getFadeOut(comp[0]), WithinAbs(0.010, 1e-8));
        CHECK_THAT(clip::getStart(comp[1]), WithinAbs(4.0 - kHalfFade, 1e-9));
        CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(6.0 + kHalfFade, 1e-9));
        CHECK_THAT(clip::getFadeIn(comp[1]), WithinAbs(0.010, 1e-8));
        CHECK_THAT(clip::getFadeOut(comp[1]), WithinAbs(0.010, 1e-8));
        CHECK_THAT(clip::getGainDb(comp[1]), WithinAbs(0.0, 1e-12));
        CHECK_THAT(clip::getStart(comp[2]), WithinAbs(6.0 - kHalfFade, 1e-9));
        CHECK_THAT(clip::getFadeIn(comp[2]), WithinAbs(0.010, 1e-8));
        CHECK_THAT(clip::getGainDb(comp[2]), WithinAbs(-3.0, 1e-12));
    }

    SECTION("silence comes in where the take has none; a take starting inside gets a quick fade")
    {
        auto short3 = laneWith(session, t, fileC, 2.0, 1.0, 5.0); // a take only from 2 to 3 s
        session.compTake(short3, 1.0, 5.0);
        auto comp = compOf(t);
        REQUIRE(comp.size() == 3);
        CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(1.0, 1e-9));
        CHECK_THAT(clip::getFadeOut(comp[0]), WithinAbs(0.010, 1e-12)); // fades out into the silence
        CHECK(clip::getFile(comp[1]) == fileC);
        CHECK_THAT(clip::getStart(comp[1]), WithinAbs(2.0, 1e-9));
        CHECK_THAT(clip::getOffset(comp[1]), WithinAbs(5.0, 1e-9));
        CHECK_THAT(clip::getStart(comp[2]), WithinAbs(5.0, 1e-9));
        CHECK_THAT(clip::getFadeIn(comp[2]), WithinAbs(0.010, 1e-12));

        // A swipe inside a take, with silence on both sides: quick fades on the take.
        auto take4 = laneWith(session, t, fileB, 0.0, 10.0);
        session.compTake(take4, 3.5, 4.5);
        comp = compOf(t);
        REQUIRE(comp.size() == 4);
        CHECK(clip::getFile(comp[2]) == fileB);
        CHECK_THAT(clip::getFadeIn(comp[2]), WithinAbs(0.010, 1e-12));
        CHECK_THAT(clip::getFadeOut(comp[2]), WithinAbs(0.010, 1e-12));
    }

    SECTION("a time-locked comp clip stays as it is")
    {
        clip::setTimeLocked(compOf(t)[0], true, nullptr);
        auto take2 = laneWith(session, t, fileB, 0.0, 10.0);
        session.compTake(take2, 4.0, 6.0);
        const auto comp = compOf(t);
        REQUIRE(comp.size() == 2);
        CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(10.0, 1e-9));
        CHECK(clip::getFile(comp[1]) == fileB);
        CHECK_FALSE(clip::isTimeLocked(comp[1]));
    }

    SECTION("crossfades off: plain cuts between takes")
    {
        session.setCompCrossfade(0.0);
        auto take2 = laneWith(session, t, fileB, 0.0, 10.0);
        session.compTake(take2, 4.0, 6.0);
        const auto comp = compOf(t);
        REQUIRE(comp.size() == 3);
        CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(4.0, 1e-9));
        CHECK_THAT(clip::getStart(comp[1]), WithinAbs(4.0, 1e-9));
        CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(6.0, 1e-9));
    }
}

TEST_CASE("Comping takes that begin right at the edits leaves nothing behind in the crossfades", "[takes][session]")
{
    // Loop passes: takes of exactly 2 - 4 s (nothing before or after), comped one after another
    // over an earlier take.
    Session session;
    auto t = session.addTrack("Vocal");
    auto earlier = laneWith(session, t, fileA, 0.0, 10.0);
    session.compTake(earlier, 0.0, 10.0);
    auto pass1 = laneWith(session, t, fileB, 2.0, 2.0, 1.0);
    auto pass2 = laneWith(session, t, fileC, 2.0, 2.0, 3.0);
    session.compTake(pass1, 2.0, 4.0);
    session.compTake(pass2, 2.0, 4.0);

    // The crossfades lie on the earlier take's side (the passes have nothing beyond their edges).
    const auto comp = compOf(t);
    REQUIRE(comp.size() == 3);
    CHECK(clip::getFile(comp[0]) == fileA);
    CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(2.0 + 2.0 * kHalfFade, 1e-9));
    CHECK(clip::getFile(comp[1]) == fileC);
    CHECK_THAT(clip::getStart(comp[1]), WithinAbs(2.0, 1e-9));
    CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(4.0, 1e-9));
    CHECK(clip::getFile(comp[2]) == fileA);
    CHECK_THAT(clip::getStart(comp[2]), WithinAbs(4.0 - 2.0 * kHalfFade, 1e-9));
    CHECK(Session::getCompedRanges(pass1).empty());
}

TEST_CASE("Comping MIDI takes: plain cuts, the content offsets follow", "[takes][session][midi]")
{
    Session session;
    session.setTempo(120.0); // 2 beats a second
    auto t = session.addTrack(TrackKind::instrument, "Keys");
    auto lane1 = session.addTakeLane(t);
    auto first = session.addMidiClip(lane1, 0.0, 4.0, "first");
    for (int beat = 0; beat < 8; ++beat)
        session.addNote(first, beat, 0.5, 60);
    auto lane2 = session.addTakeLane(t);
    auto second = session.addMidiClip(lane2, 0.0, 4.0, "second");
    for (int beat = 0; beat < 8; ++beat)
        session.addNote(second, beat, 0.5, 67);

    session.compTake(lane1, 0.0, 4.0);
    session.compTake(lane2, 1.0, 2.0);
    const auto comp = compOf(t);
    REQUIRE(comp.size() == 3);
    CHECK(clip::getName(comp[0]) == "first");
    CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(1.0, 1e-9));
    CHECK(clip::getName(comp[1]) == "second");
    CHECK_THAT(clip::getStart(comp[1]), WithinAbs(1.0, 1e-9));
    CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(2.0, 1e-9));
    CHECK_THAT(clip::getOffset(comp[1]), WithinAbs(2.0, 1e-9)); // beats
    CHECK(clip::getName(comp[2]) == "first");
    CHECK_THAT(clip::getStart(comp[2]), WithinAbs(2.0, 1e-9));
    CHECK_THAT(clip::getOffset(comp[2]), WithinAbs(4.0, 1e-9));
}

TEST_CASE("Take lanes: kept from the comp, removed when unused, linked through copies and saves", "[takes][session]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    auto t = session.addTrack("Vox");
    auto loose = session.addClip(t, fileA, 0.0, 4.0); // recorded before there were lanes

    // The comp's own material goes into a lane before anything replaces it, once.
    session.beginTransaction("Keep");
    auto kept = session.keepCompInTakeLane(t, 1.0, 2.0);
    REQUIRE(kept.isValid());
    CHECK(takelane::getName(kept) == "Take 1");
    CHECK(session.getTakeLaneOf(loose) == kept);
    REQUIRE(takelane::getClips(kept).size() == 1);
    CHECK(clip::getFile(takelane::getClips(kept)[0]) == fileA);
    CHECK_FALSE(session.keepCompInTakeLane(t, 1.0, 2.0).isValid());

    auto copyLane = session.addTakeLaneFromComp(t);
    CHECK(takelane::getName(copyLane) == "Comp 1");
    CHECK(takelane::getClips(copyLane).size() == 1);
    auto take2 = laneWith(session, t, fileB, 0.0, 4.0);
    CHECK(takelane::getName(take2) == "Take 2");
    session.compTake(take2, 1.0, 2.0);

    // Takes' files count as used (loaded, kept with the project).
    auto onlyInLane = laneWith(session, t, fileC, 5.0, 1.0);
    const auto files = session.getReferencedFiles();
    CHECK(std::find(files.begin(), files.end(), fileC) != files.end());
    session.removeTakeLane(onlyInLane);

    // Copies of the track: lanes, links and the auditioned lane follow the copy.
    track::setAuditionedTake(t, takelane::getId(take2), nullptr);
    auto copy = session.duplicateTrack(t, {}, t);
    REQUIRE(copy.isValid());
    const auto copyLanes = Session::getTakeLanes(copy);
    REQUIRE(copyLanes.size() == 3);
    CHECK(takelane::getId(copyLanes[0]) != takelane::getId(kept));
    for (const auto& c : compOf(copy))
    {
        const auto lane = session.getTakeLaneOf(c);
        REQUIRE(lane.isValid());
        CHECK(lane.getParent() == copy);
    }
    CHECK(track::getAuditionedTake(copy) == takelane::getId(copyLanes[2]));
    Session::DuplicateOptions noClips;
    noClips.clips = false;
    CHECK(Session::getTakeLanes(session.duplicateTrack(t, noClips, copy)).empty());
    track::setAuditionedTake(t, 0, nullptr);

    // Saved and opened again.
    const auto file = temp.dir.getChildFile("takes.dojo");
    REQUIRE(app.project.saveAs(file).wasOk());
    App reopened;
    REQUIRE(reopened.project.open(file).wasOk());
    const auto loaded = reopened.project.getSession().getTrack(0);
    const auto loadedLanes = Session::getTakeLanes(loaded);
    REQUIRE(loadedLanes.size() == 3);
    CHECK(takelane::getName(loadedLanes[2]) == "Take 2");
    CHECK(reopened.project.getSession().getTakeLaneOf(compOf(loaded)[1]) == loadedLanes[2]);
    CHECK(clip::getFile(takelane::getClips(loadedLanes[2])[0]) == fileB);

    // Unused lanes go (Comp 1: nothing in the comp comes from it); removing a lane unlinks its clips.
    session.beginTransaction("Unused");
    CHECK(session.removeUnusedTakeLanes(t) == 1);
    CHECK(Session::getTakeLanes(t).size() == 2);
    session.removeTakeLane(take2);
    for (const auto& c : compOf(t))
        CHECK(session.getTakeLaneOf(c) != take2);
    REQUIRE(session.getUndoManager().undo());
    CHECK(Session::getTakeLanes(t).size() == 3);
}

TEST_CASE("Auditioning a take lane: it plays instead of the comp", "[takes][engine]")
{
    test::TempFolder temp;
    const auto a = test::writeWav(temp.dir.getChildFile("a.wav"), std::vector<float>(static_cast<std::size_t>(2 * kRate), 0.5f), kRate);
    const auto b = test::writeWav(temp.dir.getChildFile("b.wav"), std::vector<float>(static_cast<std::size_t>(2 * kRate), 0.25f), kRate);
    Session session;
    AudioAssetCache assets;
    DeviceManager devices(session, [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); });
    GraphBuilder builder;
    engine::AudioEngine engine(0);
    engine.prepare(kRate, kBlock, 2);
    devices.setPlayHead(engine.getPlayHead());
    devices.prepareAll(kRate, kBlock);
    builder.setRenderFormat(kRate, kBlock);
    juce::String error;
    REQUIRE(assets.loadNow(a, error) != nullptr);
    REQUIRE(assets.loadNow(b, error) != nullptr);

    auto t = session.addTrack("Vocal");
    auto take1 = laneWith(session, t, a, 0.0, 2.0);
    auto take2 = laneWith(session, t, b, 0.0, 2.0);
    session.compTake(take1, 0.0, 2.0);

    // Renders two seconds (the left output).
    int serial = 0;
    const auto render = [&]
    {
        auto graph = builder.build(session, assets, &devices);
        REQUIRE(graph->tracks.size() == 1);
        graph->serial = ++serial;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        std::vector<float> out;
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(2.0 * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            engine.releaseRetiredGraphs();
            out.insert(out.end(), l.begin(), l.end());
        }
        REQUIRE(engine.post(engine::command::SetPlaying{false}));
        engine.process(nullptr, 0, channels, 2, kBlock);
        return out;
    };

    const auto comp = render();
    const double level = at(comp, 0.5);
    REQUIRE(level > 0.1);
    CHECK_THAT(at(comp, 1.5), WithinAbs(level, 1e-4));

    track::setAuditionedTake(t, takelane::getId(take2), nullptr);
    const auto auditioned = render();
    CHECK_THAT(at(auditioned, 0.5), WithinAbs(level * 0.5, 1e-4));
    CHECK_THAT(at(auditioned, 1.5), WithinAbs(level * 0.5, 1e-4));

    // Removing the lane ends the audition.
    session.removeTakeLane(take2);
    CHECK(track::getAuditionedTake(t) == 0);
    CHECK_THAT(at(render(), 1.5), WithinAbs(level, 1e-4));

    // The comp's edit crossfades over 10 ms: no step anywhere in it, centred on the edit.
    auto take3 = laneWith(session, t, b, 0.0, 2.0);
    session.compTake(take3, 1.0, 2.0);
    const auto crossfaded = render();
    double steepest = 0.0;
    for (auto i = static_cast<std::size_t>(0.98 * kRate); i < static_cast<std::size_t>(1.02 * kRate); ++i)
        steepest = std::max(steepest, static_cast<double>(std::abs(crossfaded[i + 1] - crossfaded[i])));
    CHECK(steepest < 0.02 * level);
    CHECK_THAT(at(crossfaded, 0.994), WithinAbs(level, 1e-3));
    CHECK_THAT(at(crossfaded, 1.006), WithinAbs(level * 0.5, 1e-3));
    CHECK(at(crossfaded, 1.0) > level * 0.5 + 0.05); // (the middle of an equal-power crossfade)

    // The same take going on at another gain: an equal-gain crossfade, a smooth ramp from one gain
    // to the other with no dip or swell.
    session.compTake(take1, 0.0, 2.0);
    clip::setGainDb(compOf(t)[0], -6.0206, nullptr); // (half the level)
    session.compTake(take1, 1.0, 2.0);
    const auto ramp = render();
    CHECK_THAT(at(ramp, 0.99), WithinAbs(level * 0.5, 1e-3));
    CHECK_THAT(at(ramp, 1.0), WithinAbs(level * 0.75, 2e-3)); // (half way: the two gains' mean)
    CHECK_THAT(at(ramp, 1.01), WithinAbs(level, 1e-3));
    for (auto i = static_cast<std::size_t>(0.99 * kRate); i < static_cast<std::size_t>(1.01 * kRate); ++i)
        CHECK(ramp[i + 1] >= ramp[i] - 1.0e-6);

    // Without the crossfade, the edit is a plain cut: only the 1 ms fades every clip edge has.
    session.compTake(take1, 0.0, 2.0);
    session.setCompCrossfade(0.0);
    session.compTake(take3, 1.0, 2.0);
    const auto cut = render();
    CHECK_THAT(at(cut, 0.998), WithinAbs(level, 1e-3));
    CHECK_THAT(at(cut, 1.002), WithinAbs(level * 0.5, 1e-3));
    CHECK(std::abs(at(crossfaded, 1.002) - level * 0.5) > 0.02); // (the crossfade was still under way)
}

TEST_CASE("Recording over a take keeps both in take lanes and comps the new one in", "[takes][record]")
{
    test::TempFolder temp;
    App app;
    app.engine.prepare(kRate, kBlock, 2, 2);
    auto& session = app.project.getSession();
    REQUIRE(app.project.saveAs(temp.dir.getChildFile("takes.dojo")).wasOk());

    // The track holds a first take (a tone over 4 s); the second is punched in from 1 to 2 s.
    const auto tone = test::writeWav(temp.dir.getChildFile("first take.wav"), test::sine(440.0, kRate, 4 * 48000, 0.25f), kRate);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(tone, error) != nullptr);
    auto t = session.addTrack("Vocal");
    session.addClip(t, tone, 0.0, 4.0);
    track::setInput(t, 0, true, nullptr);
    track::setArmed(t, true, nullptr);
    session.setLoop(false, 1.0, 2.0);
    session.setPunchInEnabled(true);
    session.setPunchOutEnabled(true);
    app.project.getCompiler().rebuildNow();

    std::vector<float> inL(kBlock, 0.5f), inR(kBlock, 0.5f), outL(kBlock), outR(kBlock);
    const float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    const auto run = [&](double seconds)
    {
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
            app.engine.process(inputs, 2, outputs, 2, kBlock);
    };

    Recorder recorder(app.project, app.controller);
    REQUIRE(recorder.start().isEmpty());
    app.controller.locateSeconds(0.0);
    run(3.0);
    recorder.stop();
    run(0.01); // acknowledges StopCapture
    REQUIRE(test::pumpMessagesUntil([&] { return !recorder.isBusy(); }));

    const auto lanes = Session::getTakeLanes(t);
    REQUIRE(lanes.size() == 2);
    CHECK(takelane::getName(lanes[0]) == "Take 1");
    CHECK(takelane::getName(lanes[1]) == "Take 2");
    const auto first = takelane::getClips(lanes[0]);
    REQUIRE(first.size() == 1);
    CHECK(clip::getFile(first[0]) == tone);
    CHECK_THAT(clip::getEnd(first[0]), WithinAbs(4.0, 1e-9));
    const auto second = takelane::getClips(lanes[1]);
    REQUIRE(second.size() == 1); // the whole recording, though the comp keeps 1 - 2 s of it
    const auto recorded = clip::getFile(second[0]);
    CHECK(recorded != tone);
    CHECK_THAT(clip::getStart(second[0]), WithinAbs(0.0, 1e-9));
    CHECK_THAT(clip::getLength(second[0]), WithinAbs(3.0, 0.01));

    const auto comp = compOf(t);
    REQUIRE(comp.size() == 3);
    CHECK(clip::getFile(comp[0]) == tone);
    CHECK_THAT(clip::getEnd(comp[0]), WithinAbs(1.0 + kHalfFade, 1e-9));
    CHECK(session.getTakeLaneOf(comp[0]) == lanes[0]);
    CHECK(clip::getFile(comp[1]) == recorded);
    CHECK_THAT(clip::getStart(comp[1]), WithinAbs(1.0 - kHalfFade, 1e-9));
    CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(2.0 + kHalfFade, 1e-9));
    CHECK_THAT(clip::getOffset(comp[1]), WithinAbs(1.0 - kHalfFade, 1e-9));
    CHECK(session.getTakeLaneOf(comp[1]) == lanes[1]);
    CHECK(clip::getFile(comp[2]) == tone);
    CHECK_THAT(clip::getStart(comp[2]), WithinAbs(2.0 - kHalfFade, 1e-9));
    CHECK(session.getTakeLaneOf(comp[2]) == lanes[0]);

    // One undo step: the first take alone again, no lanes.
    REQUIRE(session.getUndoManager().undo());
    CHECK(Session::getTakeLanes(t).empty());
    REQUIRE(compOf(t).size() == 1);
    CHECK_THAT(clip::getEnd(compOf(t)[0]), WithinAbs(4.0, 1e-9));
}

TEST_CASE("Loop Record: the transport cycles the loop and each pass is a take in a lane", "[takes][record]")
{
    test::TempFolder temp;
    App app;
    app.engine.prepare(kRate, kBlock, 2, 2);
    auto& session = app.project.getSession();
    REQUIRE(app.project.saveAs(temp.dir.getChildFile("loop.dojo")).wasOk());

    auto guitar = session.addTrack("Guitar");
    track::setInput(guitar, 0, true, nullptr);
    track::setArmed(guitar, true, nullptr);
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    track::setArmed(keys, true, nullptr);
    session.setLoop(true, 1.0, 2.0);
    session.setLoopRecording(true);
    app.project.getCompiler().rebuildNow();

    Recorder recorder(app.project, app.controller);
    REQUIRE(recorder.start().isEmpty());
    app.controller.locateSeconds(0.5);

    // The transport plays 0.5 - 2 s, then 1 - 2 s over and over: the guitar plays louder each pass
    // (0.1, 0.2, 0.3, ...), the keys a different note (60, 62, 64, ...) 0.2 s into the loop.
    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
    const float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    const int blocks = static_cast<int>(3.7 * kRate) / kBlock;
    for (int block = 0; block < blocks; ++block)
    {
        const double elapsed = block * kBlock / kRate;
        const int pass = elapsed < 1.5 ? 0 : 1 + static_cast<int>((elapsed - 1.5) / 1.0);
        std::fill(inL.begin(), inL.end(), 0.1f * static_cast<float>(pass + 1));
        std::fill(inR.begin(), inR.end(), 0.1f * static_cast<float>(pass + 1));
        const double intoPass = pass == 0 ? elapsed - 0.5 : elapsed - 1.5 - (pass - 1);
        const double previous = intoPass - kBlock / kRate;
        if (intoPass >= 0.2 && previous < 0.2)
            (void)app.engine.getLiveMidiQueue().push(juce::MidiMessage::noteOn(1, 60 + 2 * pass, 0.8f));
        if (intoPass >= 0.4 && previous < 0.4)
            (void)app.engine.getLiveMidiQueue().push(juce::MidiMessage::noteOff(1, 60 + 2 * pass));
        app.engine.process(inputs, 2, outputs, 2, kBlock);
    }
    recorder.stop();
    app.engine.process(inputs, 2, outputs, 2, kBlock); // acknowledges StopCapture
    REQUIRE(test::pumpMessagesUntil([&] { return !recorder.isBusy(); }));

    // Three passes (the fourth, stopped 0.2 s in, is dropped), each 1 - 2 s, from one file.
    const auto lanes = Session::getTakeLanes(guitar);
    REQUIRE(lanes.size() == 3);
    const double offsets[] = {0.5, 1.5, 2.5};
    juce::File recorded;
    for (std::size_t k = 0; k < lanes.size(); ++k)
    {
        CHECK(takelane::getName(lanes[k]) == "Take " + juce::String(static_cast<int>(k) + 1));
        const auto takes = takelane::getClips(lanes[k]);
        REQUIRE(takes.size() == 1);
        CHECK_THAT(clip::getStart(takes[0]), WithinAbs(1.0, 1e-9));
        CHECK_THAT(clip::getLength(takes[0]), WithinAbs(1.0, 1e-6));
        CHECK_THAT(clip::getOffset(takes[0]), WithinAbs(offsets[k], 1e-6));
        recorded = clip::getFile(takes[0]);
    }
    const auto audio = decodeLeft(recorded);
    for (std::size_t k = 0; k < 3; ++k)
        CHECK_THAT(at(audio, offsets[k] + 0.5), WithinAbs(0.1 * static_cast<double>(k + 1), 1e-3));

    // The newest pass is the comp.
    const auto comp = compOf(guitar);
    REQUIRE(comp.size() == 1);
    CHECK(session.getTakeLaneOf(comp[0]) == lanes[2]);
    CHECK_THAT(clip::getOffset(comp[0]), WithinAbs(2.5, 1e-6));

    // MIDI: a take per pass too, each with its note 0.2 s (0.4 beats) in.
    const auto midiLanes = Session::getTakeLanes(keys);
    REQUIRE(midiLanes.size() == 3);
    for (std::size_t k = 0; k < midiLanes.size(); ++k)
    {
        const auto takes = takelane::getClips(midiLanes[k]);
        REQUIRE(takes.size() == 1);
        CHECK_THAT(clip::getStart(takes[0]), WithinAbs(1.0, 1e-9));
        const auto n = takes[0].getChildWithName(ids::note);
        REQUIRE(n.isValid());
        CHECK(note::getNote(n) == 60 + 2 * static_cast<int>(k));
        CHECK_THAT(note::getStart(n), WithinAbs(0.4, 0.02));
    }
    const auto midiComp = compOf(keys);
    REQUIRE(midiComp.size() == 1);
    CHECK(note::getNote(midiComp[0].getChildWithName(ids::note)) == 64);
}

TEST_CASE("Take lanes on the arranger: T shows them, a swipe comps, a click comps a take, S auditions", "[takes][ui]")
{
    MessageThread messageThread;
    engine::AudioEngine engine{0};
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    Project project([&controller](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                    [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); });
    ui::EditorState editor;
    auto& session = project.getSession();
    auto t = session.addTrack("Vocal");
    auto other = session.addTrack("Bass");
    session.addClip(other, fileC, 0.0, 10.0);
    auto take1 = laneWith(session, t, fileA, 0.0, 10.0);
    auto take2 = laneWith(session, t, fileB, 0.0, 10.0);
    session.compTake(take1, 0.0, 10.0);

    ui::ArrangerView arranger(project, controller, editor);
    arranger.setSize(1600, 700);

    const auto event = [&arranger](juce::Point<float> p, juce::Point<float> down, int clicks = 1)
    {
        const auto now = juce::Time::getCurrentTime();
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier); // (Alt: no snap)
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &arranger, &arranger, now, down, now,
                                clicks, p != down);
    };
    const auto click = [&](juce::Point<float> p)
    {
        arranger.mouseDown(event(p, p));
        arranger.mouseUp(event(p, p));
    };
    const auto drag = [&](juce::Point<float> from, juce::Point<float> to)
    {
        arranger.mouseDown(event(from, from));
        arranger.mouseDrag(event((from + to) * 0.5f, from));
        arranger.mouseDrag(event(to, from));
        arranger.mouseUp(event(to, from));
    };

    // The T button: only on a track with take lanes; it shows them.
    CHECK(arranger.getTakesButtonArea(other).isEmpty());
    const auto tButton = arranger.getTakesButtonArea(t);
    REQUIRE_FALSE(tButton.isEmpty());
    CHECK(arranger.getTakeLaneArea(take2).isEmpty());
    click(tButton.getCentre().toFloat());
    CHECK(track::isShowingTakes(t));
    const auto lane2 = arranger.getTakeLaneArea(take2);
    REQUIRE_FALSE(lane2.isEmpty());
    CHECK(arranger.getTakeLaneArea(take1).getBottom() <= lane2.getY());
    CHECK(arranger.getTrackHeaderArea(other).getY() >= lane2.getBottom()); // (the next track moved down)

    // Times along the lanes: from the comp clip that runs 0 - 10 s.
    const auto clipArea = arranger.getClipArea(compOf(t)[0]).toFloat();
    const auto x = [&clipArea](double seconds) { return clipArea.getX() + static_cast<float>(seconds / 10.0) * clipArea.getWidth(); };
    const auto y = static_cast<float>(lane2.getCentreY());

    // Swipe across take 2 from 4 to 6 s.
    drag({x(4.0), y}, {x(6.0), y});
    auto comp = compOf(t);
    REQUIRE(comp.size() == 3);
    CHECK(session.getTakeLaneOf(comp[1]) == take2);
    CHECK_THAT(clip::getStart(comp[1]), WithinAbs(4.0 - kHalfFade, 0.03));
    CHECK_THAT(clip::getEnd(comp[1]), WithinAbs(6.0 + kHalfFade, 0.03));
    REQUIRE(session.getUndoManager().undo()); // (one step)
    CHECK(compOf(t).size() == 1);

    // A click on take 2: all of it.
    click({x(3.0), y});
    comp = compOf(t);
    REQUIRE(comp.size() == 1);
    CHECK(session.getTakeLaneOf(comp[0]) == take2);

    // S on take 1's header: it plays instead of the comp; again: the comp plays.
    const auto sButton = arranger.getTakeAuditionButtonArea(take1);
    REQUIRE_FALSE(sButton.isEmpty());
    click(sButton.getCentre().toFloat());
    CHECK(track::getAuditionedTake(t) == takelane::getId(take1));
    click(sButton.getCentre().toFloat());
    CHECK(track::getAuditionedTake(t) == 0);

    // Track > Take Lanes: hidden again, a lane from the comp, unused lanes deleted.
    editor.selectTrack(track::getId(t));
    CHECK(ui::actions::selectedTracksShowTakes(session, editor));
    CHECK(ui::actions::toggleSelectedTracksTakeLanes(session, editor));
    CHECK_FALSE(track::isShowingTakes(t));
    CHECK(ui::actions::newTakeLanesFromComp(session, editor) == 1);
    CHECK(Session::getTakeLanes(t).size() == 3);
    CHECK(track::isShowingTakes(t));
    CHECK(ui::actions::deleteUnusedTakeLanes(session, editor) == 2); // take 1 and the comp's copy
    CHECK(Session::getTakeLanes(t).size() == 1);
    CHECK(ui::actions::deleteAllTakeLanes(session, editor) == 1);
    CHECK(Session::getTakeLanes(t).empty());
    CHECK(compOf(t).size() == 1); // (the comp stays)
    CHECK(arranger.getTakesButtonArea(t).isEmpty());
}

#include "devices/DeviceRegistry.h"
#include "devices/Instruments.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Project.h"
#include "TestAudio.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"
#include "ui/components/ClipListPanel.h"
#include "ui/components/TrackDialogs.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using session::Session;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace clip = session::clip;
namespace track = session::track;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

std::set<std::uint32_t> idsOf(std::initializer_list<juce::ValueTree> clips)
{
    std::set<std::uint32_t> ids;
    for (const auto& c : clips)
        ids.insert(clip::getId(c));
    return ids;
}

std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kRate); }

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    auto processor = devices::createBuiltinDevice(session::device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

/** A session wired to a real engine, driven by the test as the audio device. */
struct PlayRig
{
    PlayRig()
        : engine(1)
        , devices(session, createDevice)
    {
        engine.prepare(kRate, kBlock, 2);
        devices.setPlayHead(engine.getPlayHead());
        devices.prepareAll(kRate, kBlock);
        builder.setRenderFormat(kRate, kBlock);
    }

    void play(double seconds)
    {
        left.clear();
        auto graph = builder.build(session, assets, &devices);
        graph->serial = ++serial;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            left.insert(left.end(), l.begin(), l.end());
            engine.releaseRetiredGraphs();
        }
        REQUIRE(engine.post(engine::command::SetPlaying{false}));
    }

    juce::ValueTree audioClip(const juce::File& file, double start = 0.0)
    {
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = session.addTrack();
        return session.addClip(t, file, start, asset->getLengthSeconds());
    }

    engine::AudioEngine engine;
    Session session;
    session::AudioAssetCache assets;
    session::DeviceManager devices;
    session::GraphBuilder builder;
    std::uint64_t serial = 0;
    std::vector<float> left;
};

/** A project with an arranger (for clicks, drags and drops). */
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
        a = session.addTrack(session::TrackKind::audio, "A");
        b = session.addTrack(session::TrackKind::audio, "B");
        clipA = session.addClip(a, juce::File("/audio/a.wav"), 2.0, 4.0);
        clipB = session.addClip(b, juce::File("/audio/b.wav"), 8.0, 4.0);
        arranger = std::make_unique<ui::ArrangerView>(project, controller, editor);
        arranger->setSize(1400, 600);
    }

    juce::MouseEvent event(juce::Point<float> at, juce::ModifierKeys mods, juce::Point<float> downAt) const
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, arranger.get(),
                                arranger.get(), now, downAt, now, 1, at != downAt);
    }

    void drag(juce::Point<float> from, juce::Point<float> by, bool snap)
    {
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | (snap ? 0 : juce::ModifierKeys::altModifier));
        arranger->mouseDown(event(from, mods, from));
        arranger->mouseDrag(event(from + by * 0.5f, mods, from));
        arranger->mouseDrag(event(from + by, mods, from));
        arranger->mouseUp(event(from + by, mods, from));
    }

    void click(juce::Point<float> at)
    {
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        arranger->mouseDown(event(at, mods, at));
        arranger->mouseUp(event(at, mods, at));
    }

    [[nodiscard]] float pixelsPerSecond() const { return static_cast<float>(arranger->getClipArea(clipA).getWidth()) / 4.0f; }
    /** x of timeline time `seconds` (from clip A, which starts at 2 s). */
    [[nodiscard]] float xOf(double seconds) const
    {
        return static_cast<float>(arranger->getClipArea(clipA).getX()) + static_cast<float>(seconds - 2.0) * pixelsPerSecond();
    }
    [[nodiscard]] float yOf(const juce::ValueTree& c) const { return arranger->getClipArea(c).toFloat().getCentreY() + 8.0f; }
    [[nodiscard]] juce::Point<float> body(const juce::ValueTree& c) const { return arranger->getClipArea(c).toFloat().getCentre().translated(0.0f, 8.0f); }
    [[nodiscard]] juce::Point<float> rightEdge(const juce::ValueTree& c) const
    {
        const auto area = arranger->getClipArea(c).toFloat();
        return {area.getRight() - 2.0f, area.getBottom() - 6.0f};
    }
    [[nodiscard]] juce::Point<float> leftEdge(const juce::ValueTree& c) const
    {
        const auto area = arranger->getClipArea(c).toFloat();
        return {area.getX() + 2.0f, area.getBottom() - 6.0f};
    }

    engine::AudioEngine engine{0};
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    session::Project project;
    ui::EditorState editor;
    std::unique_ptr<ui::ArrangerView> arranger;
    juce::ValueTree a, b, clipA, clipB;
};
} // namespace

// --- Rating -----------------------------------------------------------------------------------------

TEST_CASE("Clip > Rating rates the selected clips 1 to 5 or none, with undo", "[session][clip][rating]")
{
    Session session;
    ui::EditorState editor;
    auto t = session.addTrack();
    auto c1 = session.addClip(t, juce::File("/a.wav"), 0.0, 1.0);
    auto c2 = session.addClip(t, juce::File("/b.wav"), 2.0, 1.0);

    CHECK(clip::getRating(c1) == 0); // none by default
    editor.setSelectedClips(idsOf({c1, c2}));
    CHECK(ui::actions::getSelectedClipsRating(session, editor) == 0);
    REQUIRE(ui::actions::setSelectedClipsRating(session, editor, 4));
    CHECK(clip::getRating(c1) == 4);
    CHECK(clip::getRating(c2) == 4);
    CHECK(ui::actions::getSelectedClipsRating(session, editor) == 4);

    editor.setSelectedClips(idsOf({c2}));
    REQUIRE(ui::actions::setSelectedClipsRating(session, editor, 1));
    editor.setSelectedClips(idsOf({c1, c2}));
    CHECK(ui::actions::getSelectedClipsRating(session, editor) == -1); // mixed

    clip::setRating(c1, 9, nullptr); // out of range: clamped
    CHECK(clip::getRating(c1) == 5);

    session.getUndoManager().undo();
    CHECK(clip::getRating(c2) == 4);
    editor.setSelectedClips(idsOf({c1}));
    REQUIRE(ui::actions::setSelectedClipsRating(session, editor, 0));
    CHECK(clip::getRating(c1) == 0);
    CHECK_FALSE(c1.hasProperty(session::ids::rating));

    // The display toggle.
    CHECK(editor.isShowingClipRatings());
    editor.setShowingClipRatings(false);
    CHECK_FALSE(editor.isShowingClipRatings());
}

// --- Groups -----------------------------------------------------------------------------------------

TEST_CASE("Clip groups nest; Ungroup takes the top layer, Regroup puts it back, Ungroup All everything", "[session][clip][groups]")
{
    Session session;
    ui::EditorState editor;
    auto audio = session.addTrack(session::TrackKind::audio, "Audio");
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto c1 = session.addClip(audio, juce::File("/a.wav"), 0.0, 1.0);
    auto c2 = session.addClip(audio, juce::File("/b.wav"), 2.0, 1.0);
    auto m = session.addMidiClip(keys, 1.0, 2.0);
    auto loose = session.addClip(audio, juce::File("/c.wav"), 6.0, 1.0);

    // Audio and MIDI clips on two tracks in one group.
    editor.setSelectedClips(idsOf({c1, m}));
    const auto inner = ui::actions::groupSelectedClips(session, editor);
    REQUIRE(inner != 0);
    CHECK(session.getGroupName(inner) == "Clip Group 1");
    CHECK(clip::getOuterGroup(c1) == inner);
    CHECK(clip::getOuterGroup(m) == inner);
    CHECK(session.getGroupMembers(inner).size() == 2);

    // Selecting a member selects the whole group.
    CHECK(session.withWholeGroups({c1}).size() == 2);

    // The group and another clip in a new group: nested.
    editor.setSelectedClips(idsOf({c1, c2}));
    const auto outer = ui::actions::groupSelectedClips(session, editor);
    REQUIRE(outer != 0);
    CHECK(session.getGroupName(outer) == "Clip Group 2");
    CHECK(clip::getGroups(c1) == std::vector<std::uint32_t>{outer, inner});
    CHECK(clip::getGroups(m) == std::vector<std::uint32_t>{outer, inner});
    CHECK(clip::getGroups(c2) == std::vector<std::uint32_t>{outer});
    CHECK_FALSE(clip::isGrouped(loose));
    CHECK(editor.getSelectedClips() == idsOf({c1, c2, m}));

    // Ungroup: only the top layer; the inner group stays.
    editor.setSelectedClips(idsOf({c2}));
    CHECK(ui::actions::selectionHasGroups(session, editor));
    REQUIRE(ui::actions::ungroupSelectedClips(session, editor));
    CHECK(clip::getGroups(c1) == std::vector<std::uint32_t>{inner});
    CHECK_FALSE(clip::isGrouped(c2));
    CHECK(session.getGroupMembers(outer).empty());

    // Regroup: as it was.
    REQUIRE_FALSE(editor.getLastUngroup().empty());
    REQUIRE(ui::actions::regroupClips(session, editor));
    CHECK(clip::getGroups(c1) == std::vector<std::uint32_t>{outer, inner});
    CHECK(clip::getGroups(c2) == std::vector<std::uint32_t>{outer});
    CHECK(session.getGroupName(outer) == "Clip Group 2");
    CHECK(editor.getLastUngroup().empty());

    // Ungroup All: every layer at once.
    editor.setSelectedClips(idsOf({m}));
    REQUIRE(ui::actions::ungroupAllSelectedClips(session, editor));
    for (const auto& c : {c1, c2, m})
        CHECK_FALSE(clip::isGrouped(c));
    CHECK(session.getState().getChildWithName(session::ids::clipGroupList).getNumChildren() == 0);

    // All of it undoes.
    session.getUndoManager().undo();
    CHECK(clip::getGroups(c1) == std::vector<std::uint32_t>{outer, inner});
}

TEST_CASE("Copies of grouped clips form groups of their own", "[session][clip][groups]")
{
    Session session;
    ui::EditorState editor;
    auto t = session.addTrack();
    auto c1 = session.addClip(t, juce::File("/a.wav"), 0.0, 1.0);
    auto c2 = session.addClip(t, juce::File("/b.wav"), 1.0, 1.0);
    editor.setSelectedClips(idsOf({c1, c2}));
    const auto group = ui::actions::groupSelectedClips(session, editor);

    editor.setSelectedClips(idsOf({c1})); // (the group is duplicated whole)
    ui::actions::duplicateSelectedClips(session, editor);
    std::vector<juce::ValueTree> copies;
    for (const auto& c : t)
        if (Session::isClip(c) && c != c1 && c != c2)
            copies.push_back(c);
    REQUIRE(copies.size() == 2);
    const auto copyGroup = clip::getOuterGroup(copies[0]);
    CHECK(copyGroup != 0);
    CHECK(copyGroup != group);
    CHECK(clip::getOuterGroup(copies[1]) == copyGroup);
    CHECK(session.getGroupMembers(group).size() == 2);
}

// --- Loop / Unloop ----------------------------------------------------------------------------------

TEST_CASE("Clip > Loop repeats a clip; Unloop makes one clip per iteration", "[session][clip][loop]")
{
    Session session;
    ui::EditorState editor;
    auto t = session.addTrack();
    auto c = session.addClip(t, juce::File("/a.wav"), 1.0, 2.0, 0.5);
    clip::setFadeIn(c, 0.1, nullptr);
    clip::setFadeOut(c, 0.2, nullptr);
    session.identifySyncPoint(c, 1.5);

    editor.setSelectedClips(idsOf({c}));
    REQUIRE(ui::actions::loopSelectedClips(session, editor, 3.5));
    CHECK(clip::isLooped(c));
    CHECK(clip::getLoopLength(c) == 2.0);
    CHECK(clip::getLength(c) == 7.0);
    CHECK(ui::actions::selectionHasLoopedClips(session, editor));

    // A looped clip is not split (unloop it first).
    CHECK_FALSE(session.splitClip(c, 4.0).isValid());

    REQUIRE(ui::actions::unloopSelectedClips(session, editor));
    std::vector<juce::ValueTree> pieces;
    for (const auto& p : t)
        pieces.push_back(p);
    REQUIRE(pieces.size() == 4);
    for (std::size_t k = 0; k < pieces.size(); ++k)
    {
        CHECK_FALSE(clip::isLooped(pieces[k]));
        CHECK_THAT(clip::getStart(pieces[k]), WithinAbs(1.0 + 2.0 * static_cast<double>(k), 1e-9));
        CHECK(clip::getOffset(pieces[k]) == 0.5);
        CHECK(clip::getFile(pieces[k]) == juce::File("/a.wav"));
    }
    CHECK_THAT(clip::getLength(pieces[3]), WithinAbs(1.0, 1e-9)); // the last, partial iteration
    CHECK(clip::getFadeIn(pieces[0]) == 0.1);
    CHECK(clip::getFadeOut(pieces[0]) == 0.0);
    CHECK(clip::getFadeOut(pieces[3]) == 0.2);
    CHECK(clip::getSyncPoint(pieces[0]).has_value());
    CHECK_FALSE(clip::getSyncPoint(pieces[1]).has_value());
    CHECK(editor.getSelectedClips().size() == 4);

    session.getUndoManager().undo();
    CHECK(t.getNumChildren() == 1);
    CHECK(clip::isLooped(c));
}

TEST_CASE("Looped clips play their loop again and again: audio, MIDI and video", "[engine][clip][loop]")
{
    SECTION("audio")
    {
        test::TempFolder temp;
        // 0.25 s: a 50 ms burst, then silence.
        auto samples = test::sine(440.0, kRate, static_cast<int>(0.25 * kRate), 0.5f);
        std::fill(samples.begin() + static_cast<std::ptrdiff_t>(at(0.05)), samples.end(), 0.0f);
        const auto file = test::writeWav(temp.dir.getChildFile("burst.wav"), samples, kRate);

        PlayRig rig;
        auto c = rig.audioClip(file);
        rig.session.loopClip(c, 3.0); // 0 .. 0.75 s
        rig.play(1.0);
        for (const double iteration : {0.0, 0.25, 0.5})
        {
            CHECK_THAT(test::peak(rig.left, at(iteration + 0.01), at(iteration + 0.04)), WithinRel(0.5, 0.02));
            CHECK(test::peak(rig.left, at(iteration + 0.07), at(iteration + 0.23)) < 1e-6);
        }
        CHECK(test::peak(rig.left, at(0.76), at(1.0)) < 1e-6); // ends after three
    }

    SECTION("MIDI")
    {
        test::TempFolder temp;
        const auto file = test::writeWav(temp.dir.getChildFile("a440.wav"), test::sine(440.0, kRate, 96000, 0.5f), kRate);
        PlayRig rig;
        auto keys = rig.session.addTrack(session::TrackKind::instrument, "Keys");
        auto d = rig.session.addDevice(keys, "builtin:sampler", "Sampler");
        auto node = rig.devices.getNode(d);
        REQUIRE(node != nullptr);
        auto& sampler = dynamic_cast<devices::Sampler&>(node->getProcessor());
        juce::String error;
        REQUIRE(sampler.loadSample(file, error));
        sampler.root_->setValueNotifyingHost(sampler.root_->convertTo0to1(69.0f));

        auto c = rig.session.addMidiClip(keys, 0.0, 1.0); // two beats at 120 bpm
        rig.session.addNote(c, 0.0, 0.5, 69);           // 0 .. 0.25 s
        rig.session.loopClip(c, 3.0);                   // 0 .. 3 s
        rig.play(3.5);
        for (const double iteration : {0.0, 1.0, 2.0})
        {
            CHECK(test::peak(rig.left, at(iteration + 0.05), at(iteration + 0.2)) > 0.3);
            CHECK(test::peak(rig.left, at(iteration + 0.5), at(iteration + 0.95)) < 1e-4);
        }
        CHECK(test::peak(rig.left, at(3.05), at(3.4)) < 1e-4);
    }

    SECTION("video")
    {
        Session session;
        auto v = session.addTrack(session::TrackKind::video, "Video");
        auto c = session.addVideoClip(v, juce::File("/movie.avi"), 10.0, 2.0, 1.0);
        session.loopClip(c, 3.0); // 10 .. 16 s, the file's 1 .. 3 s three times
        const auto found = ui::actions::findVideoAt(session, 14.5);
        REQUIRE(found.clip == c);
        CHECK_THAT(found.fileSeconds, WithinAbs(1.5, 1e-9));
    }
}

// --- Sync points and Quantize to Grid -----------------------------------------------------------------

TEST_CASE("Sync points mark a moment in a clip; Quantize to Grid lines it up", "[session][clip][sync]")
{
    Session session;
    ui::EditorState editor;
    auto t = session.addTrack();
    auto c = session.addClip(t, juce::File("/a.wav"), 1.1, 4.0);
    auto other = session.addClip(t, juce::File("/b.wav"), 7.3, 1.0);
    editor.setSelectedClips(idsOf({c, other}));

    // Only clips under the playhead get one.
    REQUIRE(ui::actions::identifySyncPoint(session, editor, 2.35));
    REQUIRE(clip::getSyncPoint(c).has_value());
    CHECK_THAT(*clip::getSyncPoint(c), WithinAbs(1.25, 1e-9));
    CHECK_THAT(clip::getReferenceTime(c), WithinAbs(2.35, 1e-9));
    CHECK_FALSE(clip::getSyncPoint(other).has_value());
    CHECK_FALSE(ui::actions::identifySyncPoint(session, editor, 20.0));

    // Quantize: the sync point to the nearest half second (beats at 120 bpm); a clip without one by its start.
    REQUIRE(ui::actions::quantizeSelectedClipsToGrid(session, editor, 0.5));
    CHECK_THAT(clip::getReferenceTime(c), WithinAbs(2.5, 1e-9));
    CHECK_THAT(clip::getStart(c), WithinAbs(1.25, 1e-9));
    CHECK_THAT(clip::getStart(other), WithinAbs(7.5, 1e-9));

    // A time-locked clip stays.
    clip::setTimeLocked(other, true, nullptr);
    clip::setStart(other, 7.3, nullptr);
    ui::actions::quantizeSelectedClipsToGrid(session, editor, 0.5);
    CHECK(clip::getStart(other) == 7.3);

    // A split keeps the sync point with its material.
    auto right = session.splitClip(c, 2.0);
    REQUIRE(right.isValid());
    CHECK_FALSE(clip::getSyncPoint(c).has_value());
    REQUIRE(clip::getSyncPoint(right).has_value());
    CHECK_THAT(clip::getReferenceTime(right), WithinAbs(2.5, 1e-9));

    editor.setSelectedClips(idsOf({right}));
    CHECK(ui::actions::selectionHasSyncPoints(session, editor));
    REQUIRE(ui::actions::removeSyncPoints(session, editor));
    CHECK_FALSE(clip::getSyncPoint(right).has_value());
    CHECK_FALSE(ui::actions::selectionHasSyncPoints(session, editor));
}

// --- Clip gain ----------------------------------------------------------------------------------------

TEST_CASE("Bypass Clip Gain plays the clip at 0 dB and keeps the setting", "[engine][clip][gain]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 48000, 0.25f), kRate);
    PlayRig rig;
    auto c = rig.audioClip(file);
    clip::setGainDb(c, -12.0, nullptr);

    rig.play(0.5);
    CHECK_THAT(test::peak(rig.left, at(0.1), at(0.4)), WithinRel(0.25 * std::pow(10.0, -12.0 / 20.0), 0.01));

    ui::EditorState editor;
    editor.setSelectedClips(idsOf({c}));
    REQUIRE(ui::actions::toggleSelectedClipsGainBypass(rig.session, editor));
    CHECK(ui::actions::selectedClipsGainBypassed(rig.session, editor));
    CHECK(clip::getGainDb(c) == -12.0); // kept
    rig.play(0.5);
    CHECK_THAT(test::peak(rig.left, at(0.1), at(0.4)), WithinRel(0.25, 0.01));

    REQUIRE(ui::actions::toggleSelectedClipsGainBypass(rig.session, editor));
    CHECK_FALSE(clip::isGainBypassed(c));
}

TEST_CASE("Render Clip Gain writes the gain into new audio and resets it to 0 dB", "[ui][clip][gain]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 48000, 0.25f), kRate);
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {}, createDevice);
    REQUIRE(project.saveAs(temp.dir.getChildFile("Song.dojo")).wasOk());
    auto& session = project.getSession();
    auto t = session.addTrack();
    auto c = session.addClip(t, file, 1.0, 0.5, 0.25); // the file's 0.25 .. 0.75 s
    clip::setGainDb(c, 6.0, nullptr);

    ui::EditorState editor;
    juce::String error;
    CHECK(ui::actions::renderSelectedClipsGain(project, editor, error) == 0); // nothing selected
    CHECK(error.isNotEmpty());

    editor.setSelectedClips(idsOf({c}));
    REQUIRE(ui::actions::renderSelectedClipsGain(project, editor, error) == 1);
    CHECK(clip::getGainDb(c) == 0.0);
    CHECK(clip::getOffset(c) == 0.0);
    const auto rendered = clip::getFile(c);
    CHECK(rendered != file);
    CHECK(rendered.getParentDirectory() == temp.dir.getChildFile("Song Rendered"));

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(rendered));
    REQUIRE(reader != nullptr);
    juce::AudioBuffer<float> audio(1, static_cast<int>(reader->lengthInSamples));
    reader->read(&audio, 0, audio.getNumSamples(), 0, true, false);
    CHECK(audio.getNumSamples() >= static_cast<int>(0.5 * kRate));
    const auto original = test::sine(220.0, kRate, 48000, 0.25f);
    const float gain = static_cast<float>(std::pow(10.0, 6.0 / 20.0));
    for (const int i : {0, 1000, 12345, 23999})
        CHECK_THAT(audio.getSample(0, i), WithinAbs(original[static_cast<std::size_t>(i) + 12000] * gain, 1e-5));

    session.getUndoManager().undo();
    CHECK(clip::getFile(c) == file);
    CHECK(clip::getGainDb(c) == 6.0);
}

// --- Capture, Rename and the Clip List -------------------------------------------------------------

TEST_CASE("Capture makes a Clip List entry of a range: one clip, or contiguous material of one file", "[session][clip][capture]")
{
    Session session;
    auto t = session.addTrack(session::TrackKind::audio, "Gtr");
    auto c = session.addClip(t, juce::File("/take.wav"), 2.0, 6.0, 1.0); // the file's 1 .. 7 s at 2 .. 8 s
    clip::setGainDb(c, -3.0, nullptr);
    juce::String error;

    // Part of one clip.
    session.beginTransaction("Capture");
    auto entry = session.captureClip(t, 3.0, 4.5, "Riff", error);
    REQUIRE(entry.isValid());
    CHECK(session.getClipListEntries().size() == 1);
    CHECK(clip::getName(entry) == "Riff");
    CHECK(clip::getStart(entry) == 0.0);
    CHECK_THAT(clip::getLength(entry), WithinAbs(1.5, 1e-9));
    CHECK_THAT(clip::getOffset(entry), WithinAbs(2.0, 1e-9));
    CHECK(clip::getGainDb(entry) == -3.0);
    CHECK(clip::isUserNamed(entry));
    CHECK(clip::getId(entry) != clip::getId(c));
    CHECK(session.findClipListEntry(clip::getId(entry)) == entry);
    CHECK_FALSE(session.findClip(clip::getId(entry)).isValid()); // not on a track

    // Across a split: still one stretch of the file.
    auto right = session.splitClip(c, 5.0);
    REQUIRE(right.isValid());
    auto across = session.captureClip(t, 4.0, 6.0, "Across", error);
    REQUIRE(across.isValid());
    CHECK_THAT(clip::getOffset(across), WithinAbs(3.0, 1e-9));
    CHECK_THAT(clip::getLength(across), WithinAbs(2.0, 1e-9));

    // Moved apart (a gap), or another file: no.
    clip::setStart(right, 5.5, nullptr);
    CHECK_FALSE(session.captureClip(t, 4.0, 6.0, "Gap", error).isValid());
    CHECK(error.contains("contiguous"));
    clip::setStart(right, 5.0, nullptr);
    right.setProperty(session::ids::file, "/other.wav", nullptr);
    CHECK_FALSE(session.captureClip(t, 4.0, 6.0, "Other", error).isValid());

    // Nothing there.
    CHECK_FALSE(session.captureClip(t, 20.0, 21.0, "Empty", error).isValid());

    // A looped clip: within one iteration only.
    auto loopTrack = session.addTrack();
    auto looped = session.addClip(loopTrack, juce::File("/loop.wav"), 0.0, 1.0);
    session.loopClip(looped, 4.0);
    auto second = session.captureClip(loopTrack, 1.25, 1.75, "Second", error);
    REQUIRE(second.isValid());
    CHECK_THAT(clip::getOffset(second), WithinAbs(0.25, 1e-9));
    CHECK_FALSE(clip::isLooped(second));
    CHECK_FALSE(session.captureClip(loopTrack, 1.5, 2.5, "Over", error).isValid());

    // MIDI: one clip at a time; the content offset is in beats.
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto m1 = session.addMidiClip(keys, 0.0, 2.0);
    session.addMidiClip(keys, 2.0, 2.0);
    auto midiEntry = session.captureClip(keys, 0.5, 1.5, "", error);
    REQUIRE(midiEntry.isValid());
    CHECK(clip::getName(midiEntry) == clip::getName(m1));
    CHECK_THAT(clip::getOffset(midiEntry), WithinAbs(1.0, 1e-9)); // 0.5 s = 1 beat at 120 bpm
    CHECK_FALSE(session.captureClip(keys, 1.0, 3.0, "Both", error).isValid());

    // Undo takes the entries away again.
    session.getUndoManager().undo();
    CHECK(session.getClipListEntries().empty());
}

TEST_CASE("Capture uses the time selection or the selected clips; the entry is placed by dragging", "[ui][clip][capture]")
{
    Session session;
    ui::EditorState editor;
    auto a = session.addTrack(session::TrackKind::audio, "A");
    auto b = session.addTrack(session::TrackKind::audio, "B");
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto c = session.addClip(a, juce::File("/a.wav"), 0.0, 4.0);
    session.addClip(b, juce::File("/b.wav"), 0.0, 4.0);
    juce::String error;

    // Nothing selected.
    CHECK_FALSE(ui::actions::getCaptureRange(session, editor, error).track.isValid());
    CHECK(error.isNotEmpty());

    // Selected clips on two tracks: no.
    editor.setSelectedClips({clip::getId(c), clip::getId(c) + 1});
    CHECK_FALSE(ui::actions::getCaptureRange(session, editor, error).track.isValid());

    // A time selection over both tracks: no; over one: yes.
    editor.clearClipSelection();
    editor.setTimeSelection({1.0, 2.0, {track::getId(a), track::getId(b)}});
    CHECK_FALSE(ui::actions::getCaptureRange(session, editor, error).track.isValid());
    editor.setTimeSelection({1.0, 2.0, {track::getId(a), track::getId(keys)}}); // (Keys has no clip there)
    const auto range = ui::actions::getCaptureRange(session, editor, error);
    REQUIRE(range.track == a);
    CHECK(ui::actions::suggestCaptureName(session, range) == clip::getName(c) + " Capture");

    auto entry = ui::actions::captureSelection(session, editor, "", error);
    REQUIRE(entry.isValid());
    CHECK(clip::getName(entry) == clip::getName(c) + " Capture");
    CHECK(ui::actions::suggestCaptureName(session, range) == clip::getName(c) + " Capture 2");

    // The Clip List: captured first; auto-created clips only when asked for.
    auto items = ui::actions::getClipListItems(session, false);
    REQUIRE(items.size() == 1);
    CHECK(items[0].captured);
    CHECK(ui::actions::getClipListItems(session, true).size() == 3);

    // Placing it: on an audio track at the time given, selected.
    auto placed = ui::actions::placeClipListItem(session, editor, entry, b, 10.0);
    REQUIRE(placed.isValid());
    CHECK(placed.getParent() == b);
    CHECK(clip::getStart(placed) == 10.0);
    CHECK(clip::getOffset(placed) == 1.0);
    CHECK(clip::getLength(placed) == 1.0);
    CHECK(editor.getSelectedClips() == std::set<std::uint32_t>{clip::getId(placed)});

    // On a track that cannot hold it: a new audio track.
    const int before = session.getNumTracks();
    auto elsewhere = ui::actions::placeClipListItem(session, editor, entry, keys, 3.0);
    REQUIRE(elsewhere.isValid());
    CHECK(session.getNumTracks() == before + 1);
    CHECK(track::getKind(elsewhere.getParent()) == session::TrackKind::audio);
    session.getUndoManager().undo(); // one step
    CHECK(session.getNumTracks() == before);
}

TEST_CASE("Rename makes a clip user-defined: it is in the Clip List even with auto-created clips hidden", "[ui][clip][rename]")
{
    Session session;
    auto t = session.addTrack();
    auto c = session.addClip(t, juce::File("/drums.wav"), 0.0, 4.0);
    auto right = session.splitClip(c, 2.0); // auto-created
    REQUIRE(right.isValid());
    CHECK(ui::actions::getClipListItems(session, false).empty());
    CHECK(ui::actions::getClipListItems(session, true).size() == 2);

    CHECK_FALSE(ui::actions::renameClip(session, right, "   "));
    REQUIRE(ui::actions::renameClip(session, right, "Fill"));
    CHECK(clip::getName(right) == "Fill");
    CHECK(clip::isUserNamed(right));
    const auto items = ui::actions::getClipListItems(session, false);
    REQUIRE(items.size() == 1);
    CHECK(items[0].clip == right);
    CHECK(items[0].userDefined);
    CHECK_FALSE(items[0].captured);

    session.getUndoManager().undo();
    CHECK_FALSE(clip::isUserNamed(right));
}

TEST_CASE("The Clip List and the clip menu's properties survive saving", "[session][clip][capture]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("take.wav"), test::sine(220.0, kRate, 48000), kRate);
    Session session;
    auto t = session.addTrack();
    session.addClip(t, file, 0.0, 1.0);
    juce::String error;
    REQUIRE(session.captureClip(t, 0.25, 0.75, "Middle", error).isValid());
    auto c = t.getChild(0);
    clip::setRating(c, 3, nullptr);
    clip::setLoop(c, true, 0.5, nullptr);
    clip::setSyncPoint(c, 0.2, nullptr);
    clip::setGainBypassed(c, true, nullptr);
    session.groupClips({c}, "Takes");

    const auto saved = temp.dir.getChildFile("Song.dojo");
    REQUIRE(session.save(saved).wasOk());
    // (the media path is stored relative to the session too)
    REQUIRE(saved.loadFileAsString().contains("fileRelative=\"take.wav\""));

    Session loaded;
    REQUIRE(loaded.load(saved).wasOk());
    const auto entries = loaded.getClipListEntries();
    REQUIRE(entries.size() == 1);
    CHECK(clip::getName(entries[0]) == "Middle");
    CHECK(clip::getFile(entries[0]) == file);
    const auto lc = loaded.getAllTracks()[0].getChild(0);
    CHECK(clip::getRating(lc) == 3);
    CHECK(clip::isLooped(lc));
    CHECK(clip::getLoopLength(lc) == 0.5);
    CHECK(clip::getSyncPoint(lc) == std::optional<double>(0.2));
    CHECK(clip::isGainBypassed(lc));
    REQUIRE(clip::isGrouped(lc));
    CHECK(loaded.getGroupName(clip::getOuterGroup(lc)) == "Takes");
}

// --- The arranger --------------------------------------------------------------------------------------

TEST_CASE("The arranger selects clip groups as a whole and does not trim inside them", "[ui][clip][groups]")
{
    ArrangerRig rig;
    auto& session = rig.project.getSession();
    session.groupClips({rig.clipA, rig.clipB});

    rig.click(rig.body(rig.clipA));
    CHECK(rig.editor.getSelectedClips() == idsOf({rig.clipA, rig.clipB}));

    // An edge drag moves the group instead of trimming one of its clips.
    const float oneSecond = rig.pixelsPerSecond();
    rig.drag(rig.rightEdge(rig.clipA), {oneSecond, 0.0f}, false);
    CHECK(clip::getLength(rig.clipA) == 4.0);
    CHECK_THAT(clip::getStart(rig.clipA), WithinAbs(3.0, 0.02));
    CHECK_THAT(clip::getStart(rig.clipB), WithinAbs(9.0, 0.02)); // the whole group moved

    // A marquee touching one member selects the group.
    rig.editor.clearClipSelection();
    const auto from = juce::Point<float>(rig.xOf(1.0), rig.yOf(rig.clipA) - 4.0f);
    rig.drag(from, {rig.pixelsPerSecond() * 3.0f, 2.0f}, false);
    CHECK(rig.editor.getSelectedClips() == idsOf({rig.clipA, rig.clipB}));
}

TEST_CASE("The arranger lengthens looped clips at the end only and snaps moves by the sync point", "[ui][clip][loop][sync]")
{
    ArrangerRig rig;
    const float oneSecond = rig.pixelsPerSecond();
    REQUIRE(oneSecond > 10.0f);

    // Sync point 0.3 s in: a move snaps it (not the start) to the half-second grid.
    clip::setSyncPoint(rig.clipA, 0.3, nullptr);
    rig.drag(rig.body(rig.clipA), {oneSecond * 1.1f, 0.0f}, true);
    CHECK_THAT(clip::getReferenceTime(rig.clipA), WithinAbs(3.5, 1e-6));
    CHECK_THAT(clip::getStart(rig.clipA), WithinAbs(3.2, 1e-6));

    // Trimming the start keeps the sync point on the same material.
    clip::setStart(rig.clipA, 2.0, nullptr);
    rig.drag(rig.leftEdge(rig.clipA), {oneSecond * 0.2f, 0.0f}, false);
    CHECK_THAT(clip::getStart(rig.clipA), WithinAbs(2.2, 0.02));
    REQUIRE(clip::getSyncPoint(rig.clipA).has_value());
    CHECK_THAT(clip::getReferenceTime(rig.clipA), WithinAbs(2.3, 1e-6));
    clip::setSyncPoint(rig.clipA, std::nullopt, nullptr);
    clip::setStart(rig.clipA, 2.0, nullptr);
    clip::setLength(rig.clipA, 4.0, nullptr);
    clip::setOffset(rig.clipA, 0.0, nullptr);

    // Looped: the left edge moves the clip; the right edge draws out more loops.
    rig.project.getSession().loopClip(rig.clipA, 1.0);
    rig.drag(rig.leftEdge(rig.clipA), {oneSecond, 0.0f}, false);
    CHECK(clip::getLength(rig.clipA) == 4.0);
    CHECK_THAT(clip::getStart(rig.clipA), WithinAbs(3.0, 0.02));
    rig.drag(rig.rightEdge(rig.clipA), {oneSecond * 5.0f, 0.0f}, false);
    CHECK_THAT(clip::getLength(rig.clipA), WithinAbs(9.0, 0.05));
    CHECK(clip::getLoopLength(rig.clipA) == 4.0);
}

TEST_CASE("A marquee drag is the time selection; Clip List drags drop copies onto tracks", "[ui][clip][capture]")
{
    ArrangerRig rig;
    auto& session = rig.project.getSession();

    // Drag over track A from 7 s (after its clip) back to 5 s.
    const auto from = juce::Point<float>(rig.xOf(7.0), rig.yOf(rig.clipA) - 4.0f);
    rig.drag(from, {-rig.pixelsPerSecond() * 2.0f, 2.0f}, false);
    const auto selection = rig.editor.getTimeSelection();
    REQUIRE_FALSE(selection.isEmpty());
    CHECK_THAT(selection.start, WithinAbs(5.0, 0.02));
    CHECK_THAT(selection.end, WithinAbs(7.0, 0.02));
    CHECK(selection.trackIds == std::set<std::uint32_t>{track::getId(rig.a)});

    juce::String error;
    auto entry = ui::actions::captureSelection(session, rig.editor, "Hook", error);
    REQUIRE(entry.isValid());
    CHECK_THAT(clip::getOffset(entry), WithinAbs(3.0, 0.02)); // 5 .. 6 s of the clip (the rest is empty)
    CHECK_THAT(clip::getLength(entry), WithinAbs(1.0, 0.02));

    // A click elsewhere clears it.
    rig.click({rig.xOf(14.0), rig.yOf(rig.clipB)});
    CHECK(rig.editor.getTimeSelection().isEmpty());

    // The Clip List panel lists it and describes drags of it.
    ui::ClipListPanel panel(session, rig.editor);
    REQUIRE(panel.getItems().size() == 1);
    const auto description = ui::ClipListPanel::describeDrag(entry);
    CHECK(ui::ClipListPanel::findDragged(session, description) == entry);
    CHECK_FALSE(ui::ClipListPanel::findDragged(session, "something else").isValid());

    // Dropped on track B at 14 s.
    juce::DragAndDropTarget::SourceDetails details(description, &panel, juce::Point<int>(juce::roundToInt(rig.xOf(14.0)), juce::roundToInt(rig.yOf(rig.clipB))));
    REQUIRE(rig.arranger->isInterestedInDragSource(details));
    rig.arranger->itemDropped(details);
    juce::ValueTree placed;
    for (const auto& c : rig.b)
        if (Session::isClip(c) && c != rig.clipB)
            placed = c;
    REQUIRE(placed.isValid());
    CHECK(clip::getName(placed) == "Hook");
    CHECK_THAT(clip::getStart(placed), WithinAbs(14.0, 1e-6));
    CHECK_THAT(clip::getOffset(placed), WithinAbs(3.0, 0.02));

    // Removing it from the list (panel menu), with undo. The copy on track B is a user-defined clip
    // of its own and stays listed.
    panel.removeItem(0);
    panel.refresh();
    REQUIRE(panel.getItems().size() == 1);
    CHECK_FALSE(panel.getItems()[0].captured);
    CHECK(panel.getItems()[0].clip == placed);
    session.getUndoManager().undo();
    panel.refresh();
    REQUIRE(panel.getItems().size() == 1); // (the same clip: listed once)
    CHECK(panel.getItems()[0].captured);
}

TEST_CASE("The text prompt (Loop, Capture, Rename) accepts only what makes sense", "[ui][clip]")
{
    juce::MessageManager::getInstance();
    juce::String got;
    ui::TextPromptPanel loops("Number of loops", "4", "Loop", true);
    loops.onConfirm = [&got](const juce::String& text) { got = text; };
    loops.text.setText("0", juce::dontSendNotification);
    loops.confirm();
    CHECK(got.isEmpty());
    loops.text.setText("2.5", juce::dontSendNotification);
    loops.confirm();
    CHECK(got == "2.5");

    got.clear();
    ui::TextPromptPanel name("Name", "Old", "Rename", false);
    name.onConfirm = [&got](const juce::String& text) { got = text; };
    name.text.setText("   ", juce::dontSendNotification);
    name.confirm();
    CHECK(got.isEmpty());
    name.text.setText("  New name ", juce::dontSendNotification);
    name.confirm();
    CHECK(got == "New name");
}

#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "TestAudio.h"
#include "ui/AppServices.h"
#include "ui/BeatMaker.h"
#include "ui/Commands.h"
#include "ui/MainComponent.h"
#include "ui/Preferences.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <vector>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using session::Session;
using ui::BeatMaker;
namespace clip = session::clip;
namespace note = session::note;

namespace
{
std::vector<juce::ValueTree> notesOf(const juce::ValueTree& midiClip)
{
    std::vector<juce::ValueTree> notes;
    for (const auto& child : midiClip)
        if (child.hasType(session::ids::note))
            notes.push_back(child);
    std::sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) { return note::getStart(a) < note::getStart(b); });
    return notes;
}

/** A BeatMaker with a fake host: what it sends, what it says, a playhead the test moves. */
struct Rig
{
    struct Sent
    {
        std::uint32_t track = 0;
        juce::MidiMessage message;
    };

    explicit Rig(Session& s)
        : session(s)
    {
        BeatMaker::Host host;
        host.isEnabled = [this] { return enabled; };
        host.getTargetTrack = [this] { return target; };
        host.getRecordingClip = [this] { return recording ? clipToRecord : juce::ValueTree(); };
        host.getPlayheadSeconds = [this] { return playhead; };
        host.sendMidi = [this](std::uint32_t track, const juce::MidiMessage& m) { sent.push_back({track, m}); };
        host.onMessage = [this](const juce::String& text) { messages.add(text); };
        host.isKeyDown = [this](int keyCode) { return keysDown.count(keyCode) > 0; };
        beats = std::make_unique<BeatMaker>(session, std::move(host));
    }

    Session& session;
    bool enabled = true;
    std::uint32_t target = 0;
    bool recording = false;
    juce::ValueTree clipToRecord;
    std::optional<double> playhead;
    std::vector<Sent> sent;
    juce::StringArray messages;
    std::set<int> keysDown;
    std::unique_ptr<BeatMaker> beats;
};
} // namespace

TEST_CASE("Beat Maker: keys A to P are the 16 pads, C2 (MIDI 36) upwards", "[beatmaker]")
{
    for (int pad = 0; pad < 16; ++pad)
    {
        const auto upper = static_cast<juce::juce_wchar>('A' + pad);
        const auto lower = static_cast<juce::juce_wchar>('a' + pad);
        CHECK(BeatMaker::padForKey(juce::KeyPress(static_cast<int>(upper), 0, upper)) == pad);
        CHECK(BeatMaker::padForKey(juce::KeyPress(static_cast<int>(lower), 0, lower)) == pad);
        CHECK(BeatMaker::padForKey(juce::KeyPress(static_cast<int>(upper), juce::ModifierKeys::shiftModifier, upper)) == pad);
        CHECK(BeatMaker::keyForPad(pad) == juce::String::charToString(upper));
    }
    CHECK(BeatMaker::keyForPad(16).isEmpty());
    CHECK(BeatMaker::keyForPad(-1).isEmpty());

    // Past P, digits, space: not pads. Ctrl, Alt or Cmd: shortcuts, not pads.
    for (const auto key : {'q', 'r', 'z', '1', ' '})
        CHECK(BeatMaker::padForKey(juce::KeyPress(key, 0, static_cast<juce::juce_wchar>(key))) == -1);
    CHECK(BeatMaker::padForKey(juce::KeyPress('a', juce::ModifierKeys::ctrlModifier, 0)) == -1);
    CHECK(BeatMaker::padForKey(juce::KeyPress('a', juce::ModifierKeys::altModifier, 0)) == -1);
    CHECK(BeatMaker::padForKey(juce::KeyPress('a', juce::ModifierKeys::commandModifier, 0)) == -1);
    // The key code alone (some systems give no character with it).
    CHECK(BeatMaker::padForKey(juce::KeyPress('C', 0, 0)) == 2);

    CHECK(BeatMaker::kFirstNote == 36);
    CHECK(BeatMaker::kPads == 16);
}

TEST_CASE("Beat Maker: where a time falls in a MIDI clip's content", "[beatmaker]")
{
    Session session; // 120 BPM: a beat is half a second
    auto track = session.addTrack(session::TrackKind::instrument, "Drums");
    auto c = session.addMidiClip(track, 2.0, 4.0);
    const auto& map = session.getTempoMap();

    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 2.0), WithinAbs(0.0, 1e-9));
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 3.0), WithinAbs(2.0, 1e-9));
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 5.99), WithinAbs(7.98, 1e-9));
    CHECK_FALSE(BeatMaker::contentBeatAt(c, map, 1.99).has_value());
    CHECK_FALSE(BeatMaker::contentBeatAt(c, map, 6.0).has_value()); // (the end is outside)
    CHECK_FALSE(BeatMaker::contentBeatAt(track, map, 3.0).has_value());

    // The clip's offset (content beats), and the track's speed.
    clip::setOffset(c, 1.0, nullptr);
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 3.0), WithinAbs(3.0, 1e-9));
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 3.0, 2.0), WithinAbs(5.0, 1e-9));

    // Looped: every iteration (one second, two beats) takes the notes into the first.
    clip::setOffset(c, 0.0, nullptr);
    clip::setLoop(c, true, 1.0, nullptr);
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 2.25), WithinAbs(0.5, 1e-9));
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 3.25), WithinAbs(0.5, 1e-9));
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 5.75), WithinAbs(1.5, 1e-9));
    CHECK_THAT(*BeatMaker::contentBeatAt(c, map, 4.0), WithinAbs(0.0, 1e-9));

    // Another tempo: content beats are timeline beats (a second is now one beat).
    session.setTempo(60.0);
    clip::setLoop(c, false, clip::getLength(c), nullptr);
    CHECK_THAT(*BeatMaker::contentBeatAt(c, session.getTempoMap(), clip::getStart(c) + 1.5), WithinAbs(1.5, 1e-9));
}

TEST_CASE("Beat Maker: pads play the target track, held keys do not retrigger", "[beatmaker]")
{
    Session session;
    auto track = session.addTrack(session::TrackKind::instrument, "Drums");
    Rig rig(session);
    rig.target = session::track::getId(track);
    auto& beats = *rig.beats;

    beats.padDown(0, false);
    REQUIRE(rig.sent.size() == 1);
    CHECK(rig.sent[0].track == rig.target);
    CHECK(rig.sent[0].message.isNoteOn());
    CHECK(rig.sent[0].message.getNoteNumber() == 36);
    CHECK(rig.sent[0].message.getVelocity() == BeatMaker::kVelocity);
    CHECK(beats.isHeld(0));

    beats.padDown(0, false); // (key repeat)
    CHECK(rig.sent.size() == 1);
    beats.padUp(0);
    REQUIRE(rig.sent.size() == 2);
    CHECK(rig.sent[1].message.isNoteOff());
    CHECK(rig.sent[1].message.getNoteNumber() == 36);
    CHECK_FALSE(beats.isHeld(0));

    // Shift: an accent. P: the 16th pad.
    beats.padDown(15, true);
    REQUIRE(rig.sent.size() == 3);
    CHECK(rig.sent[2].message.getNoteNumber() == 51);
    CHECK(rig.sent[2].message.getVelocity() == BeatMaker::kAccentVelocity);
    beats.releaseAll();
    CHECK(rig.sent.size() == 4);
    CHECK(rig.sent[3].message.isNoteOff());

    // Not recording: nothing is written anywhere.
    CHECK(beats.getNotesRecorded() == 0);

    // No track to play: it says so, and sends nothing.
    rig.target = 0;
    beats.padDown(1, false);
    beats.padUp(1);
    CHECK(rig.sent.size() == 4);
    CHECK(rig.messages.size() == 1);
}

TEST_CASE("Beat Maker: keys go to pads only while it is on; releases are noticed", "[beatmaker]")
{
    Session session;
    auto track = session.addTrack(session::TrackKind::instrument, "Drums");
    Rig rig(session);
    rig.target = session::track::getId(track);
    auto& beats = *rig.beats;

    rig.enabled = false;
    CHECK_FALSE(beats.keyPressed(juce::KeyPress('a', 0, 'a'), nullptr));
    CHECK(rig.sent.empty());

    rig.enabled = true;
    CHECK_FALSE(beats.keyPressed(juce::KeyPress('r', 0, 'r'), nullptr)); // Record stays R
    CHECK_FALSE(beats.keyPressed(juce::KeyPress('s', juce::ModifierKeys::commandModifier, 0), nullptr));
    CHECK(rig.sent.empty());

    rig.keysDown = {'D'};
    CHECK(beats.keyPressed(juce::KeyPress('D', 0, 'd'), nullptr));
    CHECK(beats.keyPressed(juce::KeyPress('D', 0, 'd'), nullptr)); // (repeat: taken, ignored)
    REQUIRE(rig.sent.size() == 1);
    CHECK(rig.sent[0].message.getNoteNumber() == 39);

    // Another key changes state while D is still down: D stays held.
    CHECK_FALSE(beats.keyStateChanged(false, nullptr));
    CHECK(beats.isHeld(3));
    // D comes up.
    rig.keysDown.clear();
    CHECK_FALSE(beats.keyStateChanged(false, nullptr));
    CHECK_FALSE(beats.isHeld(3));
    REQUIRE(rig.sent.size() == 2);
    CHECK(rig.sent[1].message.isNoteOff());
}

TEST_CASE("Beat Maker: recording into the selected MIDI clip", "[beatmaker]")
{
    Session session; // 120 BPM
    auto track = session.addTrack(session::TrackKind::instrument, "Drums");
    auto c = session.addMidiClip(track, 2.0, 4.0);
    Rig rig(session);
    rig.target = session::track::getId(track);
    rig.clipToRecord = c;
    rig.recording = true;
    auto& beats = *rig.beats;
    auto& undo = session.getUndoManager();
    undo.clearUndoHistory();

    // A at 2.5 s, held to 2.75 s: beat 1, half a beat long, velocity 100.
    rig.playhead = 2.5;
    beats.padDown(0, false);
    rig.playhead = 2.75;
    beats.padUp(0);
    // Shift+C at 3.0 s, a tap: beat 2, accented.
    rig.playhead = 3.0;
    beats.padDown(2, true);
    beats.padUp(2);

    auto notes = notesOf(c);
    REQUIRE(notes.size() == 2);
    CHECK(note::getNote(notes[0]) == 36);
    CHECK_THAT(note::getStart(notes[0]), WithinAbs(1.0, 1e-9));
    CHECK_THAT(note::getLength(notes[0]), WithinAbs(0.5, 1e-9));
    CHECK_THAT(note::getVelocity(notes[0]), WithinAbs(100.0 / 127.0, 1e-6));
    CHECK(note::getNote(notes[1]) == 38);
    CHECK_THAT(note::getStart(notes[1]), WithinAbs(2.0, 1e-9));
    CHECK(note::getLength(notes[1]) >= session::kMinNoteLength); // (no shorter than the shortest note)
    CHECK_THAT(note::getVelocity(notes[1]), WithinAbs(1.0, 1e-6));
    CHECK(beats.getNotesRecorded() == 2);

    // One pass, one undo step.
    CHECK(undo.getUndoDescription() == BeatMaker::kUndoName);
    undo.undo();
    CHECK(notesOf(c).empty());
    CHECK_FALSE(undo.canUndo());

    // A new pass is an undo step of its own.
    rig.playhead = 2.0;
    beats.padDown(1, false);
    beats.padUp(1);
    beats.startPass();
    rig.playhead = 2.5;
    beats.padDown(1, false);
    beats.padUp(1);
    REQUIRE(notesOf(c).size() == 2);
    undo.undo();
    REQUIRE(notesOf(c).size() == 1);
    CHECK_THAT(note::getStart(notesOf(c)[0]), WithinAbs(0.0, 1e-9));

    // Outside the clip: heard, not recorded, and it says why.
    const auto sentBefore = rig.sent.size();
    rig.playhead = 1.0;
    beats.padDown(4, false);
    beats.padUp(4);
    CHECK(rig.sent.size() == sentBefore + 2);
    CHECK(notesOf(c).size() == 1);
    CHECK(rig.messages.size() == 1);

    // Stopped (no playhead): played, not recorded.
    rig.playhead.reset();
    beats.padDown(5, false);
    beats.padUp(5);
    CHECK(notesOf(c).size() == 1);

    // Not recording: played, not recorded.
    rig.recording = false;
    rig.playhead = 2.5;
    beats.padDown(6, false);
    beats.padUp(6);
    CHECK(notesOf(c).size() == 1);
}

TEST_CASE("Beat Maker: a looped clip takes the notes into its loop; a held note across the wrap keeps its length", "[beatmaker]")
{
    Session session; // 120 BPM
    auto track = session.addTrack(session::TrackKind::instrument, "Drums");
    auto c = session.addMidiClip(track, 0.0, 8.0);
    clip::setLoop(c, true, 2.0, nullptr); // one bar, repeated
    Rig rig(session);
    rig.target = session::track::getId(track);
    rig.clipToRecord = c;
    rig.recording = true;
    auto& beats = *rig.beats;

    // Third iteration, beat 1.
    rig.playhead = 4.5;
    beats.padDown(0, false);
    rig.playhead = 4.75;
    beats.padUp(0);
    auto notes = notesOf(c);
    REQUIRE(notes.size() == 1);
    CHECK_THAT(note::getStart(notes[0]), WithinAbs(1.0, 1e-9));
    CHECK_THAT(note::getLength(notes[0]), WithinAbs(0.5, 1e-9));

    // Held over the transport's loop (the playhead went back): timed by the clock instead.
    rig.playhead = 1.9;
    beats.padDown(1, false);
    rig.playhead = 0.1;
    beats.padUp(1);
    notes = notesOf(c);
    REQUIRE(notes.size() == 2);
    CHECK_THAT(note::getStart(notes[1]), WithinAbs(3.8, 1e-9));
    CHECK(note::getLength(notes[1]) >= session::kMinNoteLength);
    CHECK(note::getLength(notes[1]) < 1.0);
}

TEST_CASE("Beat Maker Mode in the app: Record with a MIDI clip selected records the keys into it", "[beatmaker][ui]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;

    engine::AudioEngine engine(0);
    engine.prepare(48'000.0, 256, 2);
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    project.getCompiler().setRenderFormat(48'000.0, 256);
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    session::Recorder recorder(project, controller);
    session::Exporter exporter(project, controller);
    juce::ApplicationProperties settings;
    juce::PropertiesFile::Options options;
    options.applicationName = "test";
    options.filenameSuffix = ".settings";
    options.folderName = temp.dir.getFullPathName();
    settings.setStorageParameters(options);
    engine::PreviewService preview(engine, project.getAssets().getFormatManager());
    ui::Preferences preferences(*settings.getUserSettings());
    juce::ApplicationCommandManager commands;
    ui::AppServices services{project, controller, commands, plugins, recorder, exporter, settings, preview, preferences, {}, {}, {}};

    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::instrument, "Drums");
    auto c = session.addMidiClip(track, 0.0, 8.0, "Beat");
    REQUIRE(test::pumpMessagesUntil([&] { return controller.getLatestGraphSerial() > 0; }));

    const auto render = [&](double seconds)
    {
        std::vector<float> l(256), r(256);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(seconds * 48'000.0); done += 256)
            engine.process(nullptr, 0, channels, 2, 256);
        controller.pump();
    };

    CHECK_FALSE(preferences.getBeatMakerMode()); // (off by default)

    {
        ui::MainComponent main(services);
        main.setSize(1400, 900);
        commands.registerAllCommandsForTarget(&main);
        commands.setFirstCommandTarget(&main);
        auto& beats = main.getBeatMaker();
        std::set<int> keysDown;
        beats.setKeyQuery([&](int keyCode) { return keysDown.count(keyCode) > 0; });
        const auto frame = [&] { main.refreshDisplay(juce::Time::getMillisecondCounterHiRes() / 1000.0); };

        // Off: letters are not pads.
        CHECK_FALSE(beats.keyPressed(juce::KeyPress('a', 0, 'a'), &main));

        preferences.setBeatMakerMode(true);
        main.getEditorState().selectClip(clip::getId(c), false);
        session::track::setArmed(track, true, nullptr); // (a new instrument track is armed)

        // Record: Beat Maker records into the clip, not the recorder (whose take would replace the
        // clip); the transport runs.
        REQUIRE(commands.invokeDirectly(ui::commands::record, false));
        CHECK(main.isBeatRecording());
        CHECK_FALSE(recorder.isRecording());
        juce::ApplicationCommandInfo info(ui::commands::record);
        main.getCommandInfo(ui::commands::record, info);
        CHECK(info.shortName == "Stop Recording");

        render(1.0);
        frame();
        REQUIRE(controller.getLastTelemetry().playing);

        // A at about 1 s (beat 2), held for half a second (a beat).
        keysDown = {'A'};
        CHECK(beats.keyPressed(juce::KeyPress('A', 0, 'a'), &main));
        render(0.5);
        frame();
        keysDown.clear();
        beats.keyStateChanged(false, &main);

        auto notes = notesOf(c);
        REQUIRE(notes.size() == 1);
        CHECK(note::getNote(notes[0]) == 36);
        CHECK_THAT(note::getStart(notes[0]), WithinAbs(2.0, 0.25));
        CHECK_THAT(note::getLength(notes[0]), WithinAbs(1.0, 0.25));
        CHECK_THAT(note::getVelocity(notes[0]), WithinAbs(100.0 / 127.0, 1e-6));

        // Shift+B: the second pad, accented.
        render(0.25);
        frame();
        keysDown = {'B'};
        CHECK(beats.keyPressed(juce::KeyPress('B', juce::ModifierKeys::shiftModifier, 'B'), &main));
        keysDown.clear();
        beats.keyStateChanged(false, &main);
        notes = notesOf(c);
        REQUIRE(notes.size() == 2);
        CHECK(note::getNote(notes[1]) == 37);
        CHECK_THAT(note::getVelocity(notes[1]), WithinAbs(1.0, 1e-6));
        CHECK(session.getUndoManager().getUndoDescription() == BeatMaker::kUndoName);

        // Stop (play/stop): the recording ends with the transport.
        REQUIRE(commands.invokeDirectly(ui::commands::playStop, false));
        CHECK_FALSE(main.isBeatRecording());
        render(0.05);
        frame();
        CHECK_FALSE(controller.getLastTelemetry().playing);

        // Stopped, not recording: the keys play, nothing more is recorded.
        CHECK(beats.keyPressed(juce::KeyPress('C', 0, 'c'), &main));
        beats.releaseAll();
        CHECK(notesOf(c).size() == 2);

        // One undo step takes the pass away.
        session.getUndoManager().undo();
        CHECK(notesOf(c).empty());

        // Recording stops with the transport however it stops (here: the engine stopped itself).
        REQUIRE(commands.invokeDirectly(ui::commands::record, false));
        REQUIRE(main.isBeatRecording());
        render(0.1);
        frame();
        controller.stop();
        render(0.05);
        frame();
        CHECK_FALSE(main.isBeatRecording());

        commands.setFirstCommandTarget(nullptr);
    }
}

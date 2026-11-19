// The A800's deck: the reels carry exactly what the machine plays (the arrangement on the master,
// the track's material on a track) and wind from the left reel to the right one with the transport.

#include "devices/DeviceRegistry.h"
#include "devices/TapeA800.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/EditorState.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/PagedEditor.h"
#include "ui/devices/TapeDeck.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using devices::TapeA800;
namespace tape = dojo::ui::tape;

namespace
{
/** Plays one block through the machine with the transport at `seconds`. */
void at(ProcessorNode& node, TapeA800& machine, double seconds, bool playing, bool recording = false)
{
    TestPlayHead head;
    head.info.setTimeInSeconds(seconds);
    head.info.setIsPlaying(playing);
    head.info.setIsRecording(recording);
    machine.setPlayHead(&head);
    juce::AudioBuffer<double> buffer(2, kBlock);
    buffer.clear();
    juce::MidiBuffer midi;
    node.process(buffer, kBlock, midi, nullptr);
    machine.setPlayHead(nullptr);
}
} // namespace

TEST_CASE("Tape: the machine knows where the tape is and whether it records", "[devices][tape]")
{
    auto [node, machine] = makeNode<TapeA800>();
    at(*node, *machine, 12.5, true, true);
    CHECK_THAT(machine->getTapeSeconds(), WithinAbs(12.5, 1e-12));
    CHECK(machine->isRolling());
    CHECK(machine->isRecording());
    at(*node, *machine, 30.0, false, true); // (a stopped transport records nothing)
    CHECK_THAT(machine->getTapeSeconds(), WithinAbs(30.0, 1e-12));
    CHECK_FALSE(machine->isRolling());
    CHECK_FALSE(machine->isRecording());
}

TEST_CASE("Tape deck: the packs keep the tape's area and the reels turn at the tape speed", "[ui][tape]")
{
    constexpr double length = 200.0, ips = 15.0;
    const double hub = tape::kHubRadius, full = tape::kFullRadius;

    // All on the left reel at the head of the tape, all on the right at its tail; past either end
    // the tape stays there.
    const auto head = tape::reelsAt(0.0, length, ips);
    CHECK_THAT(head.leftRadius, WithinAbs(full, 1e-12));
    CHECK_THAT(head.rightRadius, WithinAbs(hub, 1e-12));
    CHECK(head.leftAngle == 0.0);
    CHECK(head.rightAngle == 0.0);
    const auto tail = tape::reelsAt(length, length, ips);
    CHECK_THAT(tail.leftRadius, WithinAbs(hub, 1e-12));
    CHECK_THAT(tail.rightRadius, WithinAbs(full, 1e-12));
    CHECK(tail.wound == 1.0);
    CHECK(tape::reelsAt(-5.0, length, ips).leftRadius == head.leftRadius);
    CHECK(tape::reelsAt(length + 60.0, length, ips).rightAngle == tail.rightAngle);

    // Halfway, the packs are the same size; anywhere, together they hold one reel's worth of tape.
    const auto half = tape::reelsAt(length * 0.5, length, ips);
    CHECK_THAT(half.leftRadius, WithinAbs(half.rightRadius, 1e-12));
    CHECK_THAT(half.leftRadius, WithinAbs(std::sqrt((full * full + hub * hub) * 0.5), 1e-12));
    for (const double t : {13.0, 77.0, 151.0})
    {
        const auto r = tape::reelsAt(t, length, ips);
        CHECK_THAT(r.leftRadius * r.leftRadius + r.rightRadius * r.rightRadius - 2.0 * hub * hub, WithinRel(full * full - hub * hub, 1e-12));
        CHECK(r.wound == t / length);
    }

    // Each reel turns at the tape speed over its own radius: the emptier one faster.
    for (const double t : {20.0, 100.0, 180.0})
    {
        constexpr double dt = 1.0e-4;
        const auto a = tape::reelsAt(t, length, ips), b = tape::reelsAt(t + dt, length, ips);
        CHECK_THAT((b.leftAngle - a.leftAngle) / dt, WithinRel(ips / a.leftRadius, 1e-3));
        CHECK_THAT((b.rightAngle - a.rightAngle) / dt, WithinRel(ips / a.rightRadius, 1e-3));
    }
    CHECK(tape::reelsAt(20.0, length, ips).rightAngle < tape::reelsAt(20.0, length, ips * 2.0).rightAngle);
    // Over the whole tape both turn equally often: its length over the mean circumference.
    CHECK_THAT(tail.leftAngle, WithinRel(tail.rightAngle, 1e-12));
    CHECK_THAT(tail.rightAngle, WithinRel(2.0 * ips * length / (full + hub), 1e-12));

    // A standard 2,500 ft reel: 2,000 s at 15 ips, 1,000 s at 30.
    CHECK_THAT(tape::standardReelSeconds(15.0), WithinAbs(2000.0, 1e-9));
    CHECK_THAT(tape::standardReelSeconds(30.0), WithinAbs(1000.0, 1e-9));
}

TEST_CASE("Tape deck: the tape is as long as what the machine plays", "[ui][tape][session]")
{
    session::Session session;
    const double ips = 15.0;
    // An empty session: a standard reel.
    CHECK(session.getContentEnd(session.getMaster()) == 0.0);
    CHECK_THAT(tape::lengthFor(session, session.getMaster(), ips), WithinAbs(tape::standardReelSeconds(ips), 1e-9));

    auto drums = session.addTrack(session::TrackKind::audio, "Drums");
    auto bass = session.addTrack(session::TrackKind::audio, "Bass");
    auto bus = session.addTrack(session::TrackKind::bus, "Rhythm Bus");
    session.addClip(drums, juce::File("/drums.wav"), 0.0, 20.0);
    session.addClip(drums, juce::File("/drums.wav"), 25.0, 5.0); // (ends at 30 s)
    session.addClip(bass, juce::File("/bass.wav"), 40.0, 10.0);  // (ends at 50 s)

    // The master: the whole arrangement. A track: up to the end of its last clip.
    CHECK(session.getContentEnd(session.getMaster()) == 50.0);
    CHECK(session.getContentEnd(drums) == 30.0);
    CHECK(session.getContentEnd(bass) == 50.0);
    CHECK_THAT(tape::lengthFor(session, drums, ips), WithinAbs(30.0, 1e-12));
    CHECK_THAT(tape::lengthFor(session, session.getMaster(), ips), WithinAbs(50.0, 1e-12));

    // A bus: what feeds it (routed to it, or sending to it, active).
    CHECK(session.getContentEnd(bus) == 0.0);
    CHECK_THAT(tape::lengthFor(session, bus, ips), WithinAbs(50.0, 1e-12)); // (nothing yet: the arrangement)
    session::track::setOutput(drums, session::track::getId(bus), nullptr);
    CHECK(session.getContentEnd(bus) == 30.0);
    REQUIRE(session.addSend(bass, bus).isValid());
    CHECK(session.getContentEnd(bus) == 50.0);
    session::track::setActive(bass, false, nullptr);
    CHECK(session.getContentEnd(bus) == 30.0);
    session::track::setActive(bass, true, nullptr);

    // A folder: its members.
    auto keys = session.addTrack(session::TrackKind::audio, "Keys");
    session.addClip(keys, juce::File("/keys.wav"), 0.0, 12.0);
    auto folder = session.groupIntoFolder({keys}, "Keys Folder");
    REQUIRE(folder.isValid());
    CHECK(session.getContentEnd(folder) == 12.0);

    // An instrument: its own MIDI, and the MIDI tracks played into it; its aux track: the same.
    auto synth = session.addTrack(session::TrackKind::instrument, "Synth");
    session.addMidiClip(synth, 2.0, 4.0);
    CHECK(session.getContentEnd(synth) == 6.0);
    auto notes = session.addTrack(session::TrackKind::midi, "Notes");
    session.addMidiClip(notes, 60.0, 8.0);
    session::track::setMidiTarget(notes, session::track::getId(synth), nullptr);
    CHECK(session.getContentEnd(synth) == 68.0);
    auto aux = session.addTrack(session::TrackKind::bus, "Synth 3-4");
    session::track::setAuxSource(aux, session::track::getId(synth), 2, nullptr);
    CHECK(session.getContentEnd(aux) == 68.0);
    CHECK(session.getContentEnd(session.getMaster()) == 68.0);
}

TEST_CASE("Tape deck: the reels follow the transport, wind to a locate and stop at the end", "[ui][tape]")
{
    juce::MessageManager::getInstance();
    auto [node, machine] = makeNode<TapeA800>();
    const double ips = TapeA800::inchesPerSecond(machine->speed_->getIndex());
    at(*node, *machine, 0.0, false);
    ui::TapeDeck deck(*machine, [] { return 100.0; });
    deck.setSize(436, 190);
    CHECK(deck.getTapeLength() == 100.0);
    CHECK(deck.getShownSeconds() == 0.0);
    CHECK(deck.getMotion() == ui::TapeDeck::Motion::stopped);
    CHECK(deck.getReels().leftRadius == tape::kFullRadius);

    // Playing: the tape is where the transport is, the reels as far round as the tape has run.
    for (const double t : {0.1, 0.13, 0.17, 0.2})
    {
        at(*node, *machine, t, true);
        deck.update(1.0 / 30.0);
        CHECK(deck.getShownSeconds() == t);
        CHECK(deck.getMotion() == ui::TapeDeck::Motion::playing);
    }
    CHECK(deck.getReels().rightAngle == tape::reelsAt(0.2, 100.0, ips).rightAngle);
    CHECK(deck.getBlur(0) == 0.0f);
    at(*node, *machine, 0.23, true, true);
    deck.update(1.0 / 30.0);
    CHECK(deck.getMotion() == ui::TapeDeck::Motion::recording);

    // A locate winds there, fast forward, the flanges a blur, and the tape stops on the spot.
    at(*node, *machine, 80.0, false);
    deck.update(1.0 / 30.0);
    CHECK(deck.getMotion() == ui::TapeDeck::Motion::fastForward);
    CHECK(deck.getShownSeconds() > 0.23);
    CHECK(deck.getShownSeconds() < 80.0);
    float blur = 0.0f;
    for (int frame = 0; frame < 90 && deck.getMotion() != ui::TapeDeck::Motion::stopped; ++frame)
    {
        deck.update(1.0 / 30.0);
        blur = std::max(blur, deck.getBlur(1));
    }
    CHECK(deck.getMotion() == ui::TapeDeck::Motion::stopped);
    CHECK(deck.getShownSeconds() == 80.0);
    CHECK(blur > 0.5f);
    for (int frame = 0; frame < 30; ++frame)
        deck.update(1.0 / 30.0);
    CHECK(deck.getBlur(1) == 0.0f);

    // Back to the start: a rewind.
    at(*node, *machine, 0.0, false);
    deck.update(1.0 / 30.0);
    CHECK(deck.getMotion() == ui::TapeDeck::Motion::rewinding);
    for (int frame = 0; frame < 120; ++frame)
        deck.update(1.0 / 30.0);
    CHECK(deck.getShownSeconds() == 0.0);
    CHECK(deck.getReels().leftRadius == tape::kFullRadius);

    // Past the end: all of it on the right reel, the END lamp lit.
    at(*node, *machine, 130.0, true);
    for (int frame = 0; frame < 120; ++frame)
        deck.update(1.0 / 30.0);
    CHECK(deck.isAtEnd());
    CHECK(deck.getReels().wound == 1.0);
    CHECK_THAT(deck.getReels().rightRadius, WithinAbs(tape::kFullRadius, 1e-12));
    (void) deck.createComponentSnapshot(deck.getLocalBounds());

    // The timer.
    CHECK(ui::TapeDeck::timerText(0.0) == "00:00.0");
    CHECK(ui::TapeDeck::timerText(83.46) == "01:23.4");
    CHECK(ui::TapeDeck::timerText(6000.0) == "100:00.0");
    CHECK(ui::TapeDeck::timerText(-3.0) == "00:00.0");
}

TEST_CASE("Tape deck: the editor's deck takes its length from the track or the master it is on", "[ui][tape]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    ui::EditorState editorState;
    auto synth = session.addTrack(session::TrackKind::instrument, "Synth");
    session.addMidiClip(synth, 0.0, 42.0);
    auto lead = session.addTrack(session::TrackKind::instrument, "Lead");
    session.addMidiClip(lead, 10.0, 80.0);

    const auto deckOn = [&](juce::ValueTree owner, std::unique_ptr<juce::Component>& editor) -> ui::TapeDeck*
    {
        auto device = session.addDevice(owner, TapeA800::kTypeId, "A800");
        auto node = project.getDevices().getNode(device);
        REQUIRE(node != nullptr);
        int width = 0;
        editor = ui::createDeviceEditor(*node, ui::DeviceEditorContext{project, editorState, device, owner}, width);
        CHECK(width >= 400);
        auto* pages = dynamic_cast<ui::PagedEditor*>(editor.get());
        REQUIRE(pages != nullptr);
        CHECK(pages->getPageTitle(0) == "Tape");
        CHECK(pages->getMissingParameters().isEmpty());
        return dynamic_cast<ui::TapeDeck*>(pages->getDisplay());
    };
    std::unique_ptr<juce::Component> onTrack, onMaster;
    auto* trackDeck = deckOn(synth, onTrack);
    auto* masterDeck = deckOn(session.getMaster(), onMaster);
    REQUIRE(trackDeck != nullptr);
    REQUIRE(masterDeck != nullptr);
    CHECK(trackDeck->getTapeLength() == 42.0);
    CHECK(masterDeck->getTapeLength() == 90.0);

    // It follows the session as it changes.
    session.addMidiClip(synth, 50.0, 10.0);
    for (int frame = 0; frame < 10; ++frame)
        trackDeck->update(1.0 / 30.0);
    CHECK(trackDeck->getTapeLength() == 60.0);
}

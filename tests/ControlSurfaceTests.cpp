// Control surfaces: the Mackie Control and HUI protocols, and the surfaces working the session,
// the transport and the commands, with their faders, LEDs, displays and meters following it.

#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/Commands.h"
#include "ui/EditorState.h"
#include "ui/Preferences.h"
#include "ui/PreferencesWindow.h"
#include "ui/surfaces/ControlSurfaces.h"
#include "ui/surfaces/SurfaceProtocol.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

using namespace dojo;
using namespace dojo::ui::surfaces;
using Catch::Matchers::WithinAbs;

namespace
{
std::vector<juce::uint8> bytesOf(const juce::MidiMessage& message)
{
    return {message.getRawData(), message.getRawData() + message.getRawDataSize()};
}

/** A surface's side of the cable: what The Dojo sent it. */
struct Wire
{
    std::vector<juce::MidiMessage> messages;
    void clear() { messages.clear(); }
    [[nodiscard]] bool contains(const juce::MidiMessage& wanted) const
    {
        const auto want = bytesOf(wanted);
        return std::any_of(messages.begin(), messages.end(), [&](const juce::MidiMessage& m) { return bytesOf(m) == want; });
    }
    /** The last pitch bend on a channel (a Mackie fader), or -1. */
    [[nodiscard]] int lastFader(int channel) const
    {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            if (it->isPitchWheel() && it->getChannel() == channel)
                return it->getPitchWheelValue();
        return -1;
    }
    /** The text sent to a Mackie LCD line (position 0 or 56), or empty. */
    [[nodiscard]] juce::String lcd(int position) const
    {
        for (auto it = messages.rbegin(); it != messages.rend(); ++it)
            if (it->isSysEx() && it->getSysExDataSize() > 6 && it->getSysExData()[4] == 0x12 && it->getSysExData()[5] == position)
                return juce::String(reinterpret_cast<const char*>(it->getSysExData() + 6), static_cast<std::size_t>(it->getSysExDataSize() - 6));
        return {};
    }
};

juce::MidiMessage key(int note, bool pressed = true)
{
    return juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(pressed ? 127 : 0));
}

/** Searches a component's children (and theirs) for an ID. */
juce::Component* findById(juce::Component& parent, const juce::String& id)
{
    if (parent.getComponentID() == id)
        return &parent;
    for (auto* child : parent.getChildren())
        if (auto* found = findById(*child, id))
            return found;
    return nullptr;
}
} // namespace

TEST_CASE("Mackie Control protocol: faders, keys, V-Pots, jog, displays, meters and the handshake", "[ui][surfaces]")
{
    // Faders: pitch bends on channels 1-9, both ways.
    const auto fader = mackie::fader(3, 12000);
    CHECK(bytesOf(fader) == std::vector<juce::uint8>{0xE3, 12000 & 0x7F, 12000 >> 7});
    auto event = mackie::decode(fader);
    REQUIRE(event.has_value());
    CHECK(event->kind == Event::Kind::fader);
    CHECK(event->strip == 3);
    CHECK(event->value == 12000);
    CHECK(mackie::decode(mackie::fader(8, 5))->strip == 8); // (the master fader)

    // Every key's note goes both ways.
    for (const auto k : {Key::recArm, Key::solo, Key::mute, Key::select, Key::vpotPress})
        for (int strip = 0; strip < 8; ++strip)
            CHECK(mackie::keyOf(mackie::noteOf(k, strip)) == std::pair{k, strip});
    for (const auto k : {Key::play, Key::stop, Key::record, Key::rewind, Key::fastForward, Key::cycle, Key::bankLeft, Key::bankRight,
                         Key::channelLeft, Key::channelRight, Key::flip, Key::assignPan, Key::assignSend, Key::save, Key::undo, Key::shift,
                         Key::scrub, Key::up, Key::down, Key::nameValue, Key::smpteBeats, Key::drop, Key::replace})
        CHECK(mackie::keyOf(mackie::noteOf(k)).first == k);
    CHECK(mackie::noteOf(Key::play) == 0x5E);
    CHECK(mackie::noteOf(Key::mute, 2) == 0x12);
    CHECK(mackie::keyOf(0x36 + 4) == std::pair{Key::function, 4});

    event = mackie::decode(key(0x5E));
    REQUIRE(event.has_value());
    CHECK(event->kind == Event::Kind::key);
    CHECK(event->key == Key::play);
    CHECK(event->value == 1);
    CHECK(mackie::decode(key(0x5E, false))->value == 0);
    event = mackie::decode(key(0x68 + 2));
    CHECK(event->kind == Event::Kind::faderTouch);
    CHECK(event->strip == 2);
    CHECK(mackie::decode(key(0x70))->strip == 8);

    // V-Pots and the jog wheel: bit 6 turns back.
    event = mackie::decode(juce::MidiMessage::controllerEvent(1, 0x12, 0x03));
    CHECK(event->kind == Event::Kind::vpot);
    CHECK(event->strip == 2);
    CHECK(event->value == 3);
    CHECK(mackie::decode(juce::MidiMessage::controllerEvent(1, 0x12, 0x43))->value == -3);
    event = mackie::decode(juce::MidiMessage::controllerEvent(1, 0x3C, 0x41));
    CHECK(event->kind == Event::Kind::jog);
    CHECK(event->value == -1);

    // LEDs, rings, meters.
    CHECK(bytesOf(mackie::led(Key::record, 0, Led::on)) == std::vector<juce::uint8>{0x90, 0x5F, 0x7F});
    CHECK(bytesOf(mackie::led(Key::mute, 3, Led::blink)) == std::vector<juce::uint8>{0x90, 0x13, 0x01});
    CHECK(bytesOf(mackie::ring(1, 0, 6, false)) == std::vector<juce::uint8>{0xB0, 0x31, 0x06});
    CHECK(bytesOf(mackie::ring(1, 2, 11, true)) == std::vector<juce::uint8>{0xB0, 0x31, 0x40 | 0x20 | 11});
    CHECK(bytesOf(mackie::meter(3, 12)) == std::vector<juce::uint8>{0xD0, 0x3C});

    // The LCD and the time display.
    CHECK(bytesOf(mackie::lcd(mackie::kMain, 56, "Ab")) == std::vector<juce::uint8>{0xF0, 0x00, 0x00, 0x66, 0x14, 0x12, 56, 'A', 'b', 0xF7});
    CHECK(bytesOf(mackie::lcd(mackie::kExtender, 0, "X"))[4] == 0x15);
    const auto time = mackie::timeDisplay("  1. 1. 1.000");
    REQUIRE(time.size() == 10);
    CHECK(bytesOf(time[9]) == std::vector<juce::uint8>{0xB0, 0x49, 0x20});        // leftmost: blank
    CHECK(bytesOf(time[7]) == std::vector<juce::uint8>{0xB0, 0x47, 0x31 | 0x40}); // "1." (its dot lit)
    CHECK(bytesOf(time[0]) == std::vector<juce::uint8>{0xB0, 0x40, 0x30});        // rightmost: "0"
    const auto assignment = mackie::assignmentDisplay("PN");
    CHECK(bytesOf(assignment[0]) == std::vector<juce::uint8>{0xB0, 0x4B, 'P' - 0x40});

    // The handshake: a challenge is answered with the serial and four bytes.
    const std::vector<juce::uint8> challenge{0x00, 0x00, 0x66, 0x14, 0x01, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x20, 0x30, 0x40};
    event = mackie::decode(juce::MidiMessage::createSysExMessage(challenge.data(), static_cast<int>(challenge.size())));
    REQUIRE(event.has_value());
    CHECK(event->kind == Event::Kind::handshake);
    const auto answer = bytesOf(mackie::answerChallenge(mackie::kMain, event->serial, event->challenge));
    REQUIRE(answer.size() == 1 + 5 + 7 + 4 + 1);
    CHECK(std::vector<juce::uint8>(answer.begin(), answer.begin() + 6) == std::vector<juce::uint8>{0xF0, 0x00, 0x00, 0x66, 0x14, 0x02});
    CHECK(std::vector<juce::uint8>(answer.begin() + 6, answer.begin() + 13) == std::vector<juce::uint8>{1, 2, 3, 4, 5, 6, 7});
    for (std::size_t i = 13; i < 17; ++i)
        CHECK(answer[i] < 0x80);
    const std::vector<juce::uint8> confirmed{0x00, 0x00, 0x66, 0x14, 0x03, 1, 2, 3, 4, 5, 6, 7};
    CHECK(mackie::decode(juce::MidiMessage::createSysExMessage(confirmed.data(), static_cast<int>(confirmed.size())))->kind == Event::Kind::online);
    CHECK_FALSE(mackie::decode(juce::MidiMessage::createSysExMessage(confirmed.data(), static_cast<int>(confirmed.size())), mackie::kExtender).has_value());

    // Meter segments as the meters are printed; the fader law is the mixer's.
    CHECK(meterSegment(-70.0f) == 0);
    CHECK(meterSegment(-60.0f) == 1);
    CHECK(meterSegment(-15.0f) == 5);
    CHECK(meterSegment(-14.0f) == 6);
    CHECK(meterSegment(0.0f) == 12);
    CHECK(meterSegment(3.0f) == 12);
    CHECK(faderPosition(-60.0) == 0);
    CHECK(faderPosition(-80.0) == 0);
    CHECK(faderPosition(6.0) == 16383);
    CHECK(std::abs(faderPosition(-12.0) - 8192) <= 1);
    CHECK_THAT(faderDecibels(faderPosition(-3.0)), WithinAbs(-3.0, 0.02));
}

TEST_CASE("HUI protocol: zones and ports, fader pairs, pings, displays", "[ui][surfaces]")
{
    hui::Decoder decoder;
    // A key is a zone, then its port (+64 pressed).
    CHECK_FALSE(decoder.decode(juce::MidiMessage::controllerEvent(1, 0x0F, hui::kZoneTransport)).has_value());
    auto event = decoder.decode(juce::MidiMessage::controllerEvent(1, 0x2F, 0x44));
    REQUIRE(event.has_value());
    CHECK(event->key == Key::play);
    CHECK(event->value == 1);
    CHECK(decoder.decode(juce::MidiMessage::controllerEvent(1, 0x2F, 0x04))->value == 0);
    // A strip's zone: port 0 is the fader's touch, 2 its mute.
    (void) decoder.decode(juce::MidiMessage::controllerEvent(1, 0x0F, 3));
    event = decoder.decode(juce::MidiMessage::controllerEvent(1, 0x2F, 0x40));
    CHECK(event->kind == Event::Kind::faderTouch);
    CHECK(event->strip == 3);
    event = decoder.decode(juce::MidiMessage::controllerEvent(1, 0x2F, 0x42));
    CHECK(event->key == Key::mute);
    CHECK(event->strip == 3);
    // Faders: the high seven bits, then the low seven.
    CHECK_FALSE(decoder.decode(juce::MidiMessage::controllerEvent(1, 0x02, 0x40)).has_value());
    event = decoder.decode(juce::MidiMessage::controllerEvent(1, 0x22, 0x05));
    CHECK(event->kind == Event::Kind::fader);
    CHECK(event->strip == 2);
    CHECK(event->value == (0x40 << 7 | 0x05));
    // V-Pots, the ping's answer.
    CHECK(decoder.decode(juce::MidiMessage::controllerEvent(1, 0x41, 0x42))->value == -2);
    CHECK(decoder.decode(juce::MidiMessage::noteOn(1, 0, juce::uint8{0x7F}))->kind == Event::Kind::pingReply);

    for (const auto k : {Key::recArm, Key::solo, Key::mute, Key::select, Key::vpotPress})
        CHECK(hui::keyOf(hui::switchOf(k, 5).first, hui::switchOf(k, 5).second) == std::pair{k, 5});
    for (const auto k : {Key::play, Key::stop, Key::record, Key::rewind, Key::fastForward, Key::bankLeft, Key::bankRight, Key::cycle,
                         Key::undo, Key::save, Key::returnToZero})
        CHECK(hui::keyOf(hui::switchOf(k).first, hui::switchOf(k).second).first == k);

    const auto led = hui::led(Key::stop, 0, Led::on);
    REQUIRE(led.size() == 2);
    CHECK(bytesOf(led[0]) == std::vector<juce::uint8>{0xB0, 0x0C, hui::kZoneTransport});
    CHECK(bytesOf(led[1]) == std::vector<juce::uint8>{0xB0, 0x2C, 0x43});
    const auto fader = hui::fader(1, 0x2005);
    CHECK(bytesOf(fader[0]) == std::vector<juce::uint8>{0xB0, 0x01, 0x40});
    CHECK(bytesOf(fader[1]) == std::vector<juce::uint8>{0xB0, 0x21, 0x05});
    CHECK(bytesOf(hui::ping()) == std::vector<juce::uint8>{0x90, 0x00, 0x00});
    CHECK(bytesOf(hui::meter(2, 1, 9)) == std::vector<juce::uint8>{0xA0, 0x02, 0x19});
    CHECK(bytesOf(hui::stripName(4, "Kick Drum")) == std::vector<juce::uint8>{0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x10, 4, 'K', 'i', 'c', 'k', 0xF7});
    // (the rightmost digit first; 0x10 lights a digit's dot)
    CHECK(bytesOf(hui::timeDisplay("01.02.03.04")) == std::vector<juce::uint8>{0xF0, 0x00, 0x00, 0x66, 0x05, 0x00, 0x11, 4, 0, 0x13, 0, 0x12, 0, 0x11, 0, 0xF7});
}

TEST_CASE("Control surfaces work the mixer, the transport and the commands, and show the session", "[ui][surfaces]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    for (int i = 1; i <= 20; ++i)
        session.addTrack(session::TrackKind::audio, "T" + juce::String(i));
    const auto tracks = session.getAllTracks();
    auto bus = session.addTrack(session::TrackKind::bus, "Verb");
    auto send = session.addSend(tracks[1], bus, -10.0);

    ui::EditorState editor;
    std::vector<juce::CommandID> commands;
    bool playing = false;
    double playhead = 8.0;
    std::vector<double> locations;
    Host host;
    host.invoke = [&](juce::CommandID command) { commands.push_back(command); };
    host.editor = [&] { return &editor; };
    host.playing = [&] { return playing; };
    host.recording = [] { return false; };
    host.playhead = [&] { return playhead; };
    host.locate = [&](double seconds)
    {
        locations.push_back(seconds);
        playhead = seconds;
    };

    ControlSurfaces surfaces(project, host, nullptr);
    surfaces.setSetups({{Model::mackieControl, {}, {}}, {Model::mackieExtender, {}, {}}});
    REQUIRE(surfaces.getNumStrips() == 16);
    CHECK(surfaces.describeStatus(0) == "Pick its MIDI ports");
    Wire main, extender;
    surfaces.setSender(0, [&](const juce::MidiMessage& m) { main.messages.push_back(m); });
    surfaces.setSender(1, [&](const juce::MidiMessage& m) { extender.messages.push_back(m); });
    surfaces.refresh(true);

    // The two units side by side: tracks 1-8, then 9-16.
    CHECK(main.lcd(0).startsWith("T1     T2     T3     "));
    CHECK(extender.lcd(0).startsWith("T9     T10    "));
    CHECK(extender.lcd(0).length() == 56);
    CHECK(main.lastFader(1) == faderPosition(0.0));
    CHECK(main.contains(mackie::led(Key::stop, 0, Led::on)));
    CHECK(main.contains(mackie::led(Key::assignPan, 0, Led::on)));
    CHECK(main.lcd(56).startsWith("  <C>  "));

    SECTION("Faders: the track's volume, touched or not, and they follow it")
    {
        surfaces.receive(0, key(0x68 + 1));
        surfaces.receive(0, mackie::fader(1, faderPosition(-6.0)));
        CHECK_THAT(session::track::getVolumeDb(tracks[1]), WithinAbs(-6.0, 0.05));
        // While touched, the fader is the hand's: no motor moves.
        main.clear();
        session::track::setVolumeDb(tracks[1], -20.0, nullptr);
        surfaces.refresh();
        CHECK(main.lastFader(2) == -1);
        surfaces.receive(0, key(0x68 + 1, false));
        surfaces.refresh();
        CHECK(main.lastFader(2) == faderPosition(-20.0));
        // One undo step per touch.
        session.getUndoManager().undo();
        CHECK_THAT(session::track::getVolumeDb(tracks[1]), WithinAbs(0.0, 0.05));
        // The extender's first fader is track 9; the master fader is the master's.
        surfaces.receive(1, mackie::fader(0, faderPosition(-12.0)));
        CHECK_THAT(session::track::getVolumeDb(tracks[8]), WithinAbs(-12.0, 0.05));
        surfaces.receive(0, mackie::fader(8, faderPosition(-3.0)));
        CHECK_THAT(session.getMasterVolumeDb(), WithinAbs(-3.0, 0.05));
    }

    SECTION("Strip keys, lit as the tracks are")
    {
        surfaces.receive(0, key(mackie::noteOf(Key::mute, 0)));
        surfaces.receive(0, key(mackie::noteOf(Key::solo, 1)));
        surfaces.receive(0, key(mackie::noteOf(Key::recArm, 2)));
        surfaces.receive(0, key(mackie::noteOf(Key::select, 3)));
        CHECK(session::track::isMuted(tracks[0]));
        CHECK(session::track::isSoloed(tracks[1]));
        CHECK(session::track::isArmed(tracks[2]));
        CHECK(editor.getSelectedTrack() == session::track::getId(tracks[3]));
        main.clear();
        surfaces.refresh();
        CHECK(main.contains(mackie::led(Key::mute, 0, Led::on)));
        CHECK(main.contains(mackie::led(Key::solo, 1, Led::on)));
        CHECK(main.contains(mackie::led(Key::recArm, 2, Led::on)));
        CHECK(main.contains(mackie::led(Key::select, 3, Led::on)));
        CHECK(main.contains(juce::MidiMessage::noteOn(1, 0x73, juce::uint8{0x7F}))); // (RUDE SOLO)
        // Shift + select adds to the selection; RUDE SOLO clears every solo.
        surfaces.receive(0, key(mackie::noteOf(Key::shift)));
        surfaces.receive(0, key(mackie::noteOf(Key::select, 4)));
        surfaces.receive(0, key(mackie::noteOf(Key::shift), false));
        CHECK(editor.isTrackSelected(session::track::getId(tracks[3])));
        CHECK(editor.isTrackSelected(session::track::getId(tracks[4])));
        surfaces.receive(0, key(mackie::noteOf(Key::rudeSolo)));
        CHECK_FALSE(session::track::isSoloed(tracks[1]));
    }

    SECTION("V-Pots: pan, a send, the ring; pressed, back to the default; FLIP")
    {
        surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x10, 5));
        CHECK_THAT(session::track::getPan(tracks[0]), WithinAbs(0.1, 1e-6));
        main.clear();
        surfaces.refresh();
        CHECK(main.contains(mackie::ring(0, 0, 7)));
        CHECK(main.lcd(56).startsWith("  R10  "));
        // A turn is one undo step (until a pause, or another V-Pot).
        surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x10, 5));
        CHECK_THAT(session::track::getPan(tracks[0]), WithinAbs(0.2, 1e-6));
        session.getUndoManager().undo();
        CHECK_THAT(session::track::getPan(tracks[0]), WithinAbs(0.0, 1e-9));
        surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x10, 5));
        surfaces.tick(1.5);
        surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x10, 5));
        session.getUndoManager().undo();
        CHECK_THAT(session::track::getPan(tracks[0]), WithinAbs(0.1, 1e-6));
        surfaces.receive(0, key(mackie::noteOf(Key::vpotPress, 0)));
        CHECK_THAT(session::track::getPan(tracks[0]), WithinAbs(0.0, 1e-9));

        // SEND: the first send's level (track 2 has one).
        surfaces.receive(0, key(mackie::noteOf(Key::assignSend)));
        CHECK(surfaces.getVpotMode() == ControlSurfaces::VpotMode::send);
        surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x11, 4));
        CHECK_THAT(session::send::getGainDb(send), WithinAbs(-8.0, 1e-6));
        main.clear();
        surfaces.refresh();
        CHECK(main.lcd(56).substring(7, 14) == " -8.0  ");
        CHECK(main.lcd(56).substring(0, 7).trim() == "--");
        CHECK(main.contains(mackie::led(Key::assignSend, 0, Led::on)));

        // FLIP: the fader takes over the send, the V-Pot the volume.
        surfaces.receive(0, key(mackie::noteOf(Key::flip)));
        surfaces.receive(0, mackie::fader(1, faderPosition(-3.0)));
        CHECK_THAT(session::send::getGainDb(send), WithinAbs(-3.0, 0.05));
        surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x11, 0x42));
        CHECK_THAT(session::track::getVolumeDb(tracks[1]), WithinAbs(-1.0, 1e-6));
    }

    SECTION("Banks and channels, and the selection moving the bank")
    {
        surfaces.receive(0, key(mackie::noteOf(Key::bankRight)));
        CHECK(surfaces.getBank() == 5); // (21 tracks, 16 strips: the last bank shows the last track)
        main.clear();
        surfaces.refresh();
        CHECK(main.lcd(0).startsWith("T6     T7     "));
        surfaces.receive(0, key(mackie::noteOf(Key::channelLeft)));
        CHECK(surfaces.getBank() == 4);
        surfaces.receive(0, key(mackie::noteOf(Key::bankLeft)));
        CHECK(surfaces.getBank() == 0);
        editor.selectTrack(session::track::getId(tracks[15]));
        surfaces.receive(0, key(mackie::noteOf(Key::down)));
        CHECK(editor.getSelectedTrack() == session::track::getId(tracks[16]));
        CHECK(surfaces.getBank() == 1);
    }

    SECTION("The transport and the commands")
    {
        surfaces.receive(0, key(mackie::noteOf(Key::play)));
        CHECK(commands.back() == ui::commands::playStop);
        surfaces.receive(0, key(mackie::noteOf(Key::stop)));
        CHECK(commands.back() == ui::commands::returnToStart); // (stopped: back to the start)
        playing = true;
        surfaces.receive(0, key(mackie::noteOf(Key::stop)));
        CHECK(commands.back() == ui::commands::playStop);
        const auto before = commands.size();
        surfaces.receive(0, key(mackie::noteOf(Key::play))); // (already playing)
        CHECK(commands.size() == before);
        surfaces.receive(0, key(mackie::noteOf(Key::record)));
        CHECK(commands.back() == ui::commands::record);
        surfaces.receive(0, key(mackie::noteOf(Key::cycle)));
        CHECK(commands.back() == ui::commands::toggleLoop);
        surfaces.receive(0, key(mackie::noteOf(Key::drop)));
        CHECK(commands.back() == ui::commands::togglePunchIn);
        surfaces.receive(0, key(mackie::noteOf(Key::save)));
        CHECK(commands.back() == ui::commands::saveSession);
        surfaces.receive(0, key(mackie::noteOf(Key::undo)));
        CHECK(commands.back() == ui::commands::undo);
        surfaces.receive(0, key(mackie::noteOf(Key::shift)));
        surfaces.receive(0, key(mackie::noteOf(Key::undo)));
        CHECK(commands.back() == ui::commands::redo);
        surfaces.receive(0, key(mackie::noteOf(Key::shift), false));
        surfaces.receive(0, key(mackie::noteOf(Key::function, 0)));
        CHECK(commands.back() == ui::commands::showMixer);
        main.clear();
        surfaces.refresh();
        CHECK(main.contains(mackie::led(Key::play, 0, Led::on)));
        CHECK(main.contains(mackie::led(Key::stop, 0, Led::off)));

        // REWIND: a bar back now, then again while held; the jog wheel: a beat per tick.
        const double beat = 60.0 / session.getTempo();
        playhead = 20.0;
        surfaces.receive(0, key(mackie::noteOf(Key::rewind)));
        CHECK_THAT(playhead, WithinAbs(20.0 - 4 * beat, 1e-9));
        surfaces.tick(0.3);
        CHECK(locations.size() == 1);
        surfaces.tick(0.2); // (held past 0.4 s: repeating)
        CHECK(locations.size() == 2);
        surfaces.receive(0, key(mackie::noteOf(Key::rewind), false));
        surfaces.tick(1.0);
        CHECK(locations.size() == 2);
        surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x3C, 0x02));
        CHECK_THAT(playhead, WithinAbs(20.0 - 8 * beat + 2 * beat, 1e-9));
    }

    SECTION("The time display, in bars and beats or in time")
    {
        playhead = 60.0 / session.getTempo() * 9.5; // (bar 3, beat 2, the third sixteenth)
        main.clear();
        surfaces.refresh();
        for (const auto& message : mackie::timeDisplay("  3. 2. 3.000"))
            CHECK(main.contains(message));
        surfaces.receive(0, key(mackie::noteOf(Key::smpteBeats)));
        playhead = 83.5;
        main.clear();
        surfaces.refresh();
        for (const auto& message : mackie::timeDisplay("  0.01.23.500"))
            CHECK(main.contains(message));
        CHECK(main.contains(juce::MidiMessage::noteOn(1, 0x71, juce::uint8{0x7F})));
    }

    SECTION("A Mackie Control's handshake is answered")
    {
        const std::vector<juce::uint8> challenge{0x00, 0x00, 0x66, 0x14, 0x01, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x20, 0x30, 0x40};
        main.clear();
        surfaces.receive(0, juce::MidiMessage::createSysExMessage(challenge.data(), static_cast<int>(challenge.size())));
        CHECK(main.contains(mackie::answerChallenge(mackie::kMain, {1, 2, 3, 4, 5, 6, 7}, {0x10, 0x20, 0x30, 0x40})));
        CHECK(surfaces.getStatus(0).online);
    }
}

TEST_CASE("HUI surfaces: pinged, online while they answer, and worked through zones", "[ui][surfaces]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    auto kick = session.addTrack(session::TrackKind::audio, "Kick Drum");
    std::vector<juce::CommandID> commands;
    Host host;
    host.invoke = [&](juce::CommandID command) { commands.push_back(command); };
    ControlSurfaces surfaces(project, host, nullptr);
    surfaces.setSetups({{Model::hui, {}, {}}});
    Wire wire;
    surfaces.setSender(0, [&](const juce::MidiMessage& m) { wire.messages.push_back(m); });
    surfaces.refresh(true);
    CHECK(wire.contains(hui::stripName(0, "Kick")));
    CHECK(wire.contains(hui::fader(0, faderPosition(0.0))[0]));

    // Pinged once a second; online while it answers.
    wire.clear();
    surfaces.tick(1.0);
    CHECK(wire.contains(hui::ping()));
    CHECK_FALSE(surfaces.getStatus(0).online);
    surfaces.receive(0, juce::MidiMessage::noteOn(1, 0, juce::uint8{0x7F}));
    CHECK(surfaces.getStatus(0).online);
    surfaces.tick(3.5);
    CHECK_FALSE(surfaces.getStatus(0).online);

    // Its keys and faders.
    surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x0F, 0));
    surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x2F, 0x42));
    CHECK(session::track::isMuted(kick));
    surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x0F, hui::kZoneTransport));
    surfaces.receive(0, juce::MidiMessage::controllerEvent(1, 0x2F, 0x44));
    CHECK(commands.back() == ui::commands::playStop);
    for (const auto& message : hui::fader(0, faderPosition(-9.0)))
        surfaces.receive(0, message);
    CHECK_THAT(session::track::getVolumeDb(kick), WithinAbs(-9.0, 0.05));
    wire.clear();
    surfaces.refresh();
    const auto led = hui::led(Key::mute, 0, Led::on);
    CHECK(wire.contains(led[1]));
}

TEST_CASE("Control surfaces: setups kept as text; the Preferences page adds, shows and removes them", "[ui][surfaces][preferences]")
{
    const std::vector<Setup> setups{{Model::mackieControl, "in-1", "out-1"}, {Model::hui, "in|2", "out:2"}};
    CHECK(setupsFromString(toString(setups)) == setups);
    CHECK(setupsFromString("").empty());
    CHECK(setupsFromString("not json").empty());

    // A surface's MIDI input can only be taken if it is there.
    engine::AudioEngine engine;
    engine::AudioDeviceHost host(engine);
    CHECK_FALSE(host.routeMidiInput("no such input", [](const juce::MidiMessage&) {}));

    juce::MessageManager::getInstance();
    test::TempFolder temp;
    juce::PropertiesFile::Options options;
    options.applicationName = "test";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    juce::PropertiesFile settings(temp.dir.getChildFile("settings.xml"), options);
    ui::Preferences preferences(settings);
    auto page = ui::createControlSurfacesPage(preferences, [](int index) { return "status " + juce::String(index); });
    page->setSize(760, 520);
    auto& window = *page;

    auto* add = dynamic_cast<juce::TextButton*>(findById(window, "addSurface"));
    REQUIRE(add != nullptr);
    add->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    add->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    auto stored = setupsFromString(preferences.getControlSurfaces());
    REQUIRE(stored.size() == 2);
    CHECK(stored[0].model == Model::mackieControl);
    CHECK(stored[1].model == Model::mackieExtender); // (the next one: an extender)
    auto* status = dynamic_cast<juce::Label*>(findById(window, "surfaceStatus1"));
    REQUIRE(status != nullptr);
    CHECK(status->getText() == "status 1");

    // Changing a row's model, removing one.
    auto* model = dynamic_cast<juce::ComboBox*>(findById(window, "surfaceModel1"));
    REQUIRE(model != nullptr);
    model->setSelectedId(static_cast<int>(Model::hui) + 1, juce::sendNotificationSync);
    CHECK(setupsFromString(preferences.getControlSurfaces())[1].model == Model::hui);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100); // (the rows are rebuilt)
    auto* remove = dynamic_cast<juce::TextButton*>(findById(window, "surfaceRemove0"));
    REQUIRE(remove != nullptr);
    remove->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(100);
    stored = setupsFromString(preferences.getControlSurfaces());
    REQUIRE(stored.size() == 1);
    CHECK(stored[0].model == Model::hui);
}

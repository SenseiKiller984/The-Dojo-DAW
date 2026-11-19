// MIDI effects: Arpeggiator, Chord Generator, Scale and MIDI Filter, on their own and in the chain
// in front of an instrument (instrument and MIDI tracks).

#include "core/rt/RealtimeGuard.h"
#include "devices/DeviceRegistry.h"
#include "devices/MidiEffects.h"
#include "engine/AudioEngine.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"
#include "ui/DeviceActions.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/devices/MidiEffectEditors.h"
#include "EffectTestHelpers.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>

using namespace dojo;
using namespace dojo::test::devices;
using devices::Arpeggiator;
using devices::ChordGenerator;
using devices::MidiFilter;
using devices::ScaleDevice;

namespace
{
using Timed = std::vector<std::pair<int, juce::MidiMessage>>;

/** Runs a MIDI effect for `total` samples, fed `in` (absolute sample times), the transport at `bpm`
    from beat `startPpq` (playing or stopped). Its output, at absolute sample times. */
Timed run(devices::MidiEffect& effect, const Timed& in, int total, double bpm = 120.0, bool playing = true, double startPpq = 0.0)
{
    TestPlayHead head;
    effect.setPlayHead(&head);
    juce::AudioBuffer<double> audio(2, kBlock);
    juce::MidiBuffer midi;
    Timed out;
    for (int offset = 0; offset < total; offset += kBlock)
    {
        const int n = std::min(kBlock, total - offset);
        head.info.setIsPlaying(playing);
        head.info.setBpm(bpm);
        head.info.setPpqPosition(startPpq + static_cast<double>(offset) / kRate * bpm / 60.0);
        head.info.setTimeInSeconds(static_cast<double>(offset) / kRate);
        midi.clear();
        for (const auto& [sample, message] : in)
            if (sample >= offset && sample < offset + n)
                midi.addEvent(message, sample - offset);
        audio.setSize(2, n, false, false, true);
        audio.clear();
        effect.processBlock(audio, midi);
        for (const auto meta : midi)
            out.emplace_back(offset + meta.samplePosition, meta.getMessage());
    }
    effect.setPlayHead(nullptr);
    return out;
}

/** The note-ons (or note-offs) of an output: sample and note. */
std::vector<std::pair<int, int>> notes(const Timed& events, bool on = true)
{
    std::vector<std::pair<int, int>> result;
    for (const auto& [sample, message] : events)
        if (on ? message.isNoteOn() : message.isNoteOff())
            result.emplace_back(sample, message.getNoteNumber());
    return result;
}

std::vector<int> pitches(const Timed& events, bool on = true)
{
    std::vector<int> result;
    for (const auto& [sample, note] : notes(events, on))
        result.push_back(note);
    return result;
}

juce::MidiMessage on(int note, int velocity = 100, int channel = 1)
{
    return juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(velocity));
}

juce::MidiMessage off(int note, int channel = 1)
{
    return juce::MidiMessage::noteOff(channel, note);
}

std::vector<int> sequence(int style, std::initializer_list<int> played, int octaves = 1)
{
    std::vector<Arpeggiator::Held> held;
    for (const int note : played)
        held.push_back({note, 100, 1, 0});
    std::array<Arpeggiator::Held, Arpeggiator::kMaxSequence> expanded{};
    std::array<int, Arpeggiator::kMaxSequence> steps{};
    int expandedCount = 0;
    const int length = Arpeggiator::buildSequence(style, held.data(), static_cast<int>(held.size()), octaves, expanded, expandedCount, steps);
    std::vector<int> result;
    for (int i = 0; i < length; ++i)
        result.push_back(expanded[static_cast<std::size_t>(steps[static_cast<std::size_t>(i)])].note);
    return result;
}

template <typename Effect>
std::unique_ptr<Effect> prepared()
{
    auto effect = std::make_unique<Effect>();
    effect->prepareToPlay(kRate, kBlock);
    return effect;
}
} // namespace

TEST_CASE("Arpeggiator: the styles step through the held notes (and their octaves)", "[devices][midi]")
{
    using A = Arpeggiator;
    CHECK(sequence(A::up, {64, 60, 67}) == std::vector<int>{60, 64, 67});
    CHECK(sequence(A::down, {64, 60, 67}) == std::vector<int>{67, 64, 60});
    CHECK(sequence(A::upDown, {64, 60, 67}) == std::vector<int>{60, 64, 67, 64});
    CHECK(sequence(A::downUp, {64, 60, 67}) == std::vector<int>{67, 64, 60, 64});
    CHECK(sequence(A::converge, {60, 62, 64, 65}) == std::vector<int>{60, 65, 62, 64});
    CHECK(sequence(A::diverge, {60, 62, 64, 65}) == std::vector<int>{64, 62, 65, 60});
    CHECK(sequence(A::asPlayed, {64, 60, 67}) == std::vector<int>{64, 60, 67});
    CHECK(sequence(A::up, {60, 64, 67}, 2) == std::vector<int>{60, 64, 67, 72, 76, 79});
    CHECK(sequence(A::upDown, {60}) == std::vector<int>{60});
    CHECK(sequence(A::up, {120}, 3) == std::vector<int>{120}); // (octaves above 127 are left out)
    CHECK(A::rateBeats(9) == 0.25);                             // 1/16
    CHECK(A::rateBeats(10) * 3.0 == 0.5);                       // 1/16T: three in an eighth
}

TEST_CASE("Arpeggiator: on the song's grid, with gate, swing, repeats and velocity", "[devices][midi]")
{
    const Timed chord{{0, on(60, 90)}, {0, on(64, 80)}, {0, on(67, 70)}};
    // 1/16 at 120 bpm: a step every 6000 samples, each half a step long.
    {
        auto arp = prepared<Arpeggiator>();
        const auto out = run(*arp, chord, 48000);
        const auto ons = notes(out);
        REQUIRE(ons.size() >= 8);
        const std::vector<std::pair<int, int>> expected{{0, 60}, {6000, 64}, {12000, 67}, {18000, 60}, {24000, 64}, {30000, 67}, {36000, 60}, {42000, 64}};
        CHECK(std::vector<std::pair<int, int>>(ons.begin(), ons.begin() + 8) == expected);
        CHECK(notes(out, false).front() == std::pair{3000, 60});
        // Velocity as played.
        CHECK(out.front().second.getVelocity() == 90);
    }
    // Swing: every second step half a step later at 100 %.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "swing", 100.0f);
        const auto ons = notes(run(*arp, chord, 30000));
        REQUIRE(ons.size() >= 4);
        CHECK(ons[1].first == 9000);
        CHECK(ons[2].first == 12000);
        CHECK(ons[3].first == 21000);
    }
    // Repeats, fixed velocity, a full gate and two octaves.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "repeats", 1.0f); // (2)
        setParameter(*arp, "velmode", 1.0f);
        setParameter(*arp, "velocity", 33.0f);
        setParameter(*arp, "gate", 100.0f);
        setParameter(*arp, "octaves", 1.0f); // (2)
        const auto out = run(*arp, chord, 48000);
        const auto played = pitches(out);
        REQUIRE(played.size() >= 8);
        CHECK(std::vector<int>(played.begin(), played.begin() + 8) == std::vector<int>{60, 60, 64, 64, 67, 67, 72, 72});
        for (const auto& [sample, message] : out)
            if (message.isNoteOn())
                CHECK(message.getVelocity() == 33);
        CHECK(notes(out, false).front() == std::pair{6000, 60});
    }
    // Chord: every held note at each step.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "style", static_cast<float>(Arpeggiator::chord));
        const auto ons = notes(run(*arp, chord, 7000));
        CHECK(ons == std::vector<std::pair<int, int>>{{0, 60}, {0, 64}, {0, 67}, {6000, 60}, {6000, 64}, {6000, 67}});
    }
    // A late note waits for the grid; other MIDI passes straight through.
    {
        auto arp = prepared<Arpeggiator>();
        const auto out = run(*arp, {{1000, on(62)}, {1500, juce::MidiMessage::controllerEvent(1, 1, 64)}}, 13000);
        CHECK(notes(out) == std::vector<std::pair<int, int>>{{6000, 62}, {12000, 62}});
        CHECK(std::count_if(out.begin(), out.end(), [](const auto& e) { return e.second.isController() && e.first == 1500; }) == 1);
    }
}

TEST_CASE("Arpeggiator: free-running without the transport; Hold; Retrigger; stopping", "[devices][midi]")
{
    // Stopped: the first note starts the pattern at once, then the tempo's step length.
    {
        auto arp = prepared<Arpeggiator>();
        const auto ons = notes(run(*arp, {{1000, on(60)}, {1000, on(67)}}, 14000, 120.0, false));
        CHECK(ons == std::vector<std::pair<int, int>>{{1000, 60}, {7000, 67}, {13000, 60}});
    }
    // Free rate (no sync): 10 Hz is 4800 samples.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "sync", 0.0f);
        setParameter(*arp, "freerate", 10.0f);
        const auto ons = notes(run(*arp, {{0, on(60)}}, 10000));
        CHECK(ons == std::vector<std::pair<int, int>>{{0, 60}, {4800, 60}, {9600, 60}});
    }
    // Hold: the notes play on after the keys are let go; a new chord replaces them.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "hold", 1.0f);
        const auto out = run(*arp, {{0, on(60)}, {0, on(64)}, {100, off(60)}, {100, off(64)}, {14000, on(70)}, {14100, off(70)}}, 31000);
        const auto ons = notes(out);
        CHECK(ons == std::vector<std::pair<int, int>>{{0, 60}, {6000, 64}, {12000, 60}, {18000, 70}, {24000, 70}, {30000, 70}});
    }
    // Without Hold, letting go stops it.
    {
        auto arp = prepared<Arpeggiator>();
        const auto ons = notes(run(*arp, {{0, on(60)}, {7000, off(60)}}, 30000));
        CHECK(ons == std::vector<std::pair<int, int>>{{0, 60}, {6000, 60}});
    }
    // Retrigger: a new note starts the pattern over.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "retrigger", 1.0f);
        const auto ons = notes(run(*arp, {{0, on(60)}, {0, on(64)}, {0, on(67)}, {10000, on(62)}}, 13000));
        CHECK(ons == std::vector<std::pair<int, int>>{{0, 60}, {6000, 64}, {12000, 60}});
    }
    // All notes off (the transport stopping) ends what plays and clears the notes.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "gate", 200.0f);
        const auto out = run(*arp, {{0, on(60)}, {0, on(64)}, {7000, juce::MidiMessage::allNotesOff(1)}}, 30000);
        CHECK(notes(out) == std::vector<std::pair<int, int>>{{0, 60}, {6000, 64}});
        CHECK(notes(out, false) == std::vector<std::pair<int, int>>{{7000, 60}, {7000, 64}}); // (their gates were longer)
        CHECK(arp->getShownLength() == 0);
    }
    // The display shows the pattern.
    {
        auto arp = prepared<Arpeggiator>();
        setParameter(*arp, "hold", 1.0f);
        (void) run(*arp, {{0, on(60)}, {0, on(64)}, {0, on(67)}}, 7000);
        REQUIRE(arp->getShownLength() == 3);
        CHECK(arp->getShownNote(0) == 60);
        CHECK(arp->getShownNote(2) == 67);
        CHECK(arp->getShownStep() == 1);
    }
}

TEST_CASE("Chord Generator: shapes, diatonic chords, voicing, strum and clean releases", "[devices][midi]")
{
    const auto chordOf = [](ChordGenerator& chord, int note)
    {
        std::array<int, ChordGenerator::kMaxChord> notes{};
        const int count = chord.chordFor(note, notes);
        return std::vector<int>(notes.begin(), notes.begin() + count);
    };
    auto chord = prepared<ChordGenerator>();
    CHECK(chordOf(*chord, 60) == std::vector<int>{60, 64, 67}); // (Major by default)
    setParameter(*chord, "shape", 11.0f);                       // m7
    CHECK(chordOf(*chord, 57) == std::vector<int>{57, 60, 64, 67});
    setParameter(*chord, "shape", 0.0f); // Custom: the shifts
    setParameter(*chord, "shift1", 7.0f);
    setParameter(*chord, "shift2", 12.0f);
    setParameter(*chord, "shift3", -12.0f);
    CHECK(chordOf(*chord, 60) == std::vector<int>{48, 60, 67, 72});

    // Diatonic in C major: each degree its own triad (or seventh).
    setParameter(*chord, "mode", 1.0f);
    CHECK(chordOf(*chord, 62) == std::vector<int>{62, 65, 69});  // D minor
    CHECK(chordOf(*chord, 71) == std::vector<int>{71, 74, 77});  // B diminished
    CHECK(chordOf(*chord, 61) == std::vector<int>{60, 64, 67});  // (off the scale: from its nearest note)
    setParameter(*chord, "type", 1.0f);                          // sevenths
    CHECK(chordOf(*chord, 67) == std::vector<int>{67, 71, 74, 77}); // G7
    setParameter(*chord, "key", 9.0f);                           // A
    setParameter(*chord, "scale", 1.0f);                         // minor
    setParameter(*chord, "type", 0.0f);
    CHECK(chordOf(*chord, 57) == std::vector<int>{57, 60, 64}); // A minor
    setParameter(*chord, "mode", 0.0f);
    setParameter(*chord, "shape", 1.0f); // Major

    // Inversion and voicing.
    setParameter(*chord, "inversion", 1.0f);
    CHECK(chordOf(*chord, 60) == std::vector<int>{64, 67, 72});
    setParameter(*chord, "inversion", 0.0f);
    setParameter(*chord, "shape", 10.0f); // Maj7
    setParameter(*chord, "voicing", 1.0f);
    CHECK(chordOf(*chord, 60) == std::vector<int>{55, 60, 64, 71}); // drop 2
    setParameter(*chord, "voicing", 2.0f);
    CHECK(chordOf(*chord, 60) == std::vector<int>{60, 67, 76, 83}); // spread
    setParameter(*chord, "voicing", 0.0f);
    setParameter(*chord, "shape", 1.0f);

    // Playing: the chord on, and off with its key; added notes at their own velocity.
    setParameter(*chord, "velocity", 50.0f);
    auto out = run(*chord, {{0, on(60, 100)}, {1000, off(60)}}, 2000);
    CHECK(notes(out) == std::vector<std::pair<int, int>>{{0, 60}, {0, 64}, {0, 67}});
    for (const auto& [sample, message] : out)
        if (message.isNoteOn())
            CHECK(message.getVelocity() == (message.getNoteNumber() == 60 ? 100 : 50));
    CHECK(notes(out, false).size() == 3);
    CHECK(chord->getShownCount() == 3);

    // Two chords sharing a note: it sounds until both have let it go.
    chord = prepared<ChordGenerator>();
    out = run(*chord, {{0, on(60)}, {500, on(57)}, {1000, off(60)}, {2000, off(57)}}, 3000); // C (60 64 67), A (57 61 64)
    CHECK(notes(out, false) == std::vector<std::pair<int, int>>{{1000, 60}, {1000, 67}, {2000, 57}, {2000, 61}, {2000, 64}});

    // Strum: 10 ms between the notes upwards; letting go before the last cancels it.
    setParameter(*chord, "strum", 10.0f);
    out = run(*chord, {{100, on(60)}, {700, off(60)}}, 3000);
    CHECK(notes(out) == std::vector<std::pair<int, int>>{{100, 60}, {580, 64}});
    CHECK(notes(out, false) == std::vector<std::pair<int, int>>{{700, 60}, {700, 64}});
    setParameter(*chord, "strumdir", 1.0f);
    out = run(*chord, {{0, on(60)}, {2000, off(60)}}, 3000);
    CHECK(notes(out) == std::vector<std::pair<int, int>>{{0, 67}, {480, 64}, {960, 60}});
}

TEST_CASE("Scale: nearest, up, down, remove and white keys; transpose, shift, octave", "[devices][midi]")
{
    auto scale = prepared<ScaleDevice>();
    CHECK(scale->map(61) == 60); // C major, nearest (a tie goes down)
    CHECK(scale->map(66) == 65);
    CHECK(scale->map(62) == 62);
    setParameter(*scale, "mode", 1.0f);
    CHECK(scale->map(61) == 62);
    setParameter(*scale, "mode", 2.0f);
    CHECK(scale->map(66) == 65);
    setParameter(*scale, "mode", 3.0f);
    CHECK(scale->map(61) == -1);
    CHECK(scale->map(64) == 64);

    // White keys: D Dorian from middle C, black keys silent.
    setParameter(*scale, "mode", 4.0f);
    setParameter(*scale, "root", 2.0f);
    setParameter(*scale, "scale", 4.0f); // Dorian
    CHECK(scale->map(60) == 62);
    CHECK(scale->map(62) == 64);
    CHECK(scale->map(64) == 65);
    CHECK(scale->map(72) == 74);
    CHECK(scale->map(59) == 60); // (B below: the degree below the root)
    CHECK(scale->map(61) == -1);
    // A pentatonic on the white keys: five per octave.
    setParameter(*scale, "root", 0.0f);
    setParameter(*scale, "scale", 9.0f); // major pentatonic
    CHECK(scale->map(67) == 69);         // (G: the fifth degree, A)
    CHECK(scale->map(69) == 72);         // (A, the sixth white key: the next octave's root)

    // Transpose before, shift (degrees) and octave after.
    setParameter(*scale, "mode", 0.0f);
    setParameter(*scale, "scale", 0.0f);
    setParameter(*scale, "transpose", 2.0f);
    CHECK(scale->map(60) == 62);
    CHECK(scale->map(64) == 65); // (F# to F)
    setParameter(*scale, "transpose", 0.0f);
    setParameter(*scale, "shift", 2.0f);
    CHECK(scale->map(60) == 64);
    CHECK(scale->map(71) == 74);
    setParameter(*scale, "shift", 0.0f);
    setParameter(*scale, "octave", -1.0f);
    CHECK(scale->map(60) == 48);
    setParameter(*scale, "octave", 0.0f);

    // A note's release (and aftertouch) follow where its note went, whatever changed since.
    auto out = run(*scale, {{0, on(61)}, {500, juce::MidiMessage::aftertouchChange(1, 61, 40)}}, 1000);
    CHECK(notes(out) == std::vector<std::pair<int, int>>{{0, 60}});
    CHECK(out.back().second.isAftertouch());
    CHECK(out.back().second.getNoteNumber() == 60);
    CHECK(scale->getLastIn() == 61);
    CHECK(scale->getLastOut() == 60);
    setParameter(*scale, "mode", 1.0f);
    out = run(*scale, {{0, off(61)}}, 100);
    CHECK(notes(out, false) == std::vector<std::pair<int, int>>{{0, 60}});
    // Removed notes vanish, their releases too.
    setParameter(*scale, "mode", 3.0f);
    out = run(*scale, {{0, on(61)}, {10, off(61)}, {20, on(62)}}, 100);
    CHECK(notes(out) == std::vector<std::pair<int, int>>{{20, 62}});
    CHECK(notes(out, false).empty());
}

TEST_CASE("MIDI Filter: note and velocity ranges, channels, message types; releases always pass", "[devices][midi]")
{
    auto filter = prepared<MidiFilter>();
    setParameter(*filter, "low", 60.0f);
    setParameter(*filter, "high", 72.0f);
    auto out = run(*filter, {{0, on(59)}, {1, on(60)}, {2, on(72)}, {3, on(73)}}, 100);
    CHECK(pitches(out) == std::vector<int>{60, 72});
    setParameter(*filter, "outside", 1.0f);
    out = run(*filter, {{0, on(59)}, {1, on(61)}}, 100);
    CHECK(pitches(out) == std::vector<int>{59});
    setParameter(*filter, "outside", 0.0f);
    CHECK(filter->getParameterById("low")->getText(filter->getParameterById("low")->getValue(), 8) == "C3");
    CHECK(filter->getParameterById("low")->convertFrom0to1(filter->getParameterById("low")->getValueForText("F#4")) == 78.0f);

    // Velocities: a range, then scaled and offset.
    setParameter(*filter, "vellow", 40.0f);
    setParameter(*filter, "velhigh", 100.0f);
    setParameter(*filter, "velscale", 50.0f);
    setParameter(*filter, "veloffset", 10.0f);
    out = run(*filter, {{0, on(60, 30)}, {1, on(62, 80)}, {2, on(64, 110)}}, 100);
    REQUIRE(notes(out).size() == 1);
    CHECK(out.front().second.getVelocity() == 50);

    // A note let through is released, even after the range changed; one held back is not.
    filter = prepared<MidiFilter>();
    setParameter(*filter, "high", 64.0f);
    (void) run(*filter, {{0, on(62)}}, 10);
    setParameter(*filter, "high", 61.0f);
    out = run(*filter, {{0, off(62)}, {1, off(70)}}, 10);
    CHECK(notes(out, false) == std::vector<std::pair<int, int>>{{0, 62}});

    // Channels: only channel 2, sent out on 5 (and released there).
    filter = prepared<MidiFilter>();
    setParameter(*filter, "channel", 2.0f);
    setParameter(*filter, "outchannel", 5.0f);
    out = run(*filter, {{0, on(60, 100, 1)}, {1, on(62, 100, 2)}, {2, off(62, 2)}}, 10);
    REQUIRE(out.size() == 2);
    CHECK(out[0].second.getChannel() == 5);
    CHECK(out[1].second.isNoteOff());
    CHECK(out[1].second.getChannel() == 5);

    // Message types.
    filter = prepared<MidiFilter>();
    setParameter(*filter, "cc", 0.0f);
    setParameter(*filter, "program", 0.0f);
    out = run(*filter, {{0, juce::MidiMessage::controllerEvent(1, 7, 100)}, {1, juce::MidiMessage::pitchWheel(1, 9000)},
                        {2, juce::MidiMessage::programChange(1, 5)}, {3, juce::MidiMessage::channelPressureChange(1, 50)}},
               10);
    REQUIRE(out.size() == 2);
    CHECK(out[0].second.isPitchWheel());
    CHECK(out[1].second.isChannelPressure());
    CHECK(filter->getSeen(MidiFilter::controlKind) == 1);
    CHECK(filter->getPassed(MidiFilter::controlKind) == 0);
    CHECK(filter->getPassed(MidiFilter::pitchBendKind) == 1);
    // All notes off always passes, and ends the notes it sent out on another channel.
    setParameter(*filter, "outchannel", 3.0f);
    out = run(*filter, {{0, on(60)}, {5, juce::MidiMessage::allNotesOff(1)}}, 10);
    CHECK(notes(out, false) == std::vector<std::pair<int, int>>{{5, 60}});
    CHECK(out.back().second.isAllNotesOff());
    setParameter(*filter, "other", 0.0f);
    const juce::uint8 sysex[] = {0x7e, 0x7f, 0x06, 0x01};
    out = run(*filter, {{0, juce::MidiMessage::createSysExMessage(sysex, 4)}}, 10);
    CHECK(out.empty());
}

//==================================================================================================
// In the chain

namespace
{
/** A test instrument that writes down the notes it is played (no allocation). */
class NoteRecorder final : public juce::AudioProcessor
{
public:
    static constexpr const char* kTypeId = "test:recorder";
    NoteRecorder()
        : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
    }
    const juce::String getName() const override { return "Recorder"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        for (const auto meta : midi)
            if (meta.numBytes >= 3 && (meta.data[0] & 0xf0) == 0x90 && meta.data[2] > 0)
                if (const int count = count_.load(); count < static_cast<int>(notes_.size()))
                {
                    notes_[static_cast<std::size_t>(count)] = meta.data[1];
                    count_.store(count + 1);
                }
    }
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

    [[nodiscard]] std::vector<int> played() const
    {
        return std::vector<int>(notes_.begin(), notes_.begin() + count_.load());
    }

private:
    std::array<int, 256> notes_{};
    std::atomic<int> count_{0};
};

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    if (session::device::getType(device) == NoteRecorder::kTypeId)
        return std::make_unique<NoteRecorder>();
    auto processor = devices::createBuiltinDevice(session::device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

struct ChainRig
{
    ChainRig()
        : engine(2)
        , devices(session, createDevice)
    {
        engine.prepare(kRate, kBlock, 2);
        devices.setPlayHead(engine.getPlayHead());
        devices.prepareAll(kRate, kBlock);
        builder.setRenderFormat(kRate, kBlock);
    }

    std::unique_ptr<engine::RenderGraph> publish()
    {
        auto graph = builder.build(session, assets, &devices);
        auto copy = builder.build(session, assets, &devices);
        graph->serial = 1;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
        return copy;
    }

    void play(double seconds)
    {
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            engine.releaseRetiredGraphs();
        }
    }

    NoteRecorder& recorder(const juce::ValueTree& device)
    {
        auto node = devices.getNode(device);
        REQUIRE(node != nullptr);
        return dynamic_cast<NoteRecorder&>(node->getProcessor());
    }

    template <typename Effect>
    Effect& effect(const juce::ValueTree& device)
    {
        auto node = devices.getNode(device);
        REQUIRE(node != nullptr);
        return dynamic_cast<Effect&>(node->getProcessor());
    }

    engine::AudioEngine engine;
    session::Session session;
    session::AudioAssetCache assets;
    session::DeviceManager devices;
    session::GraphBuilder builder;
};
} // namespace

TEST_CASE("MIDI effects play before the instrument: on its track, and on a MIDI track playing it", "[engine][midi]")
{
    // An instrument track: Scale (C major), then the instrument. C#3 comes out as C3.
    {
        ChainRig rig;
        auto synth = rig.session.addTrack(session::TrackKind::instrument, "Synth");
        const auto recorder = rig.session.addDevice(synth, NoteRecorder::kTypeId, "Recorder");
        const auto scale = rig.session.addDevice(synth, ScaleDevice::kTypeId, "Scale", 0);
        (void) scale;
        auto clip = rig.session.addMidiClip(synth, 0.0, 2.0);
        rig.session.addNote(clip, 0.0, 1.0, 61);
        rig.session.addNote(clip, 1.0, 1.0, 66);
        const auto graph = rig.publish();
        const auto& track = *std::find_if(graph->tracks.begin(), graph->tracks.end(), [&](const auto& t) { return t.trackId == session::track::getId(synth); });
        CHECK(track.instrumentIndex == 1);
        rig.play(1.0);
        CHECK(rig.recorder(recorder).played() == std::vector<int>{60, 65});
    }
    // A MIDI track with an Arpeggiator plays the instrument track: its notes arrive arpeggiated.
    {
        ChainRig rig;
        auto synth = rig.session.addTrack(session::TrackKind::instrument, "Synth");
        const auto recorder = rig.session.addDevice(synth, NoteRecorder::kTypeId, "Recorder");
        auto keys = rig.session.addTrack(session::TrackKind::midi, "Keys");
        session::track::setMidiTarget(keys, session::track::getId(synth), nullptr);
        rig.session.addDevice(keys, Arpeggiator::kTypeId, "Arpeggiator");
        auto clip = rig.session.addMidiClip(keys, 0.0, 4.0);
        rig.session.addNote(clip, 0.0, 2.0, 60); // (a beat: four sixteenths at 120)
        rig.session.addNote(clip, 0.0, 2.0, 64);
        (void) rig.publish();
        rig.play(0.45);
        CHECK(rig.recorder(recorder).played() == std::vector<int>{60, 64, 60, 64});
    }
    // A MIDI Filter in front lets nothing through: the instrument hears nothing.
    {
        ChainRig rig;
        auto synth = rig.session.addTrack(session::TrackKind::instrument, "Synth");
        const auto recorder = rig.session.addDevice(synth, NoteRecorder::kTypeId, "Recorder");
        const auto filter = rig.session.addDevice(synth, MidiFilter::kTypeId, "MIDI Filter", 0);
        setParameter(rig.effect<MidiFilter>(filter), "notes", 0.0f);
        auto clip = rig.session.addMidiClip(synth, 0.0, 2.0);
        rig.session.addNote(clip, 0.0, 1.0, 60);
        (void) rig.publish();
        rig.play(0.5);
        CHECK(rig.recorder(recorder).played().empty());
    }
}

TEST_CASE("MIDI effects keep their place in the chain: before the instrument, which stays before the audio effects", "[ui][midi]")
{
    session::Session session;
    ui::EditorState editor;
    const auto choiceOf = [](const juce::String& type)
    {
        for (const auto& choice : ui::deviceActions::getNativeChoices())
            if (choice.type == type)
                return choice;
        FAIL("no such device");
        return ui::deviceActions::DeviceChoice{};
    };
    const auto types = [](const juce::ValueTree& owner)
    {
        juce::StringArray result;
        for (const auto& d : session::Session::getDevices(owner))
            result.add(session::device::getType(d));
        return result;
    };
    auto synth = session.addTrack(session::TrackKind::instrument, "Synth");
    using namespace ui::deviceActions;
    (void) addDevice(session, editor, synth, choiceOf("builtin:sampler"));
    (void) addDevice(session, editor, synth, choiceOf(Arpeggiator::kTypeId));
    (void) addDevice(session, editor, synth, choiceOf(ChordGenerator::kTypeId));
    (void) addDevice(session, editor, synth, choiceOf("builtin:eq"));
    CHECK(types(synth) == juce::StringArray{Arpeggiator::kTypeId, ChordGenerator::kTypeId, "builtin:sampler", "builtin:eq"});
    // A new instrument replaces the old one, after the MIDI effects.
    (void) addDevice(session, editor, synth, choiceOf("builtin:operator"));
    CHECK(types(synth) == juce::StringArray{Arpeggiator::kTypeId, ChordGenerator::kTypeId, "builtin:operator", "builtin:eq"});
    CHECK(session::device::getType(findInstrument(synth)) == "builtin:operator");
    CHECK(insertionIndex(synth, choiceOf(ScaleDevice::kTypeId)) == 2);
    CHECK(insertionIndex(synth, choiceOf("builtin:eq")) == -1);

    // Moves keep that order.
    const auto chain = session::Session::getDevices(synth);
    CHECK(canMoveDevice(chain, 0, 1));
    CHECK_FALSE(canMoveDevice(chain, 1, 2));
    CHECK_FALSE(canMoveDevice(chain, 3, 2));
    CHECK_FALSE(canMoveDevice(chain, 2, 0));
    CHECK_FALSE(canMoveDevice(chain, 0, 9));

    // On an audio track a MIDI effect gets a MIDI track of its own; no MIDI effects in insert loops.
    auto vocal = session.addTrack(session::TrackKind::audio, "Vocal");
    const auto arp = addDevice(session, editor, vocal, choiceOf(Arpeggiator::kTypeId));
    const auto midiTrack = session::Session::getDeviceOwner(arp);
    CHECK(session::track::getKind(midiTrack) == session::TrackKind::midi);
    CHECK(editor.getSelectedTrack() == session::track::getId(midiTrack));
    CHECK(session::Session::getDevices(vocal).empty());
    CHECK_FALSE(fitsInLoop(choiceOf(MidiFilter::kTypeId)));
    CHECK(isMidiEffectDevice(arp));
    CHECK_FALSE(isInstrumentDevice(arp));
}

TEST_CASE("MIDI effects: their editors name real controls and paint", "[ui][midi]")
{
    juce::MessageManager::getInstance();
    for (const auto* type : {Arpeggiator::kTypeId, ChordGenerator::kTypeId, ScaleDevice::kTypeId, MidiFilter::kTypeId})
    {
        auto processor = devices::createBuiltinDevice(type);
        REQUIRE(processor != nullptr);
        processor->prepareToPlay(kRate, kBlock);
        CHECK(processor->isMidiEffect());
        CHECK(processor->acceptsMidi());
        const auto* info = devices::findBuiltinDevice(type);
        REQUIRE(info != nullptr);
        CHECK(info->isMidiEffect);
        CHECK(info->category == "MIDI");
        int width = 0;
        auto editor = ui::createMidiEffectEditor(*processor, width);
        REQUIRE(editor != nullptr);
        auto* pages = dynamic_cast<ui::PagedEditor*>(editor.get());
        REQUIRE(pages != nullptr);
        CHECK(pages->getMissingParameters().isEmpty());
        editor->setSize(width, 200);
        for (std::size_t p = 0; p < pages->getNumPages(); ++p)
        {
            pages->showPage(p);
            (void) editor->createComponentSnapshot(editor->getLocalBounds());
        }
    }
}

TEST_CASE("MIDI effects never touch the heap while processing", "[devices][midi][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();
    for (const auto* type : {Arpeggiator::kTypeId, ChordGenerator::kTypeId, ScaleDevice::kTypeId, MidiFilter::kTypeId})
    {
        auto processor = devices::createBuiltinDevice(type);
        auto* effect = dynamic_cast<devices::MidiEffect*>(processor.get());
        REQUIRE(effect != nullptr);
        engine::ProcessorNode node(std::move(processor));
        TestPlayHead head;
        head.info.setBpm(120.0);
        head.info.setIsPlaying(true);
        node.prepare(kRate, kBlock, &head);
        juce::AudioBuffer<double> buffer(2, kBlock);
        juce::MidiBuffer midi;
        midi.ensureSize(4096);
        const auto before = rt::getRealtimeViolationCount();
        {
            const rt::ScopedRealtimeSection section;
            for (int block = 0; block < 200; ++block)
            {
                head.info.setPpqPosition(block * kBlock / kRate * 2.0);
                midi.clear();
                if (block % 20 == 0)
                    for (const int note : {48, 52, 55})
                    {
                        const juce::uint8 bytes[3] = {0x90, static_cast<juce::uint8>(note + block % 12), 100};
                        midi.addEvent(bytes, 3, 10);
                    }
                if (block % 20 == 10)
                    for (const int note : {48, 52, 55})
                    {
                        const juce::uint8 bytes[3] = {0x80, static_cast<juce::uint8>(note + (block - 10) % 12), 0};
                        midi.addEvent(bytes, 3, 10);
                    }
                node.process(buffer, kBlock, midi, nullptr);
            }
        }
        INFO(type);
        CHECK(rt::getRealtimeViolationCount() == before);
    }
}

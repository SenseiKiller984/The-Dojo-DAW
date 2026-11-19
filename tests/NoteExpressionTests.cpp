// Per-note expression (MPE): curves on notes; the compiler giving each note a channel of its own;
// the engine sending each note's pitch bend, pressure and slide there; Drift following them; MPE
// controllers recorded into the curves; and the piano roll's expression lanes.

#include "devices/DeviceRegistry.h"
#include "devices/DriftSynth.h"
#include "devices/dsp/VirtualAnalog.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "ui/EditorState.h"
#include "ui/components/ClipEditor.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <set>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;
using note::Expression;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

/** A test instrument that writes down every MIDI message it is sent, with its sample time. */
class MidiLog final : public juce::AudioProcessor
{
public:
    static constexpr const char* kTypeId = "test:midilog";
    struct Entry
    {
        long long sample = 0;
        std::uint8_t bytes[3]{};
        int size = 0;
    };
    MidiLog()
        : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
    }
    const juce::String getName() const override { return "MIDI Log"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        for (const auto meta : midi)
            if (const int count = count_.load(); count < static_cast<int>(entries_.size()) && meta.numBytes <= 3)
            {
                auto& entry = entries_[static_cast<std::size_t>(count)];
                entry.sample = processed_ + meta.samplePosition;
                entry.size = meta.numBytes;
                std::copy_n(meta.data, meta.numBytes, entry.bytes);
                count_.store(count + 1);
            }
        processed_ += buffer.getNumSamples();
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

    [[nodiscard]] std::vector<std::pair<long long, juce::MidiMessage>> messages() const
    {
        std::vector<std::pair<long long, juce::MidiMessage>> result;
        for (int i = 0; i < count_.load(); ++i)
        {
            const auto& entry = entries_[static_cast<std::size_t>(i)];
            result.emplace_back(entry.sample, juce::MidiMessage(entry.bytes, entry.size));
        }
        return result;
    }

private:
    std::array<Entry, 4096> entries_{};
    std::atomic<int> count_{0};
    long long processed_ = 0;
};

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    if (device::getType(device) == MidiLog::kTypeId)
        return std::make_unique<MidiLog>();
    auto processor = devices::createBuiltinDevice(device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

/** A session, its devices and an engine playing them (no audio device). */
struct Rig
{
    Rig()
        : engine(0)
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
        graph->serial = ++serial;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
        return copy;
    }

    /** Plays from the start for `seconds`; the left output. */
    std::vector<float> play(double seconds)
    {
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        std::vector<float> out;
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            engine.releaseRetiredGraphs();
            out.insert(out.end(), l.begin(), l.end());
        }
        return out;
    }

    template <typename P>
    P& processor(const juce::ValueTree& device)
    {
        auto node = devices.getNode(device);
        REQUIRE(node != nullptr);
        return dynamic_cast<P&>(node->getProcessor());
    }

    engine::AudioEngine engine;
    Session session;
    AudioAssetCache assets;
    DeviceManager devices;
    GraphBuilder builder;
    int serial = 0;
};

/** The energy of `x` at `frequency` (Goertzel), from `from` seconds for `length` seconds. */
double energyAt(const std::vector<float>& x, double frequency, double from, double length)
{
    const auto begin = static_cast<std::size_t>(from * kRate);
    const auto end = std::min(x.size(), begin + static_cast<std::size_t>(length * kRate));
    const double coefficient = 2.0 * std::cos(2.0 * juce::MathConstants<double>::pi * frequency / kRate);
    double s1 = 0.0, s2 = 0.0;
    for (auto i = begin; i < end; ++i)
    {
        const double s0 = x[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return (s1 * s1 + s2 * s2 - coefficient * s1 * s2) / static_cast<double>(std::max<std::size_t>(1, end - begin));
}

double semitonesOf(const juce::MidiMessage& bend)
{
    return (bend.getPitchWheelValue() - 8192) / 8192.0 * engine::kMpePitchBendRange;
}

juce::MouseEvent mouse(juce::Component& c, juce::Point<float> at, juce::Point<float> downAt, juce::ModifierKeys mods = {}, int clicks = 1)
{
    const auto now = juce::Time::getCurrentTime();
    const auto flags = mods.getRawFlags() | (mods.isPopupMenu() ? 0 : juce::ModifierKeys::leftButtonModifier);
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(flags),
                            juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                            juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                            juce::MouseInputSource::defaultTiltY, &c, &c, now, downAt, now, clicks, at != downAt);
}
} // namespace

TEST_CASE("Note expression: curves on notes, kept in range and order, read in between", "[mpe][session]")
{
    Session session;
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    auto clipTree = session.addMidiClip(keys, 0.0, 4.0);
    auto n = session.addNote(clipTree, 0.0, 2.0, 60);
    CHECK_FALSE(note::hasExpression(n));
    CHECK_THAT(note::expressionAt({}, 1.0), WithinAbs(0.0, 1e-12));

    session.beginTransaction("Draw");
    note::setExpression(n, Expression::pitch, {{1.0, 60.0}, {0.0, 0.0}, {0.5, 7.0}}, &session.getUndoManager());
    note::setExpression(n, Expression::pressure, {{0.0, 0.25}, {1.5, 2.0}}, &session.getUndoManager());
    const auto pitch = note::getExpression(n, Expression::pitch);
    REQUIRE(pitch.size() == 3);
    CHECK_THAT(pitch[0].beat, WithinAbs(0.0, 1e-9));
    CHECK_THAT(pitch[1].value, WithinAbs(7.0, 1e-9));
    CHECK_THAT(pitch[2].value, WithinAbs(48.0, 1e-9)); // (+-48 semitones at most)
    CHECK_THAT(note::getExpression(n, Expression::pressure)[1].value, WithinAbs(1.0, 1e-9));
    CHECK(note::hasExpression(n));
    CHECK(note::hasExpression(n, Expression::pressure));
    CHECK_FALSE(note::hasExpression(n, Expression::slide));

    // Straight lines between the points; held before the first and after the last.
    CHECK_THAT(note::expressionAt(pitch, 0.25), WithinAbs(3.5, 1e-9));
    CHECK_THAT(note::expressionAt(pitch, 0.75), WithinAbs(27.5, 1e-9));
    CHECK_THAT(note::expressionAt(pitch, 5.0), WithinAbs(48.0, 1e-9));
    CHECK_THAT(note::expressionAt({{0.5, 0.3}}, 0.0), WithinAbs(0.3, 1e-9));

    // Thinning keeps the corners of a recorded curve and drops the points on its straight runs.
    std::vector<note::ExpressionPoint> ramp;
    for (int i = 0; i <= 100; ++i)
        ramp.push_back({i * 0.01, i <= 50 ? i * 0.1 : 5.0}); // up to 5 semitones, then flat
    const auto thin = note::thinExpression(ramp, 0.01);
    REQUIRE(thin.size() == 3);
    CHECK_THAT(thin[1].beat, WithinAbs(0.5, 1e-9));
    CHECK_THAT(thin[1].value, WithinAbs(5.0, 1e-9));
    for (int i = 0; i <= 100; ++i)
        CHECK_THAT(note::expressionAt(thin, ramp[static_cast<std::size_t>(i)].beat), WithinAbs(ramp[static_cast<std::size_t>(i)].value, 0.011));
    // A jump (two points at one beat) is a corner: both ends stay.
    const auto steps = note::thinExpression({{0.0, 0.0}, {0.8, 0.0}, {0.8, 6.0}, {1.6, 6.0}, {1.6, 12.0}}, 0.01);
    CHECK(steps.size() == 5);
    CHECK_THAT(note::expressionAt(steps, 1.2), WithinAbs(6.0, 1e-9));

    // A copy of the note carries its curves; clearing one removes it; one undo step.
    const auto copy = n.createCopy();
    CHECK(note::getExpression(copy, Expression::pitch).size() == 3);
    note::setExpression(n, Expression::pitch, {}, &session.getUndoManager());
    CHECK_FALSE(note::hasExpression(n, Expression::pitch));
    REQUIRE(session.getUndoManager().undo());
    CHECK_FALSE(note::hasExpression(n));
}

TEST_CASE("Note expression makes a clip MPE: each note on a member channel of its own", "[mpe][engine]")
{
    Rig rig;
    auto keys = rig.session.addTrack(TrackKind::instrument, "Keys");
    auto plain = rig.session.addMidiClip(keys, 0.0, 2.0);
    rig.session.addNote(plain, 0.0, 1.0, 50);
    auto expressive = rig.session.addMidiClip(keys, 2.0, 4.0);
    auto a = rig.session.addNote(expressive, 0.0, 2.0, 60); // overlapping notes...
    auto b = rig.session.addNote(expressive, 1.0, 2.0, 64);
    rig.session.addNote(expressive, 3.0, 1.0, 67);            // ...then one after a has ended
    rig.session.addNote(expressive, 3.5, 1.0, 71);
    note::setExpression(a, Expression::pitch, {{0.0, 0.0}, {2.0, 12.0}}, nullptr);
    note::setExpression(b, Expression::slide, {{0.5, 0.75}}, nullptr);

    const auto graph = rig.publish();
    REQUIRE(graph->tracks.size() == 1);
    const auto& clips = graph->tracks[0].midiClips;
    REQUIRE(clips.size() == 2);
    CHECK_FALSE(clips[0].mpe);
    CHECK(clips[0].notes[0].channel == 1);
    REQUIRE(clips[1].mpe);
    const auto& notes = clips[1].notes;
    REQUIRE(notes.size() == 4);
    // Channels 2.. in turn: a free channel never used before goes first (a released note may
    // still ring on the old one).
    CHECK(notes[0].channel == 2);
    CHECK(notes[1].channel == 3);
    CHECK(notes[2].channel == 4);
    CHECK(notes[3].channel == 5);
    CHECK(notes[0].pitchCount == 2);
    CHECK(notes[1].slideCount == 1);
    CHECK(notes[2].pitchCount + notes[2].pressureCount + notes[2].slideCount == 0);
    CHECK_THAT(clips[1].expression[notes[0].pitchFirst + 1].value, WithinAbs(12.0f, 1e-6f));

    // More than fifteen notes at once share the channel whose note ends first.
    auto crowd = rig.session.addMidiClip(keys, 8.0, 4.0);
    for (int i = 0; i < 16; ++i)
        note::setExpression(rig.session.addNote(crowd, 0.0, 1.0 + i * 0.1, 40 + i), Expression::pressure, {{0.0, 0.5}}, nullptr);
    const auto crowded = rig.publish();
    const auto& sixteen = crowded->tracks[0].midiClips[2].notes;
    REQUIRE(sixteen.size() == 16);
    std::set<int> channels;
    for (std::size_t i = 0; i < 15; ++i)
        channels.insert(sixteen[i].channel);
    CHECK(channels.size() == 15);
    CHECK(sixteen[15].channel == sixteen[0].channel); // (the shortest held note's channel)
}

TEST_CASE("The engine sends each note's expression on its own channel", "[mpe][engine]")
{
    Rig rig;
    auto keys = rig.session.addTrack(TrackKind::instrument, "Keys");
    const auto log = rig.session.addDevice(keys, MidiLog::kTypeId, "Log");
    auto clipTree = rig.session.addMidiClip(keys, 0.0, 2.0); // 120 bpm: 4 beats
    auto bent = rig.session.addNote(clipTree, 0.0, 2.0, 60);
    auto still = rig.session.addNote(clipTree, 0.0, 2.0, 67);
    note::setExpression(bent, Expression::pitch, {{0.0, 0.0}, {2.0, 12.0}}, nullptr); // a ramp up an octave over a second
    note::setExpression(bent, Expression::pressure, {{0.0, 0.5}}, nullptr);
    note::setExpression(bent, Expression::slide, {{1.0, 0.25}, {1.5, 1.0}}, nullptr);
    (void) rig.publish();
    rig.play(1.2);

    const auto messages = rig.processor<MidiLog>(log).messages();
    std::map<int, int> channelOf; // note -> channel
    for (std::size_t i = 0; i < messages.size(); ++i)
    {
        const auto& m = messages[i].second;
        if (!m.isNoteOn())
            continue;
        channelOf[m.getNoteNumber()] = m.getChannel();
        // Its starting expression came first, on its channel, at the same sample.
        bool bend = false, pressure = false, slide = false;
        for (std::size_t j = 0; j < i; ++j)
            if (messages[j].first == messages[i].first && messages[j].second.getChannel() == m.getChannel())
            {
                bend = bend || messages[j].second.isPitchWheel();
                pressure = pressure || messages[j].second.isChannelPressure();
                slide = slide || (messages[j].second.isController() && messages[j].second.getControllerNumber() == 74);
            }
        CHECK(bend);
        CHECK(pressure);
        CHECK(slide);
    }
    REQUIRE(channelOf.size() == 2);
    const int bentChannel = channelOf[60];
    const int stillChannel = channelOf[67];
    CHECK(bentChannel != stillChannel);
    CHECK(bentChannel >= 2);
    CHECK(stillChannel >= 2);

    // The bent note's channel: bends rising with the ramp (each a step in time, never too many);
    // its pressure on the channel and as its key's aftertouch; its slide where it was drawn.
    std::vector<std::pair<long long, double>> bends;
    bool polyPressure = false;
    std::vector<std::pair<long long, int>> slides;
    for (const auto& [sample, m] : messages)
    {
        if (m.getChannel() == stillChannel)
            CHECK((m.isNoteOnOrOff() || sample == 0)); // (nothing moves the other note after it starts)
        if (m.getChannel() != bentChannel)
            continue;
        if (m.isPitchWheel())
            bends.emplace_back(sample, semitonesOf(m));
        if (m.isAftertouch() && m.getNoteNumber() == 60)
            polyPressure = m.getAfterTouchValue() == 64;
        if (m.isController() && m.getControllerNumber() == 74)
            slides.emplace_back(sample, m.getControllerValue());
    }
    CHECK(polyPressure);
    REQUIRE(bends.size() > 100);
    CHECK(bends.size() < 450); // (a step every 128 samples at most: 375 a second)
    CHECK_THAT(bends.front().second, WithinAbs(0.0, 1e-9));
    for (std::size_t i = 1; i < bends.size(); ++i)
    {
        CHECK(bends[i].second >= bends[i - 1].second);
        const double expected = std::min(12.0, static_cast<double>(bends[i].first) / kRate * 12.0); // (12 st a second)
        CHECK_THAT(bends[i].second, WithinAbs(expected, 0.1));
    }
    CHECK_THAT(bends.back().second, WithinAbs(12.0, 0.05));
    REQUIRE(slides.size() >= 3);
    CHECK(slides.front().second == 32); // (0.25 from the start)
    CHECK(slides.back().second == 127);
    CHECK(std::abs(static_cast<double>(slides.back().first) / kRate - 0.75) < 0.01); // (beat 1.5)

    // What the built-in instruments make of it: each note its own bend.
    devices::dsp::NoteExpressions expressions;
    expressions.setMpe(true, engine::kMpePitchBendRange, 2.0);
    for (const auto& [sample, m] : messages)
        if (sample <= static_cast<long long>(0.5 * kRate))
            expressions.handle(m);
    CHECK_THAT(expressions.bend(bentChannel), WithinAbs(6.0, 0.1)); // (half way up at 0.5 s)
    CHECK_THAT(expressions.bend(stillChannel), WithinAbs(0.0, 1e-9));
    CHECK_THAT(expressions.pressure(bentChannel, 60), WithinAbs(64.0 / 127.0, 1e-9));
}

TEST_CASE("Drift plays each note's own bend: one note of a chord glides, the other stays", "[mpe][devices]")
{
    Rig rig;
    auto keys = rig.session.addTrack(TrackKind::instrument, "Keys");
    rig.session.addDevice(keys, devices::DriftSynth::kTypeId, "Drift");
    auto clipTree = rig.session.addMidiClip(keys, 0.0, 2.0);
    auto bent = rig.session.addNote(clipTree, 0.0, 4.0, 57);  // A3, 220 Hz, held an octave up
    rig.session.addNote(clipTree, 0.0, 4.0, 52);              // E3, 164.8 Hz, as it is
    note::setExpression(bent, Expression::pitch, {{0.0, 12.0}}, nullptr);
    (void) rig.publish();
    const auto out = rig.play(1.0);

    const double a3 = energyAt(out, 220.0, 0.3, 0.6);
    const double a4 = energyAt(out, 440.0, 0.3, 0.6);
    const double e3 = energyAt(out, 164.81, 0.3, 0.6);
    const double e4 = energyAt(out, 329.63, 0.3, 0.6);
    CHECK(a4 > 20.0 * a3); // the bent note sounds an octave up...
    CHECK(e3 > 20.0 * energyAt(out, 196.0, 0.3, 0.6)); // ...the other where it was (energy at its pitch, not around it)
    CHECK(e3 > 0.1 * e4);
}

TEST_CASE("Recording an MPE controller: each note's channel expression becomes its curves", "[mpe][record]")
{
    test::TempFolder temp;
    juce::MessageManager::getInstance();
    engine::AudioEngine engine;
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    Project project([&controller](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                    [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); });
    engine.prepare(kRate, kBlock, 2, 2);
    auto& session = project.getSession();
    REQUIRE(project.saveAs(temp.dir.getChildFile("mpe.dojo")).wasOk());
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    track::setArmed(keys, true, nullptr);
    project.getCompiler().rebuildNow();

    Recorder recorder(project, controller);
    CHECK(recorder.getMpeInput()); // (on by default)
    CHECK(recorder.getMpeBendRange() == 48.0);

    // Plays what an MPE controller sends (with its bend range), block by block (256 samples,
    // 1/375 s), and returns the recorded clip.
    const auto perform = [&](double controllerRange)
    {
        REQUIRE(recorder.start().isEmpty());
        controller.locateSeconds(0.0);
        const auto bend = [controllerRange](int channel, double semitones)
        { return juce::MidiMessage::pitchWheel(channel, 8192 + juce::roundToInt(semitones / controllerRange * 8192.0)); };
        std::map<int, std::vector<juce::MidiMessage>> script;
        // Channel 2: its starting slide just before the note, then a bend up to 12 semitones and pressure.
        script[0] = {juce::MidiMessage::controllerEvent(2, 74, 64), juce::MidiMessage::noteOn(2, 60, 0.8f)};
        script[75] = {bend(2, 6.0)};
        script[150] = {bend(2, 12.0), juce::MidiMessage::channelPressureChange(2, 100)};
        // Channel 3: a note, its slide, and polyphonic aftertouch.
        script[37] = {juce::MidiMessage::noteOn(3, 64, 0.6f)};
        script[112] = {juce::MidiMessage::controllerEvent(3, 74, 100)};
        script[187] = {juce::MidiMessage::aftertouchChange(3, 64, 90)};
        // Channel 1 (the master channel): a bend for every note stays a controller of the clip.
        script[93] = {juce::MidiMessage::pitchWheel(1, 12000)};
        script[300] = {juce::MidiMessage::noteOff(2, 60), juce::MidiMessage::noteOff(3, 64)};

        std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
        const float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        for (int block = 0; block < 320; ++block)
        {
            if (const auto it = script.find(block); it != script.end())
                for (const auto& m : it->second)
                    (void) engine.getLiveMidiQueue().push(m);
            engine.process(inputs, 2, outputs, 2, kBlock);
        }
        recorder.stop();
        engine.process(inputs, 2, outputs, 2, kBlock);
        REQUIRE(test::pumpMessagesUntil([&] { return !recorder.isBusy(); }));
        const auto recorded = keys.getChildWithName(ids::midiClip);
        REQUIRE(recorded.isValid());
        return recorded;
    };
    struct Parts
    {
        juce::ValueTree n60, n64;
        std::vector<juce::ValueTree> controls;
        [[nodiscard]] int count(int controller, int channel) const
        {
            return static_cast<int>(std::count_if(controls.begin(), controls.end(), [=](const juce::ValueTree& c)
                                                  { return static_cast<int>(c.getProperty(ids::controller)) == controller &&
                                                           static_cast<int>(c.getProperty(ids::channel)) == channel; }));
        }
    };
    const auto partsOf = [](const juce::ValueTree& clip)
    {
        Parts parts;
        for (const auto& c : clip)
        {
            if (c.hasType(ids::note) && note::getNote(c) == 60)
                parts.n60 = c;
            if (c.hasType(ids::note) && note::getNote(c) == 64)
                parts.n64 = c;
            if (c.hasType(ids::control))
                parts.controls.push_back(c);
        }
        REQUIRE(parts.n60.isValid());
        REQUIRE(parts.n64.isValid());
        return parts;
    };
    const double beatsPerBlock = kBlock / kRate * 2.0; // (120 bpm)

    SECTION("MPE input on (the default): each note's own bend, pressure and slide")
    {
        const auto parts = partsOf(perform(48.0));
        const auto& n60 = parts.n60;
        const auto& n64 = parts.n64;

        // Note 60: its bend rises 0 -> 6 -> 12 at the blocks it was sent, its slide started at 64,
        // its pressure arrived later.
        const auto pitch = note::getExpression(n60, Expression::pitch);
        REQUIRE(pitch.size() >= 3);
        CHECK_THAT(note::expressionAt(pitch, 0.0), WithinAbs(0.0, 1e-6));
        CHECK_THAT(note::expressionAt(pitch, 100 * beatsPerBlock), WithinAbs(6.0, 0.02));
        CHECK_THAT(note::expressionAt(pitch, 200 * beatsPerBlock), WithinAbs(12.0, 0.02));
        CHECK(std::abs(note::expressionAt(pitch, 74 * beatsPerBlock)) < 0.2); // (the step came at block 75)
        const auto slide = note::getExpression(n60, Expression::slide);
        REQUIRE_FALSE(slide.empty());
        CHECK_THAT(note::expressionAt(slide, 0.0), WithinAbs(64.0 / 127.0, 1e-3));
        CHECK_THAT(note::expressionAt(note::getExpression(n60, Expression::pressure), 200 * beatsPerBlock), WithinAbs(100.0 / 127.0, 1e-3));

        // Note 64: no bend; its slide from 0 to 100; poly aftertouch as its pressure.
        CHECK_FALSE(note::hasExpression(n64, Expression::pitch));
        const double start64 = note::getStart(n64);
        const auto slide64 = note::getExpression(n64, Expression::slide);
        CHECK_THAT(note::expressionAt(slide64, 0.0), WithinAbs(0.0, 1e-6));
        CHECK_THAT(note::expressionAt(slide64, 120 * beatsPerBlock - start64), WithinAbs(100.0 / 127.0, 1e-3));
        CHECK_THAT(note::expressionAt(note::getExpression(n64, Expression::pressure), 200 * beatsPerBlock - start64), WithinAbs(90.0 / 127.0, 1e-3));

        // The master channel's bend stays a clip controller; nothing else became one.
        CHECK(parts.count(128, 1) == 1);
        CHECK(parts.controls.size() == 1);
    }

    SECTION("A controller with a 24-semitone bend range: its bends in semitones all the same")
    {
        recorder.setMpeInput(true, 24.0);
        const auto pitch = note::getExpression(partsOf(perform(24.0)).n60, Expression::pitch);
        CHECK_THAT(note::expressionAt(pitch, 100 * beatsPerBlock), WithinAbs(6.0, 0.02));
        CHECK_THAT(note::expressionAt(pitch, 200 * beatsPerBlock), WithinAbs(12.0, 0.02));
    }

    SECTION("MPE input off: the channels' bend, pressure and slide stay controllers on their channels")
    {
        recorder.setMpeInput(false, 48.0);
        const auto parts = partsOf(perform(48.0));
        CHECK_FALSE(note::hasExpression(parts.n60));
        CHECK(parts.count(128, 2) == 2);  // the two bends
        CHECK(parts.count(129, 2) == 1);  // channel pressure
        CHECK(parts.count(74, 2) == 1);   // the slide sent before the note
        CHECK(parts.count(74, 3) == 1);
        CHECK(parts.count(128, 1) == 1);
        // Polyphonic aftertouch is per note whatever the controller: the note's pressure.
        CHECK(note::hasExpression(parts.n64, Expression::pressure));
        CHECK_FALSE(note::hasExpression(parts.n64, Expression::slide));
    }
}

TEST_CASE("The piano roll's expression lanes: drawn under each note, ramps, erasing, clearing", "[mpe][ui][pianoroll]")
{
    juce::MessageManager::getInstance();
    Session session;
    ui::EditorState editor;
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    auto clipTree = session.addMidiClip(keys, 0.0, 4.0); // 8 beats
    auto first = session.addNote(clipTree, 0.0, 2.0, 60);
    auto second = session.addNote(clipTree, 4.0, 2.0, 64);

    ui::PianoRoll roll(session, editor);
    roll.setSize(900, 600);
    roll.setClip(clipTree);
    CHECK(roll.getExpressionLaneArea(Expression::pitch).isEmpty());
    const int gridHeightBefore = roll.getGridArea().getHeight();
    roll.setLowerLane(ui::PianoRoll::kNoteExpressionLane);
    const auto pitchLane = roll.getExpressionLaneArea(Expression::pitch);
    const auto pressureLane = roll.getExpressionLaneArea(Expression::pressure);
    const auto slideLane = roll.getExpressionLaneArea(Expression::slide);
    REQUIRE_FALSE(pitchLane.isEmpty());
    CHECK(pressureLane.getY() > pitchLane.getBottom() - 1);
    CHECK(slideLane.getY() > pressureLane.getBottom() - 1);
    CHECK(roll.getGridArea().getHeight() < gridHeightBefore); // (the three lanes take more room)

    // Draw the first note's pitch from no bend up to the top of the lane (+12 st by default).
    const float x0 = roll.getBeatX(0.2);
    const float x1 = roll.getBeatX(1.8);
    const float yMid = static_cast<float>(pitchLane.getCentreY());
    const float yTop = static_cast<float>(pitchLane.getY()) + 5.0f;
    roll.mouseDown(mouse(roll, {x0, yMid}, {x0, yMid}));
    for (int step = 1; step <= 8; ++step)
    {
        const float t = static_cast<float>(step) / 8.0f;
        roll.mouseDrag(mouse(roll, {x0 + (x1 - x0) * t, yMid + (yTop - yMid) * t}, {x0, yMid}));
    }
    roll.mouseUp(mouse(roll, {x1, yTop}, {x0, yMid}));
    const auto drawn = note::getExpression(first, Expression::pitch);
    REQUIRE(drawn.size() >= 9);
    CHECK_THAT(drawn.front().value, WithinAbs(0.0, 1e-9)); // (it starts unbent)
    CHECK_THAT(note::expressionAt(drawn, 1.8), WithinAbs(12.0, 0.05));
    CHECK(note::expressionAt(drawn, 1.0) > 4.0);
    CHECK_FALSE(note::hasExpression(second)); // (only the note under the mouse)
    REQUIRE(session.getUndoManager().undo()); // (one step)
    CHECK_FALSE(note::hasExpression(first));
    REQUIRE(session.getUndoManager().redo());

    // Shift-drag: a straight ramp of pressure on the second note.
    const float p0 = roll.getBeatX(4.0);
    const float p1 = roll.getBeatX(6.0);
    const float pBottom = static_cast<float>(pressureLane.getBottom()) - 5.0f;
    const float pTop = static_cast<float>(pressureLane.getY()) + 5.0f;
    const auto shift = juce::ModifierKeys(juce::ModifierKeys::shiftModifier);
    roll.mouseDown(mouse(roll, {p0 + 1.0f, pBottom}, {p0 + 1.0f, pBottom}, shift));
    roll.mouseDrag(mouse(roll, {p1 - 1.0f, pTop}, {p0 + 1.0f, pBottom}, shift));
    roll.mouseUp(mouse(roll, {p1 - 1.0f, pTop}, {p0 + 1.0f, pBottom}, shift));
    const auto ramp = note::getExpression(second, Expression::pressure);
    REQUIRE(ramp.size() <= 3); // (the ramp's two ends, after the neutral start)
    CHECK_THAT(note::expressionAt(ramp, 0.02), WithinAbs(0.0, 0.02));
    CHECK_THAT(note::expressionAt(ramp, 1.0), WithinAbs(0.5, 0.02));
    CHECK_THAT(note::expressionAt(ramp, 1.98), WithinAbs(1.0, 0.02));

    // Right-click erases the points under the mouse; a double-click clears the curve.
    const float xErase = roll.getBeatX(1.0);
    const auto popup = juce::ModifierKeys(juce::ModifierKeys::rightButtonModifier | juce::ModifierKeys::popupMenuClickModifier);
    roll.mouseDown(mouse(roll, {xErase, yMid}, {xErase, yMid}, popup));
    roll.mouseUp(mouse(roll, {xErase, yMid}, {xErase, yMid}, popup));
    const auto erased = note::getExpression(first, Expression::pitch);
    CHECK(erased.size() < drawn.size());
    for (const auto& p : erased)
        CHECK(std::abs(p.beat - 1.0) > 0.05);
    roll.mouseDoubleClick(mouse(roll, {x0, yMid}, {x0, yMid}, {}, 2));
    CHECK_FALSE(note::hasExpression(first, Expression::pitch));
    CHECK(note::hasExpression(second, Expression::pressure)); // (the other note keeps its own)

    // The pitch lane's scale: +-12, 24, 48, 2.
    CHECK_THAT(roll.getPitchLaneRange(), WithinAbs(12.0, 1e-9));
    roll.mouseDown(mouse(roll, {10.0f, yMid}, {10.0f, yMid}));
    CHECK_THAT(roll.getPitchLaneRange(), WithinAbs(24.0, 1e-9));

    // Painting: the lanes and a bent note's curve on the grid.
    note::setExpression(first, Expression::pitch, {{0.0, 0.0}, {1.0, 7.0}}, nullptr);
    juce::Image image(juce::Image::ARGB, 900, 600, true);
    juce::Graphics g(image);
    roll.paintEntireComponent(g, true);
}

// The tempo map: tempo changes, linear and curved ramps, time signatures; the session's tempo map
// (edits, undo, files, MIDI clips keeping their bars); the engine following it (MIDI notes on a ramp,
// the play head's tempo, signature and bars, looped MIDI clips); the Click counting signature beats.

#include "core/TempoMap.h"
#include "devices/Click.h"
#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/AudioAssetCache.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"
#include "DeviceTestSupport.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <cmath>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using time::TempoMap;
using time::TempoPoint;
using time::TempoShape;

namespace
{
/** Seconds to `beat` by integrating 60 / tempo in tiny steps (the reference). */
double integrate(const std::function<double(double)>& bpmAt, double beat)
{
    constexpr int steps = 200'000;
    double seconds = 0.0;
    const double h = beat / steps;
    for (int i = 0; i < steps; ++i)
        seconds += 60.0 / bpmAt((i + 0.5) * h) * h;
    return seconds;
}
} // namespace

TEST_CASE("Tempo map: changes, linear ramps and curves in closed form, both ways", "[tempo][map]")
{
    // Constant: 120 bpm is 2 beats a second, both ways, also before the start.
    const TempoMap constant;
    CHECK(constant.isConstant());
    CHECK_THAT(constant.secondsAt(8.0), WithinAbs(4.0, 1e-12));
    CHECK_THAT(constant.beatAt(3.0), WithinAbs(6.0, 1e-12));
    CHECK_THAT(constant.secondsAt(-2.0), WithinAbs(-1.0, 1e-12));
    CHECK_THAT(constant.bpmAt(100.0), WithinAbs(120.0, 1e-12));

    // A jump: 120 until beat 8 (4 s), then 60 (a beat a second).
    const TempoMap jump({{0.0, 120.0}, {8.0, 60.0}}, {});
    CHECK_FALSE(jump.isConstant());
    CHECK_THAT(jump.secondsAt(8.0), WithinAbs(4.0, 1e-12));
    CHECK_THAT(jump.secondsAt(10.0), WithinAbs(6.0, 1e-12));
    CHECK_THAT(jump.beatAt(6.0), WithinAbs(10.0, 1e-12));
    CHECK_THAT(jump.bpmAtBeat(7.999), WithinAbs(120.0, 1e-12));
    CHECK_THAT(jump.bpmAtBeat(8.0), WithinAbs(60.0, 1e-12));
    CHECK(jump.pointIndexAt(7.5) == 0);
    CHECK(jump.pointIndexAt(8.0) == 1);

    // A linear ramp, 120 -> 60 over 8 beats: the tempo moves evenly per beat; the time is
    // 60 / k * ln(t1 / t0) with k = -7.5 bpm a beat; the exact integral agrees.
    const TempoMap ramp({{0.0, 120.0, TempoShape::linear}, {8.0, 60.0}}, {});
    CHECK_THAT(ramp.bpmAtBeat(4.0), WithinAbs(90.0, 1e-12));
    const double rampEnd = 60.0 / -7.5 * std::log(0.5);
    CHECK_THAT(ramp.secondsAt(8.0), WithinAbs(rampEnd, 1e-12));
    CHECK_THAT(ramp.secondsAt(5.0), WithinAbs(integrate([](double b) { return 120.0 - 7.5 * b; }, 5.0), 1e-7));
    CHECK_THAT(ramp.secondsAt(10.0), WithinAbs(rampEnd + 2.0, 1e-12)); // (then 60 bpm)
    for (double beat = 0.0; beat < 12.0; beat += 0.37)
        CHECK_THAT(ramp.beatAt(ramp.secondsAt(beat)), WithinAbs(beat, 1e-9));
    CHECK_THAT(ramp.bpmAt(ramp.secondsAt(2.0)), WithinAbs(105.0, 1e-9));

    // Curves: late (bend > 0) holds the start tempo longer, early (< 0) leaves it sooner; their
    // time stays within a fraction of a millisecond of the exact integral, monotonic both ways.
    for (const double bend : {-0.7, -0.3, 0.4, 0.9})
    {
        const TempoMap curve({{0.0, 80.0, TempoShape::curve, bend}, {16.0, 160.0}}, {});
        const auto exact = [bend](double b) { return 80.0 + 80.0 * time::tempoCurve(b / 16.0, bend); };
        CHECK_THAT(curve.bpmAtBeat(8.0), WithinAbs(exact(8.0), 0.5));
        if (bend > 0.0)
            CHECK(curve.bpmAtBeat(8.0) < 120.0);
        else
            CHECK(curve.bpmAtBeat(8.0) > 120.0);
        CHECK_THAT(curve.secondsAt(16.0), WithinAbs(integrate(exact, 16.0), 5.0e-4));
        CHECK_THAT(curve.secondsAt(6.5), WithinAbs(integrate(exact, 6.5), 5.0e-4));
        double last = -1.0;
        for (double beat = 0.0; beat <= 20.0; beat += 0.25)
        {
            const double s = curve.secondsAt(beat);
            CHECK(s > last);
            CHECK_THAT(curve.beatAt(s), WithinAbs(beat, 1e-9));
            last = s;
        }
    }
    CHECK_THAT(time::tempoCurve(0.5, 0.0), WithinAbs(0.5, 1e-12));
    CHECK(time::tempoCurve(0.5, 1.0) < 0.01);
    CHECK(time::tempoCurve(0.5, -1.0) > 0.9);

    // Points in any order, a missing start (the first tempo holds from 0), the later of two at a beat.
    const TempoMap messy({{12.0, 90.0}, {4.0, 100.0}, {4.0, 140.0}}, {});
    REQUIRE(messy.getPoints().size() == 3);
    CHECK_THAT(messy.getPoints()[0].bpm, WithinAbs(140.0, 1e-12));
    CHECK_THAT(messy.getPoints()[1].bpm, WithinAbs(140.0, 1e-12));
    CHECK_THAT(messy.secondsAt(4.0), WithinAbs(4.0 * 60.0 / 140.0, 1e-12));
}

TEST_CASE("Tempo map: time signatures make the bars, in their own beat units", "[tempo][map]")
{
    // 4/4 for bars 1-2, 3/4 for bars 3-4, 6/8 from bar 5.
    const TempoMap map({{0.0, 120.0}}, {{2, {3, 4}}, {4, {6, 8}}});
    CHECK(map.signatureOfBar(0).numerator == 4);
    CHECK(map.signatureOfBar(3).numerator == 3);
    CHECK(map.signatureOfBar(9).denominator == 8);
    CHECK_THAT(map.beatOfBar(2), WithinAbs(8.0, 1e-12));
    CHECK_THAT(map.beatOfBar(3), WithinAbs(11.0, 1e-12));
    CHECK_THAT(map.beatOfBar(4), WithinAbs(14.0, 1e-12));
    CHECK_THAT(map.beatOfBar(5), WithinAbs(17.0, 1e-12)); // (6/8: 3 quarter notes)
    CHECK_THAT(map.barLength(4), WithinAbs(3.0, 1e-12));
    CHECK(map.barAt(10.99) == 2);
    CHECK(map.barAt(11.0) == 3);
    CHECK(map.barAt(16.9) == 4);
    CHECK(map.barAt(-0.5) == -1);

    // Bar | beat | tick: beats are eighths in 6/8 (480 ticks each).
    auto bbt = map.toBarsBeatsTicks(15.5);
    CHECK(bbt.bar == 5);
    CHECK(bbt.beat == 4);
    CHECK(bbt.tick == 0);
    bbt = map.toBarsBeatsTicks(15.75);
    CHECK(bbt.beat == 4);
    CHECK(bbt.tick == 240);
    bbt = map.toBarsBeatsTicks(12.5);
    CHECK(bbt.bar == 4);
    CHECK(bbt.beat == 2);
    CHECK(bbt.tick == 480);
    for (double beat = 0.0; beat < 30.0; beat += 0.125)
        CHECK_THAT(map.fromBarsBeatsTicks(map.toBarsBeatsTicks(beat)), WithinAbs(beat, 1e-9));
}

TEST_CASE("Session tempo map: changes, ramps and signatures; undo; files; MIDI clips keep their bars", "[tempo][session]")
{
    test::TempFolder temp;
    session::Session session;
    CHECK(session.getTempoMap().isConstant());

    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto midi = session.addMidiClip(keys, 4.0, 2.0); // bar 5 at 120 bpm, a bar long
    auto locked = session.addMidiClip(keys, 8.0, 1.0);
    session::clip::setTimeLocked(locked, true, nullptr);
    auto vox = session.addTrack(session::TrackKind::audio, "Vox");
    auto audio = session.addClip(vox, temp.dir.getChildFile("missing.wav"), 4.0, 2.0, 0.0);

    // Halving the start tempo: the MIDI clip stays on bar 5 (now at 8 s, two bars of 2 s... one bar
    // of 4 s), the audio clip stays at 4 s, the time-locked clip stays put. One undo step.
    session.beginTransaction("Tempo");
    session.setTempo(60.0);
    CHECK_THAT(session::clip::getStart(midi), WithinAbs(8.0, 1e-9));
    CHECK_THAT(session::clip::getLength(midi), WithinAbs(4.0, 1e-9));
    CHECK_THAT(session::clip::getStart(audio), WithinAbs(4.0, 1e-12));
    CHECK_THAT(session::clip::getStart(locked), WithinAbs(8.0, 1e-12));
    REQUIRE(session.getUndoManager().undo());
    CHECK_THAT(session.getTempo(), WithinAbs(120.0, 1e-12));
    CHECK_THAT(session::clip::getStart(midi), WithinAbs(4.0, 1e-9));

    // A ramp from bar 3 (beat 8) to 60 bpm at bar 5 (beat 16), and 3/4 from bar 5.
    session.beginTransaction("Map");
    auto rampStart = session.addTempoChange(8.0, 120.0, time::TempoShape::linear);
    auto rampEnd = session.addTempoChange(16.0, 60.0);
    auto threeFour = session.addSignatureChange(5, 3, 4);
    CHECK(session.getTempoChanges().size() == 2);
    CHECK(session.getSignatureChanges().size() == 1);
    const auto& map = session.getTempoMap();
    CHECK_THAT(map.bpmAtBeat(12.0), WithinAbs(90.0, 1e-9));
    CHECK(map.signatureOfBar(4).numerator == 3);
    const double bar5 = 4.0 + 60.0 / -7.5 * std::log(0.5); // 4 s to beat 8, then the ramp
    CHECK_THAT(session.secondsAtBeat(16.0), WithinAbs(bar5, 1e-9));
    // The MIDI clip at bar 2 (4 s... beat 8) - it was at 4 s = beat 8: the ramp starts there.
    CHECK_THAT(session::clip::getStart(midi), WithinAbs(4.0, 1e-9));
    CHECK_THAT(session.beatAtSeconds(session::clip::getEnd(midi)), WithinAbs(12.0, 1e-9)); // (still 4 beats)
    CHECK_THAT(session.getTempoAt(bar5 + 1.0), WithinAbs(60.0, 1e-9));
    CHECK(session.getSignatureAt(bar5 + 0.5).numerator == 3);

    // setTempoAt edits the point in effect; setSignatureAt the run the position is in.
    session.setTempoAt(bar5 + 2.0, 72.0);
    CHECK_THAT(static_cast<double>(rampEnd.getProperty(session::ids::bpm)), WithinAbs(72.0, 1e-12));
    session.setTempoAt(1.0, 100.0);
    CHECK_THAT(session.getTempo(), WithinAbs(100.0, 1e-12));
    session.setSignatureAt(bar5 + 3.0, 7, 8);
    CHECK(static_cast<int>(threeFour.getProperty(session::ids::numerator)) == 7);
    session.setSignatureAt(0.5, 2, 4);
    REQUIRE(session.getSignatureChanges().size() == 2);
    CHECK(session.getTempoMap().signatureOfBar(0).numerator == 2);

    // Moving and reshaping a change; the start shape.
    session.setTempoChange(rampStart, 6.0, 110.0);
    session.setTempoChangeShape(rampStart, time::TempoShape::curve, 0.5);
    CHECK(session.getTempoMap().getPoints()[1].shape == time::TempoShape::curve);
    CHECK_THAT(session.getTempoMap().getPoints()[1].beat, WithinAbs(6.0, 1e-12));
    session.setStartTempoShape(time::TempoShape::linear, 0.0);
    CHECK(session.getStartTempoShape() == time::TempoShape::linear);

    // The file keeps it all.
    const auto file = temp.dir.getChildFile("tempo.dojo");
    REQUIRE(session.save(file).wasOk());
    session::Session loaded;
    REQUIRE(loaded.load(file).wasOk());
    const auto saved = session.getTempoMapShared(); // (a reference would go with the next edit)
    const auto& a = *saved;
    const auto& b = loaded.getTempoMap();
    REQUIRE(a.getPoints().size() == b.getPoints().size());
    for (double beat = 0.0; beat < 40.0; beat += 0.5)
        CHECK_THAT(b.secondsAt(beat), WithinAbs(a.secondsAt(beat), 1e-9));
    CHECK(b.signatureOfBar(6).numerator == 7);

    // Removing; undo brings everything back (the clips included); clearing.
    session.beginTransaction("Remove");
    session.removeTempoChange(rampEnd);
    session.removeSignatureChange(threeFour);
    CHECK(session.getTempoChanges().size() == 1);
    REQUIRE(session.getUndoManager().undo());
    CHECK(session.getTempoChanges().size() == 2);
    CHECK_THAT(session.getTempoMap().secondsAt(30.0), WithinAbs(a.secondsAt(30.0), 1e-9));
    session.beginTransaction("Clear");
    session.clearTempoMap();
    CHECK(session.getTempoChanges().empty());
    CHECK(session.getSignatureChanges().empty());
    CHECK_THAT(session.getTempoMap().secondsAt(10.0), WithinAbs(6.0, 1e-9)); // (100 bpm)
}

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

/** A test instrument that writes down the sample of every note-on it is sent. */
class NoteLog final : public juce::AudioProcessor
{
public:
    static constexpr const char* kTypeId = "test:notelog";
    NoteLog()
        : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
    }
    const juce::String getName() const override { return "Note Log"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        buffer.clear();
        for (const auto meta : midi)
            if (meta.getMessage().isNoteOn() && count_ < static_cast<int>(ons_.size()))
                ons_[static_cast<std::size_t>(count_++)] = processed_ + meta.samplePosition;
        if (auto* head = getPlayHead())
            if (const auto position = head->getPosition())
            {
                bpm_ = position->getBpm().orFallback(0.0);
                ppq_ = position->getPpqPosition().orFallback(0.0);
                barStart_ = position->getPpqPositionOfLastBarStart().orFallback(0.0);
                if (const auto s = position->getTimeSignature())
                    signature_ = {s->numerator, s->denominator};
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

    [[nodiscard]] std::vector<long long> noteOns() const { return {ons_.begin(), ons_.begin() + count_}; }
    void clear() { count_ = 0; }
    double bpm_ = 0.0, ppq_ = 0.0, barStart_ = 0.0;
    std::pair<int, int> signature_{0, 0};

private:
    std::array<long long, 1024> ons_{};
    int count_ = 0;
    long long processed_ = 0;
};

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    if (session::device::getType(device) == NoteLog::kTypeId)
        return std::make_unique<NoteLog>();
    auto processor = devices::createBuiltinDevice(session::device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

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

    void publish()
    {
        auto graph = builder.build(session, assets, &devices);
        graph->serial = ++serial;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
    }

    void play(double from, double seconds)
    {
        REQUIRE(engine.post(engine::command::Locate{static_cast<SampleCount>(std::llround(from * kRate))}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            engine.releaseRetiredGraphs();
        }
        REQUIRE(engine.post(engine::command::SetPlaying{false}));
        engine.process(nullptr, 0, channels, 2, kBlock);
    }

    NoteLog& log(const juce::ValueTree& track)
    {
        auto node = devices.findNode(session::device::getId(track.getChildWithName(session::ids::device)));
        REQUIRE(node != nullptr);
        return static_cast<NoteLog&>(node->getProcessor());
    }

    session::Session session;
    session::AudioAssetCache assets;
    engine::AudioEngine engine;
    session::DeviceManager devices;
    session::GraphBuilder builder;
    std::uint64_t serial = 0;
};
} // namespace

TEST_CASE("The engine follows the tempo map: notes on a ramp, the play head's tempo, signature and bars", "[tempo][engine]")
{
    Rig rig;
    auto& session = rig.session;
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    session.addDevice(keys, NoteLog::kTypeId, "Log", 0);
    // A note on every beat for 16 beats, in a clip from the start.
    auto clip = session.addMidiClip(keys, 0.0, 30.0);
    for (int beat = 0; beat < 16; ++beat)
        session.addNote(clip, beat, 0.25, 60 + beat % 12);
    // 120 bpm ramping to 60 over beats 4..12, then 60; 3/4 from bar 4 (beat 12).
    session.addTempoChange(4.0, 120.0, time::TempoShape::linear);
    session.addTempoChange(12.0, 60.0);
    session.addSignatureChange(4, 3, 4);
    session::clip::setLength(clip, 30.0, nullptr);
    rig.publish();

    rig.play(0.0, 13.0);
    const auto ons = rig.log(keys).noteOns();
    REQUIRE(ons.size() == 16);
    const auto& map = session.getTempoMap();
    for (int beat = 0; beat < 16; ++beat)
        CHECK(std::llabs(ons[static_cast<std::size_t>(beat)] - std::llround(map.secondsAt(beat) * kRate)) <= 1);
    // (the ramp's beats come later and later)
    CHECK(ons[11] - ons[10] > ons[5] - ons[4]);

    // The play head at a block in the ramp and in the 3/4 bars.
    const double inRamp = map.secondsAt(8.0);
    rig.play(inRamp, static_cast<double>(kBlock) / kRate);
    auto& log = rig.log(keys);
    CHECK_THAT(log.bpm_, WithinAbs(90.0, 0.1));
    CHECK_THAT(log.ppq_, WithinAbs(8.0, 0.01));
    CHECK(log.signature_ == std::pair<int, int>{4, 4});
    CHECK_THAT(log.barStart_, WithinAbs(8.0, 1e-9));
    rig.play(map.secondsAt(13.5), static_cast<double>(kBlock) / kRate);
    CHECK_THAT(log.bpm_, WithinAbs(60.0, 1e-6));
    CHECK(log.signature_ == std::pair<int, int>{3, 4});
    CHECK_THAT(log.barStart_, WithinAbs(12.0, 1e-9));
    rig.play(map.secondsAt(15.2), static_cast<double>(kBlock) / kRate);
    CHECK_THAT(log.barStart_, WithinAbs(15.0, 1e-9));
}

TEST_CASE("A looped MIDI clip keeps its loop in bars through a tempo change", "[tempo][engine]")
{
    Rig rig;
    auto& session = rig.session;
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    session.addDevice(keys, NoteLog::kTypeId, "Log", 0);
    // A one-bar loop (2 s at 120) with a note on beat 0, looped over 4 bars; 60 bpm from bar 3.
    auto clip = session.addMidiClip(keys, 0.0, 2.0);
    session.addNote(clip, 0.0, 0.5, 60);
    session::clip::setLength(clip, 8.0, nullptr);
    session::clip::setLoop(clip, true, 2.0, nullptr);
    session.addTempoChange(8.0, 60.0);
    // (the clip is musical: it now runs to bar 5, which is 4 + 8 = 12 s)
    CHECK_THAT(session::clip::getEnd(clip), WithinAbs(12.0, 1e-9));
    rig.publish();
    rig.play(0.0, 12.5);
    const auto ons = rig.log(keys).noteOns();
    REQUIRE(ons.size() == 4);
    CHECK(std::llabs(ons[0] - 0) <= 1);
    CHECK(std::llabs(ons[1] - std::llround(2.0 * kRate)) <= 1);
    CHECK(std::llabs(ons[2] - std::llround(4.0 * kRate)) <= 1); // bar 3 at 60 bpm: 4 s long
    CHECK(std::llabs(ons[3] - std::llround(8.0 * kRate)) <= 1);
}

namespace
{
struct MapTransport final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }
    PositionInfo info;
};

/** The click over `seconds` with a transport following `map` (as the engine's play head does). */
std::vector<double> runClick(devices::Click& click, engine::ProcessorNode& node, const TempoMap& map, double seconds)
{
    MapTransport transport;
    node.prepare(test::devices::kRate, test::devices::kBlock, nullptr);
    click.setPlayHead(&transport);
    std::vector<double> out;
    juce::AudioBuffer<double> buffer(2, test::devices::kBlock);
    juce::MidiBuffer midi;
    for (int done = 0; done < static_cast<int>(seconds * test::devices::kRate); done += test::devices::kBlock)
    {
        const double t = done / test::devices::kRate;
        const double ppq = map.beatAt(t);
        const auto s = map.signatureAt(ppq);
        transport.info.setIsPlaying(true);
        transport.info.setBpm(map.bpmAtBeat(ppq));
        transport.info.setPpqPosition(ppq);
        transport.info.setTimeInSeconds(t);
        transport.info.setPpqPositionOfLastBarStart(map.beatOfBar(map.barAt(ppq)));
        transport.info.setTimeSignature(juce::AudioPlayHead::TimeSignature{s.numerator, s.denominator});
        buffer.clear();
        node.process(buffer, test::devices::kBlock, midi, nullptr);
        for (int i = 0; i < test::devices::kBlock; ++i)
            out.push_back(buffer.getSample(0, i));
    }
    click.setPlayHead(nullptr);
    return out;
}

std::vector<std::size_t> onsets(const std::vector<double>& x)
{
    std::vector<std::size_t> result;
    std::size_t quietFor = 1000;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        if (std::abs(x[i]) > 1.0e-3 && quietFor >= 200)
            result.push_back(i);
        quietFor = std::abs(x[i]) > 1.0e-3 ? 0 : quietFor + 1;
    }
    return result;
}

double peakAround(const std::vector<double>& x, std::size_t at)
{
    double peak = 0.0;
    for (std::size_t i = at; i < std::min(x.size(), at + 2400); ++i)
        peak = std::max(peak, std::abs(x[i]));
    return peak;
}
} // namespace

TEST_CASE("The Click counts the signature's beats: eighths in 6/8, the downbeat of each bar accented", "[tempo][click]")
{
    auto [node, click] = test::devices::makeNode<devices::Click>();
    // One bar of 2/4 (two beats), then 6/8 at 120 bpm (eighths 0.25 s apart).
    const TempoMap map({{0.0, 120.0}}, {{0, {2, 4}}, {1, {6, 8}}});
    const auto out = runClick(*click, *node, map, 1.0 + 3.05);
    const auto starts = onsets(out);
    REQUIRE(starts.size() == 2 + 12 + 1);
    // 2/4: at 0 and 0.5 s; then eighths from 1 s.
    CHECK(starts[1] - 24'000 <= 1);
    for (std::size_t k = 0; k < 13; ++k)
        CHECK(std::llabs(static_cast<long long>(starts[2 + k]) - std::llround((1.0 + 0.25 * static_cast<double>(k)) * 48'000.0)) <= 1);
    // Accents on bar starts only: 0 s, 1 s (6/8 bar 1), 2.5 s (bar 2).
    const double accent = peakAround(out, starts[0]);
    const double plain = peakAround(out, starts[1]);
    CHECK(accent > plain * 1.5);
    CHECK_THAT(peakAround(out, starts[2]), WithinAbs(accent, 1e-3));
    CHECK_THAT(peakAround(out, starts[3]), WithinAbs(plain, 1e-3));
    CHECK_THAT(peakAround(out, starts[8]), WithinAbs(accent, 1e-3));
    CHECK_THAT(peakAround(out, starts[9]), WithinAbs(plain, 1e-3));
}

TEST_CASE("The arranger's tempo track: points to drag, changes and ramps to add, signatures, and the grid", "[tempo][ui]")
{
    struct MessageThread
    {
        MessageThread() { juce::MessageManager::getInstance(); }
    } messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&controller](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    ui::EditorState editor;
    auto& session = project.getSession();
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    ui::ArrangerView arranger(project, controller, editor);
    arranger.setSize(1600, 700);

    const auto event = [&arranger](juce::Point<float> p, juce::Point<float> down, juce::ModifierKeys extra = {}, int clicks = 1)
    {
        const auto now = juce::Time::getCurrentTime();
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | extra.getRawFlags());
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &arranger, &arranger, now, down, now,
                                clicks, p != down);
    };

    // Hidden by default; shown, it pushes the tracks down by its height.
    CHECK(arranger.getTempoTrackArea().isEmpty());
    const int headerY = arranger.getTrackHeaderArea(keys).getY();
    editor.setShowingTempoTrack(true);
    const auto area = arranger.getTempoTrackArea();
    REQUIRE_FALSE(area.isEmpty());
    CHECK(arranger.getTrackHeaderArea(keys).getY() == headerY + area.getHeight());
    CHECK(arranger.getSignatureStripArea().getBottom() == arranger.getTempoCurveArea().getY() - 7);
    {
        juce::Image image(juce::Image::RGB, 1600, 700, true);
        juce::Graphics g(image);
        arranger.paintEntireComponent(g, false); // (paints with the tempo track)
    }

    // Double-click in the curve: a tempo change on the grid (beats) with the tempo there.
    const auto& map0 = session.getTempoMap();
    const float xBar3 = arranger.getTempoPointPosition(0).x + static_cast<float>(map0.secondsAt(8.02) * 40.0); // ~bar 3 at 40 px/s
    const float y = arranger.tempoToY(90.0);
    arranger.mouseDoubleClick(event({xBar3, y}, {xBar3, y}, {}, 2));
    REQUIRE(session.getTempoChanges().size() == 1);
    auto change = session.getTempoChanges().front();
    CHECK_THAT(static_cast<double>(change.getProperty(session::ids::beat)), WithinAbs(8.0, 1e-9));
    CHECK_THAT(static_cast<double>(change.getProperty(session::ids::bpm)), WithinAbs(90.0, 1.0));
    CHECK(arranger.tempoPointAt(arranger.getTempoPointPosition(1)) == 1);

    // Dragging it: along the bars (on the grid) and down to a slower tempo; one undo step.
    const auto from = arranger.getTempoPointPosition(1);
    const auto to = juce::Point<float>(from.x + static_cast<float>((session.getTempoMap().secondsAt(12.0) - session.getTempoMap().secondsAt(8.0)) * 40.0),
                                       arranger.tempoToY(85.0));
    arranger.mouseDown(event(from, from));
    arranger.mouseDrag(event((from + to) * 0.5f, from));
    arranger.mouseDrag(event(to, from));
    arranger.mouseUp(event(to, from));
    CHECK_THAT(static_cast<double>(change.getProperty(session::ids::beat)), WithinAbs(12.0, 1e-9));
    CHECK_THAT(static_cast<double>(change.getProperty(session::ids::bpm)), WithinAbs(85.0, 1.0));
    CHECK(session.getUndoManager().getUndoDescription() == "Move Tempo Change");
    // The start tempo only goes up and down.
    const auto start = arranger.getTempoPointPosition(0);
    arranger.mouseDown(event(start, start));
    arranger.mouseDrag(event({start.x + 100.0f, arranger.tempoToY(100.0)}, start));
    arranger.mouseUp(event({start.x + 100.0f, arranger.tempoToY(100.0)}, start));
    CHECK_THAT(session.getTempo(), WithinAbs(100.0, 1.0));

    // The point's menu: a ramp to the next (here: the last point, so only Jump applies there) and
    // the start's ramp; the menu on empty space adds a change or a signature.
    auto itemsOf = [](const juce::PopupMenu& menu)
    {
        std::vector<std::pair<juce::String, std::function<void()>>> items;
        for (juce::PopupMenu::MenuItemIterator it(menu, true); it.next();)
            items.emplace_back(it.getItem().text, it.getItem().action);
        return items;
    };
    auto items = itemsOf(arranger.buildTempoMenu(arranger.getTempoPointPosition(0).toInt()));
    const auto find = [&](const juce::String& text) -> std::function<void()>
    {
        for (const auto& [t, action] : items)
            if (t.startsWith(text))
                return action;
        FAIL("no menu item " << text);
        return {};
    };
    find("Linear Ramp")();
    CHECK(session.getStartTempoShape() == time::TempoShape::linear);
    items = itemsOf(arranger.buildTempoMenu(arranger.getTempoPointPosition(0).toInt()));
    find("Curve to the Next: Slow Start")();
    CHECK(session.getStartTempoShape() == time::TempoShape::curve);
    CHECK(session.getStartTempoBend() > 0.0);
    items = itemsOf(arranger.buildTempoMenu(arranger.getTempoPointPosition(1).toInt()));
    find("Remove Tempo Change")();
    CHECK(session.getTempoChanges().empty());

    items = itemsOf(arranger.buildSignatureMenu(5));
    find("6/8")();
    REQUIRE(session.getSignatureChanges().size() == 1);
    CHECK(session.getTempoMap().signatureOfBar(4).numerator == 6);
    // Its marker drags along the bars.
    const auto& map = session.getTempoMap();
    const float markerX = arranger.getTempoPointPosition(0).x + static_cast<float>(map.secondsAt(map.beatOfBar(4)) * 40.0) + 2.0f;
    const float markerY = static_cast<float>(arranger.getSignatureStripArea().getCentreY());
    const float bar7X = arranger.getTempoPointPosition(0).x + static_cast<float>(map.secondsAt(map.beatOfBar(6)) * 40.0) + 2.0f;
    arranger.mouseDown(event({markerX, markerY}, {markerX, markerY}));
    arranger.mouseDrag(event({bar7X, markerY}, {markerX, markerY}));
    arranger.mouseUp(event({bar7X, markerY}, {markerX, markerY}));
    CHECK(static_cast<int>(session.getSignatureChanges().front().getProperty(session::ids::bar)) == 7);

    // Snapping and quantizing follow the map: in 6/8 a grid beat is an eighth.
    editor.setGrid(ui::EditorState::Grid::beat);
    const auto& m = session.getTempoMap();
    const double inSixEight = m.secondsAt(m.beatOfBar(6) + 0.6); // 0.6 quarters into a 6/8 bar
    CHECK_THAT(m.beatAt(editor.snap(inSixEight, m)), WithinAbs(m.beatOfBar(6) + 0.5, 1e-9));
    editor.setGrid(ui::EditorState::Grid::bar);
    CHECK_THAT(m.beatAt(editor.snap(m.secondsAt(m.beatOfBar(6) + 1.0), m)), WithinAbs(m.beatOfBar(6), 1e-9));
    CHECK_THAT(m.beatAt(editor.snap(m.secondsAt(m.beatOfBar(6) + 2.0), m)), WithinAbs(m.beatOfBar(7), 1e-9));

    // Hidden again: the tracks move back up.
    editor.setShowingTempoTrack(false);
    CHECK(arranger.getTrackHeaderArea(keys).getY() == headerY);
}

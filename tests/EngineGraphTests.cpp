#include "core/rt/RealtimeGuard.h"
#include "devices/DeviceRegistry.h"
#include "devices/Instruments.h"
#include "engine/AudioEngine.h"
#include "engine/Capture.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <thread>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    auto processor = devices::createBuiltinDevice(device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

/** A session wired to a real engine (driven by the test as if it were the audio device). */
struct Rig
{
    explicit Rig(int workers = 2)
        : engine(workers)
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

    void play(double fromSeconds = 0.0)
    {
        REQUIRE(engine.post(engine::command::Locate{static_cast<SampleCount>(fromSeconds * kRate)}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
    }

    /** Renders `seconds` and appends to left/right. */
    void render(double seconds)
    {
        const int total = static_cast<int>(seconds * kRate);
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < total; done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            left.insert(left.end(), l.begin(), l.end());
            right.insert(right.end(), r.begin(), r.end());
            engine.releaseRetiredGraphs();
        }
    }

    juce::ValueTree audioTrackWith(const juce::File& file, double start = 0.0)
    {
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = session.addTrack();
        session.addClip(t, file, start, asset->getLengthSeconds());
        return t;
    }

    engine::AudioEngine engine;
    Session session;
    AudioAssetCache assets;
    DeviceManager devices;
    GraphBuilder builder;
    std::uint64_t serial = 0;
    std::vector<float> left, right;
};

std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kRate); }
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
} // namespace

TEST_CASE("Tracks routed to a bus are summed there and follow the bus fader", "[engine][routing]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 48000, 0.25f), kRate);

    Rig rig;
    auto a = rig.audioTrackWith(tone);
    auto b = rig.audioTrackWith(tone);
    auto bus = rig.session.addTrack(TrackKind::bus, "Drums");
    track::setOutput(a, track::getId(bus), nullptr);
    track::setOutput(b, track::getId(bus), nullptr);

    rig.publish();
    rig.play();
    rig.render(0.5);
    CHECK_THAT(test::peak(rig.left, at(0.1), at(0.5)), WithinRel(0.5, 0.01)); // both tracks, via the bus

    track::setVolumeDb(bus, kMinusInfinityDb, nullptr);
    rig.publish();
    rig.left.clear();
    rig.render(0.2);
    CHECK(test::peak(rig.left, at(0.1), at(0.2)) < 1e-6);
}

TEST_CASE("Folders sum their children, and soloing keeps the path to the master open", "[engine][routing]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 48000, 0.25f), kRate);

    Rig rig;
    auto a = rig.audioTrackWith(tone);
    auto b = rig.audioTrackWith(tone);
    auto folder = rig.session.groupIntoFolder({a, b}, "Group");
    REQUIRE(folder.isValid());
    REQUIRE(Session::getParentFolder(a) == folder);

    auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
    REQUIRE(graph->levels.size() == 2); // children first, then the folder
    CHECK(graph->masterSources.size() == 1);

    track::setSoloed(a, true, nullptr);
    graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
    const auto find = [&](const juce::ValueTree& t) -> const engine::RenderTrack&
    {
        for (const auto& rt : graph->tracks)
            if (rt.trackId == track::getId(t))
                return rt;
        FAIL("track not in graph");
        return graph->tracks.front();
    };
    CHECK(find(a).audible);
    CHECK_FALSE(find(b).audible);
    CHECK(find(folder).audible); // downstream of a soloed track
}

TEST_CASE("Routing that would create a feedback loop is refused", "[engine][routing]")
{
    Rig rig;
    auto bus1 = rig.session.addTrack(TrackKind::bus, "Bus 1");
    auto bus2 = rig.session.addTrack(TrackKind::bus, "Bus 2");
    track::setOutput(bus1, track::getId(bus2), nullptr);

    CHECK_FALSE(rig.session.canFeed(bus2, bus1));
    CHECK(rig.session.canFeed(bus1, bus2));
    CHECK_FALSE(rig.session.addSend(bus2, bus1).isValid());

    // Even if the document says so, the compiled graph has no loop.
    track::setOutput(bus2, track::getId(bus1), nullptr);
    const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
    CHECK(graph->levels.size() == 2);
}

TEST_CASE("Sends feed a return bus pre or post fader, and mute silences both", "[engine][routing]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 48000, 0.25f), kRate);

    Rig rig;
    auto a = rig.audioTrackWith(tone);
    auto fx = rig.session.addTrack(TrackKind::bus, "FX");
    auto s = rig.session.addSend(a, fx, 0.0, false);
    REQUIRE(s.isValid());
    track::setVolumeDb(a, kMinusInfinityDb, nullptr);

    rig.publish();
    rig.play();
    rig.render(0.3);
    CHECK(test::peak(rig.left, at(0.1), at(0.3)) < 1e-6); // post-fader: the fader is down

    send::setPreFader(s, true, nullptr);
    rig.publish();
    rig.left.clear();
    rig.render(0.3);
    CHECK_THAT(test::peak(rig.left, at(0.1), at(0.3)), WithinRel(0.25, 0.01)); // pre-fader ignores it

    track::setMuted(a, true, nullptr);
    rig.publish();
    rig.left.clear();
    rig.render(0.3);
    CHECK(test::peak(rig.left, at(0.1), at(0.3)) < 1e-6);
}

TEST_CASE("Plug-in delay compensation aligns parallel paths", "[engine][pdc]")
{
    test::TempFolder temp;
    const auto impulse = test::writeImpulse(temp.dir.getChildFile("imp.wav"), 4800, 9600, kRate, 0.4f);

    Rig rig;
    auto a = rig.audioTrackWith(impulse);
    auto b = rig.audioTrackWith(impulse);
    rig.session.addDevice(a, "builtin:limiter", "Limiter"); // 5 ms lookahead = 240 samples

    rig.publish();
    rig.play();
    rig.render(0.3);

    // Both impulses arrive together (one peak of 0.8), late by the limiter's latency.
    CHECK(test::indexOfPeak(rig.left) == 4800 + 240);
    CHECK_THAT(test::peak(rig.left), WithinAbs(0.8, 1e-5));
    CHECK(rig.builder.getTotalLatency() == 240);
}

TEST_CASE("Delay compensation settles at the real latency when a track also sends to a return bus", "[engine][pdc]")
{
    // A track with latency next to a track that sends (post-fader) to a return bus: the return's
    // input is delayed with its source, so the return itself needs no compensation. (This used to
    // grow by the latency on every pass of the alignment, up to 16 times too much.)
    test::TempFolder temp;
    const auto impulse = test::writeImpulse(temp.dir.getChildFile("imp.wav"), 4800, 9600, kRate, 0.25f);

    Rig rig;
    auto a = rig.audioTrackWith(impulse);
    auto b = rig.audioTrackWith(impulse);
    rig.session.addDevice(a, "builtin:limiter", "Limiter"); // 5 ms lookahead = 240 samples
    auto bus = rig.session.addTrack(TrackKind::bus, "Return");
    rig.session.addDevice(bus, "builtin:utility", "Utility");
    rig.session.addSend(b, bus, 0.0);

    rig.publish();
    rig.play();
    rig.render(0.3);

    CHECK(rig.builder.getTotalLatency() == 240);
    // a, b and b's return all arrive together.
    CHECK(test::indexOfPeak(rig.left) == 4800 + 240);
    CHECK_THAT(test::peak(rig.left), WithinAbs(0.75, 1e-4));
}

TEST_CASE("Instrument tracks play MIDI clips through their instrument", "[engine][midi]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("a440.wav"), test::sine(440.0, kRate, 96000, 0.5f), kRate);

    Rig rig;
    auto t = rig.session.addTrack(TrackKind::instrument, "Keys");
    auto d = rig.session.addDevice(t, "builtin:sampler", "Sampler");
    auto node = rig.devices.getNode(d);
    REQUIRE(node != nullptr);
    auto& sampler = dynamic_cast<devices::Sampler&>(node->getProcessor());
    juce::String error;
    REQUIRE(sampler.loadSample(file, error));
    sampler.root_->setValueNotifyingHost(sampler.root_->convertTo0to1(69.0f));

    auto c = rig.session.addMidiClip(t, 0.0, 2.0);
    rig.session.addNote(c, 1.0, 1.0, 69); // beat 1 = 0.5 s at 120 bpm, one beat long

    rig.publish();
    rig.play();
    rig.render(1.5);

    CHECK(test::peak(rig.left, 0, at(0.49)) < 1e-6);
    CHECK_THAT(test::estimateFrequency(rig.left, at(0.55), at(0.95), kRate), WithinRel(440.0, 0.01));
    CHECK(test::peak(rig.left, at(1.3), at(1.5)) < 1e-4); // released at 1 s (150 ms release)
}

TEST_CASE("Volume automation follows its spline", "[engine][automation]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 96000, 0.5f), kRate);

    Rig rig;
    auto t = rig.audioTrackWith(tone);
    auto lane = rig.session.getAutomation(t, automation::volumeTarget(), true, 0.0); // silent at 0 s
    rig.session.addAutomationPoint(lane, 1.0, engine::gainToAutomation(1.0));      // 0 dB at 1 s

    rig.publish();
    rig.play();
    rig.render(1.5);
    CHECK(test::peak(rig.left, 0, at(0.05)) < 1e-3);
    CHECK_THAT(test::peak(rig.left, at(1.1), at(1.5)), WithinRel(0.5, 0.01));
}

TEST_CASE("The loop range wraps the playhead", "[engine][transport]")
{
    Rig rig;
    rig.session.setLoop(true, 0.25, 0.5);
    rig.publish();
    rig.play(0.3);
    rig.render(1.0);

    const auto telemetry = rig.engine.collectTelemetry();
    CHECK(telemetry.playheadSeconds >= 0.25);
    CHECK(telemetry.playheadSeconds < 0.5);
}

TEST_CASE("Parallel rendering is bit-identical to single-threaded rendering", "[engine][parallel]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(330.0, kRate, 48000, 0.1f), kRate);

    const auto renderWith = [&](int workers)
    {
        Rig rig(workers);
        std::vector<juce::ValueTree> tracks;
        for (int i = 0; i < 8; ++i)
        {
            auto t = rig.audioTrackWith(tone, 0.01 * i);
            track::setPan(t, -1.0 + 0.25 * i, nullptr);
            rig.session.addDevice(t, i % 2 == 0 ? "builtin:eq" : "builtin:compressor", "FX");
            tracks.push_back(t);
        }
        auto bus = rig.session.addTrack(TrackKind::bus, "Reverb");
        rig.session.addDevice(bus, "builtin:reverb", "Reverb");
        for (auto& t : tracks)
            rig.session.addSend(t, bus, -6.0);

        rig.publish();
        rig.play();
        rig.render(0.5);
        return rig.left;
    };

    const auto serial = renderWith(0);
    const auto parallel = renderWith(3);
    REQUIRE(serial.size() == parallel.size());
    CHECK(serial == parallel);
    CHECK(test::peak(serial) > 0.1);
}

TEST_CASE("Offline rendering matches the live callback", "[engine][offline]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(330.0, kRate, 48000, 0.3f), kRate);

    Rig live;
    live.audioTrackWith(tone);
    live.publish();
    live.play();
    live.render(0.5);

    Rig offline;
    offline.audioTrackWith(tone);
    offline.publish();
    offline.play();
    REQUIRE(offline.engine.beginOfflineRender());
    std::vector<float> l(kBlock), r(kBlock);
    float* channels[2] = {l.data(), r.data()};
    std::vector<float> rendered;
    for (std::size_t done = 0; done < live.left.size(); done += kBlock)
    {
        offline.engine.renderOffline(channels, 2, kBlock);
        rendered.insert(rendered.end(), l.begin(), l.end());
    }

    // While offline, the live callback stays silent and does not advance.
    std::vector<float> dl(kBlock, 1.0f), dr(kBlock, 1.0f);
    float* device[2] = {dl.data(), dr.data()};
    offline.engine.process(nullptr, 0, device, 2, kBlock);
    CHECK(test::peak(dl) == 0.0);
    offline.engine.endOfflineRender();

    CHECK(rendered == live.left);
}

TEST_CASE("Capture streams record the master and track outputs", "[engine][capture]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(330.0, kRate, 48000, 0.3f), kRate);

    Rig rig;
    auto t = rig.audioTrackWith(tone);
    rig.publish();
    rig.render(0.01); // apply the graph

    const auto masterFile = temp.dir.getChildFile("master.wav");
    const auto stemFile = temp.dir.getChildFile("stem.wav");
    const auto makeStream = [&](const juce::File& file)
    {
        std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(out, juce::AudioFormatWriterOptions{}.withSampleRate(kRate).withNumChannels(2).withBitsPerSample(24));
        return std::make_shared<engine::CaptureStream>(std::move(writer), 2, kRate, 2.0);
    };

    engine::CaptureSession capture;
    capture.master = makeStream(masterFile);
    auto stem = makeStream(stemFile);
    capture.trackOutputs[static_cast<std::size_t>(rig.builder.getSlot(track::getId(t)))] = stem.get();
    capture.ownedTrackStreams.push_back(stem);

    REQUIRE(rig.engine.post(engine::command::StartCapture{&capture}));
    rig.play();
    rig.render(0.5);
    REQUIRE(rig.engine.post(engine::command::StopCapture{}));
    rig.render(0.01);
    CHECK_FALSE(rig.engine.isCaptureActive());

    capture.master->drainToDisk();
    stem->drainToDisk();
    CHECK(capture.master->getOverflowCount() == 0);
    capture.master->finish();
    stem->finish();

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    for (const auto& file : {masterFile, stemFile})
    {
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        REQUIRE(reader != nullptr);
        CHECK(reader->lengthInSamples >= static_cast<juce::int64>(0.49 * kRate));
        juce::AudioBuffer<float> audio(2, static_cast<int>(reader->lengthInSamples));
        reader->read(&audio, 0, audio.getNumSamples(), 0, true, true);
        CHECK_THAT(audio.getMagnitude(0, 4800, 9600), WithinAbs(0.3, 0.01));
    }
}

TEST_CASE("Clip time-stretch keeps the pitch and pitch-shift keeps the timing", "[engine][stretch]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("a440.wav"), test::sine(440.0, kRate, 48000, 0.4f), kRate);

    SECTION("stretch to double length")
    {
        Rig rig;
        auto t = rig.audioTrackWith(tone);
        auto c = t.getChildWithName(ids::clip);
        clip::setStretch(c, 2.0, nullptr);
        clip::setLength(c, 2.0, nullptr);
        rig.publish();
        rig.play();
        rig.render(2.0);

        CHECK(test::rms(rig.left, at(1.4), at(1.8)) > 0.2); // still playing after the source's 1 s
        CHECK_THAT(test::estimateFrequency(rig.left, at(0.5), at(1.5), kRate), WithinRel(440.0, 0.02));
    }

    SECTION("pitch up a fifth")
    {
        Rig rig;
        auto t = rig.audioTrackWith(tone);
        clip::setPitch(t.getChildWithName(ids::clip), 7.0, nullptr);
        rig.publish();
        rig.play();
        rig.render(1.0);
        CHECK_THAT(test::estimateFrequency(rig.left, at(0.2), at(0.8), kRate), WithinRel(440.0 * std::pow(2.0, 7.0 / 12.0), 0.02));
    }
}

TEST_CASE("Overlapping clips crossfade with equal power", "[engine][crossfade]")
{
    Session session;
    AudioAssetCache assets;
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 96000, 0.4f), kRate);
    juce::String error;
    REQUIRE(assets.loadNow(tone, error) != nullptr);

    auto t = session.addTrack();
    session.addClip(t, tone, 0.0, 1.0);
    session.addClip(t, tone, 0.8, 1.0);

    GraphBuilder builder;
    const auto graph = builder.build(session, assets, nullptr);
    const auto& clips = graph->tracks[0].clips;
    REQUIRE(clips.size() == 2);
    CHECK_THAT(clips[0].fadeOutSeconds, WithinAbs(0.2, 1e-9));
    CHECK(clips[0].fadeOutEqualPower);
    CHECK_THAT(clips[1].fadeInSeconds, WithinAbs(0.2, 1e-9));
    CHECK(clips[1].fadeInEqualPower);
}

TEST_CASE("Long files stream from disk", "[engine][streaming]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("long.wav"), test::sine(220.0, kRate, 5 * 48000, 0.3f), kRate);

    Rig rig;
    rig.assets.setStreamingThreshold(1.0);
    rig.assets.setEpochSource([&] { return rig.engine.getProcessEpoch(); });
    auto t = rig.audioTrackWith(tone);
    juce::String error;
    auto asset = rig.assets.loadNow(tone, error);
    REQUIRE(asset->isStreamed());
    CHECK(asset->peaks.getNumBuckets() > 0);

    rig.publish();
    rig.play(3.0);

    // Give the service thread time to bring in the region around the playhead.
    for (int i = 0; i < 100 && test::peak(rig.left) < 0.29; ++i)
    {
        rig.render(0.02);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK_THAT(test::peak(rig.left), WithinAbs(0.3, 0.01));
    CHECK(asset->streamed->getNumResidentChunks() < 5); // not the whole file
    (void)t;
}

TEST_CASE("Rendering a full graph with devices and workers never touches the heap", "[engine][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 96000, 0.2f), kRate);

    Rig rig(3);
    auto bus = rig.session.addTrack(TrackKind::bus, "FX");
    rig.session.addDevice(bus, "builtin:reverb", "Reverb");
    rig.session.addDevice(bus, "builtin:delay", "Delay");
    for (int i = 0; i < 4; ++i)
    {
        auto t = rig.audioTrackWith(tone, 0.1 * i);
        rig.session.addDevice(t, "builtin:eq", "EQ");
        rig.session.addDevice(t, "builtin:compressor", "Comp");
        rig.session.addDevice(t, "builtin:limiter", "Limiter");
        rig.session.addSend(t, bus, -3.0, i % 2 == 0);
        auto lane = rig.session.getAutomation(t, automation::volumeTarget(), true, 0.5);
        rig.session.addAutomationPoint(lane, 1.0, 0.9);
        clip::setPitch(t.getChildWithName(ids::clip), i == 0 ? 3.0 : 0.0, nullptr);
    }
    auto keys = rig.session.addTrack(TrackKind::instrument, "Keys");
    auto sampler = rig.session.addDevice(keys, "builtin:sampler", "Sampler");
    rig.session.addDevice(keys, "builtin:pitch", "Pitch");
    juce::String error;
    REQUIRE(dynamic_cast<devices::Sampler&>(rig.devices.getNode(sampler)->getProcessor()).loadSample(tone, error));
    auto clipTree = rig.session.addMidiClip(keys, 0.0, 2.0);
    for (int n = 0; n < 8; ++n)
        rig.session.addNote(clipTree, n * 0.5, 0.4, 60 + n);
    rig.session.addDevice(rig.session.getMaster(), "builtin:limiter", "Master Limiter");
    rig.session.setLoop(true, 0.0, 1.0);

    // Build graphs up front (allocation is fine on the "message thread").
    std::vector<std::unique_ptr<engine::RenderGraph>> graphs;
    for (int i = 0; i < 10; ++i)
    {
        graphs.push_back(rig.builder.build(rig.session, rig.assets, &rig.devices));
        graphs.back()->serial = static_cast<std::uint64_t>(i + 1);
    }

    REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
    std::vector<float> l(kBlock), r(kBlock);
    float* channels[2] = {l.data(), r.data()};

    const auto before = rt::getRealtimeViolationCount();
    for (int block = 0; block < 600; ++block)
    {
        if (block % 60 == 0)
        {
            engine::command::SwapGraph swap;
            swap.graph = graphs[static_cast<std::size_t>(block / 60)].release();
            REQUIRE(rig.engine.post(swap));
        }
        rig.engine.process(nullptr, 0, channels, 2, kBlock);
        rig.engine.releaseRetiredGraphs();
    }
    CHECK(rt::getRealtimeViolationCount() == before);
    CHECK(test::peak(l) > 0.0);
}

TEST_CASE("Playback can stop at the end of the arrangement", "[engine][transport]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 24'000, 0.25f), kRate);

    Rig rig;
    rig.audioTrackWith(tone, 0.25); // the last clip ends at 0.75 s
    rig.publish();

    // Off (the default): playback runs on past the end.
    rig.play();
    rig.render(1.5);
    auto telemetry = rig.engine.collectTelemetry();
    CHECK(telemetry.playing);
    CHECK(telemetry.playheadSeconds > 1.4);

    // On: it stops exactly at the end of the last clip.
    REQUIRE(rig.engine.post(engine::command::SetPlaying{false}));
    REQUIRE(rig.engine.post(engine::command::SetStopAtEnd{true}));
    rig.play();
    rig.render(1.5);
    telemetry = rig.engine.collectTelemetry();
    CHECK_FALSE(telemetry.playing);
    CHECK_THAT(telemetry.playheadSeconds, WithinAbs(0.75, 1.0e-9));
    CHECK(test::peak(rig.left, at(2.0), at(2.24)) > 0.2); // the clip played up to the end (this pass started at 1.5 s of output)

    // Starting beyond the end plays on (it only stops when crossing the end).
    rig.play(1.0);
    rig.render(0.5);
    telemetry = rig.engine.collectTelemetry();
    CHECK(telemetry.playing);
    CHECK(telemetry.playheadSeconds > 1.4);

    // A loop that wraps before the end keeps looping.
    REQUIRE(rig.engine.post(engine::command::SetPlaying{false}));
    REQUIRE(rig.engine.post(engine::command::SetLoop{true, 0.0, 0.5}));
    rig.play();
    rig.render(2.0);
    telemetry = rig.engine.collectTelemetry();
    CHECK(telemetry.playing);
    CHECK(telemetry.playheadSeconds < 0.5);
}

TEST_CASE("After stopping at the end, playing again starts from the beginning", "[engine][transport]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 24'000, 0.25f), kRate);

    Rig rig;
    rig.audioTrackWith(tone, 0.25); // ends at 0.75 s
    rig.publish();
    REQUIRE(rig.engine.post(engine::command::SetStopAtEnd{true}));
    rig.play();
    rig.render(1.0);
    auto telemetry = rig.engine.collectTelemetry();
    REQUIRE_FALSE(telemetry.playing);
    CHECK(telemetry.stoppedAtEnd);

    // Play (without moving the playhead): from the beginning.
    REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
    rig.render(0.1);
    telemetry = rig.engine.collectTelemetry();
    CHECK(telemetry.playing);
    CHECK_FALSE(telemetry.stoppedAtEnd);
    CHECK(telemetry.playheadSeconds < 0.15);

    // Moving the playhead after the stop: play from there.
    rig.render(1.0);
    REQUIRE(rig.engine.collectTelemetry().stoppedAtEnd);
    REQUIRE(rig.engine.post(engine::command::Locate{static_cast<SampleCount>(0.5 * kRate)}));
    REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
    rig.render(0.1);
    telemetry = rig.engine.collectTelemetry();
    CHECK(telemetry.playheadSeconds > 0.55);
    CHECK(telemetry.playheadSeconds < 0.65);

    // A recording starts where the playhead is (it may extend the song past the end).
    rig.render(0.5);
    REQUIRE(rig.engine.collectTelemetry().stoppedAtEnd);
    engine::CaptureSession capture;
    REQUIRE(rig.engine.post(engine::command::StartCapture{&capture}));
    REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
    rig.render(0.1);
    telemetry = rig.engine.collectTelemetry();
    CHECK(telemetry.playing);
    CHECK(telemetry.playheadSeconds > 0.8);
    REQUIRE(rig.engine.post(engine::command::StopCapture{}));
    REQUIRE(rig.engine.post(engine::command::SetPlaying{false}));
    rig.render(0.05);
}

namespace
{
/** A stereo WAV: a 440 Hz tone on the left, a 1 kHz tone on the right. */
juce::File writeTwoTone(const juce::File& file)
{
    const int frames = static_cast<int>(kRate);
    juce::AudioBuffer<float> buffer(2, frames);
    const auto left = test::sine(440.0, kRate, frames, 0.3f);
    const auto right = test::sine(1000.0, kRate, frames, 0.3f);
    std::copy(left.begin(), left.end(), buffer.getWritePointer(0));
    std::copy(right.begin(), right.end(), buffer.getWritePointer(1));
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(kRate)
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(32)
                                                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    REQUIRE(writer != nullptr);
    writer->writeFromAudioSampleBuffer(buffer, 0, frames);
    return file;
}

/** Amplitude of `frequency` in x[begin, end) (Goertzel). */
double toneIn(const std::vector<float>& x, double frequency, std::size_t begin, std::size_t end)
{
    const double coefficient = 2.0 * std::cos(2.0 * 3.14159265358979323846 * frequency / kRate);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = begin; i < end; ++i)
    {
        const double s0 = x[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coefficient * s1 * s2)) / static_cast<double>(end - begin);
}
} // namespace

TEST_CASE("Mono tracks fold stereo clips; a clip can play just one channel of its file", "[engine][mono]")
{
    test::TempFolder temp;
    const auto file = writeTwoTone(temp.dir.getChildFile("two tone.wav"));

    struct Expect
    {
        double left440, left1000, right440, right1000;
    };
    const auto renderWith = [&](auto setUp)
    {
        Rig rig;
        auto t = rig.audioTrackWith(file);
        setUp(rig, t);
        rig.publish();
        rig.play();
        rig.render(0.6);
        const auto from = at(0.1), to = at(0.5);
        return Expect{toneIn(rig.left, 440.0, from, to), toneIn(rig.left, 1000.0, from, to), toneIn(rig.right, 440.0, from, to),
                      toneIn(rig.right, 1000.0, from, to)};
    };
    const auto clipOf = [](const juce::ValueTree& t)
    {
        for (const auto& c : t)
            if (c.hasType(ids::clip))
                return c;
        return juce::ValueTree();
    };

    // Stereo track: left on the left, right on the right.
    const auto stereo = renderWith([](Rig&, juce::ValueTree) {});
    CHECK_THAT(stereo.left440, WithinAbs(0.3, 0.01));
    CHECK(stereo.left1000 < 0.005);
    CHECK_THAT(stereo.right1000, WithinAbs(0.3, 0.01));
    CHECK(stereo.right440 < 0.005);

    // Mono track: both channels carry the mono sum (half of each tone).
    const auto mono = renderWith([](Rig&, juce::ValueTree t) { track::setFormat(t, TrackFormat::mono, nullptr); });
    for (const double v : {mono.left440, mono.left1000, mono.right440, mono.right1000})
        CHECK_THAT(v, WithinAbs(0.15, 0.01));

    // One channel of the file (Split into Mono): the right channel only, on both sides.
    const auto right = renderWith([&](Rig&, juce::ValueTree t)
                                  {
                                      track::setFormat(t, TrackFormat::mono, nullptr);
                                      clip::setSourceChannel(clipOf(t), 1, nullptr);
                                  });
    CHECK_THAT(right.left1000, WithinAbs(0.3, 0.01));
    CHECK_THAT(right.right1000, WithinAbs(0.3, 0.01));
    CHECK(right.left440 < 0.005);
    CHECK(right.right440 < 0.005);

    // ... also through the time-stretcher, and panned hard left.
    const auto stretched = renderWith([&](Rig&, juce::ValueTree t)
                                      {
                                          track::setFormat(t, TrackFormat::mono, nullptr);
                                          track::setPan(t, -1.0, nullptr);
                                          clip::setSourceChannel(clipOf(t), 0, nullptr);
                                          clip::setStretch(clipOf(t), 1.5, nullptr);
                                          clip::setLength(clipOf(t), 1.5, nullptr);
                                      });
    CHECK(stretched.left440 > 0.2);
    CHECK(stretched.left1000 < 0.01);
    CHECK(stretched.right440 < 0.005); // hard left
}

TEST_CASE("Inactive tracks are not rendered, and neither is anything routed into them", "[engine][inactive]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 48000, 0.25f), kRate);

    Rig rig;
    auto a = rig.audioTrackWith(tone);
    auto b = rig.audioTrackWith(tone);
    auto bus = rig.session.addTrack(TrackKind::bus, "Group");
    track::setOutput(b, track::getId(bus), nullptr);
    auto folder = rig.session.groupIntoFolder({a}, "Folder");
    rig.session.addDevice(a, "builtin:eq", "EQ");

    const auto graphIds = [&rig]
    {
        std::set<std::uint32_t> ids;
        const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
        for (const auto& rt : graph->tracks)
            ids.insert(rt.trackId);
        return ids;
    };
    const auto level = [&rig]
    {
        rig.left.clear();
        rig.publish();
        rig.play();
        rig.render(0.3);
        return test::peak(rig.left, at(0.1), at(0.3));
    };

    CHECK(graphIds().size() == 4);
    CHECK_THAT(level(), WithinAbs(0.5, 0.01)); // a and b

    // An inactive bus: b (routed into it) is silent but still runs; the bus itself is gone.
    track::setActive(bus, false, nullptr);
    CHECK(graphIds() == std::set<std::uint32_t>{track::getId(a), track::getId(b), track::getId(folder)});
    CHECK_THAT(level(), WithinAbs(0.25, 0.01));

    // An inactive folder takes its contents (with their devices) out of the graph.
    track::setActive(folder, false, nullptr);
    CHECK(graphIds() == std::set<std::uint32_t>{track::getId(b)});
    CHECK(level() < 1.0e-6);

    track::setActive(bus, true, nullptr);
    track::setActive(folder, true, nullptr);
    CHECK(graphIds().size() == 4);
    CHECK_THAT(level(), WithinAbs(0.5, 0.01));
}

TEST_CASE("MIDI tracks play the instrument track they are routed to", "[engine][midi][tracks]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("a440.wav"), test::sine(440.0, kRate, 96000, 0.5f), kRate);

    Rig rig;
    auto keys = rig.session.addTrack(TrackKind::instrument, "Keys");
    auto d = rig.session.addDevice(keys, "builtin:sampler", "Sampler");
    auto node = rig.devices.getNode(d);
    REQUIRE(node != nullptr);
    auto& sampler = dynamic_cast<devices::Sampler&>(node->getProcessor());
    juce::String error;
    REQUIRE(sampler.loadSample(file, error));
    sampler.root_->setValueNotifyingHost(sampler.root_->convertTo0to1(69.0f));

    auto midi = rig.session.addTrack(TrackKind::midi, "Melody");
    CHECK(holdsMidi(track::getKind(midi)));
    auto c = rig.session.addMidiClip(midi, 0.0, 2.0);
    rig.session.addNote(c, 1.0, 1.0, 69); // 0.5 s .. 1 s
    auto other = rig.session.addTrack(TrackKind::audio, "Other");
    juce::ignoreUnused(other);

    const auto render = [&rig]
    {
        rig.left.clear();
        rig.publish();
        rig.play();
        rig.render(1.0);
        return test::peak(rig.left, at(0.55), at(0.95));
    };

    CHECK(render() < 1e-6); // no instrument chosen yet
    track::setMidiTarget(midi, track::getId(keys), nullptr);
    CHECK(render() > 0.3);
    CHECK_THAT(test::estimateFrequency(rig.left, at(0.55), at(0.95), kRate), WithinRel(440.0, 0.01));

    track::setMuted(midi, true, nullptr); // a muted MIDI track sends nothing
    CHECK(render() < 1e-6);
    track::setMuted(midi, false, nullptr);

    track::setSoloed(keys, true, nullptr); // soloing the instrument keeps its MIDI track playing
    CHECK(render() > 0.3);
    track::setSoloed(keys, false, nullptr);
    track::setSoloed(other, true, nullptr); // soloing something else silences both
    CHECK(render() < 1e-6);
}

TEST_CASE("VCA Masters control their members' volume, mute and solo", "[engine][vca][tracks]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 96000, 0.25f), kRate);

    Rig rig;
    auto a = rig.audioTrackWith(tone);
    auto b = rig.audioTrackWith(tone);
    auto vca = rig.session.addTrack(TrackKind::vca, "VCA");
    track::setVca(a, track::getId(vca), nullptr);

    const auto levels = [&rig]
    {
        rig.left.clear();
        rig.publish();
        rig.play();
        rig.render(0.5);
        return test::peak(rig.left, at(0.2), at(0.5));
    };
    const auto levelOf = [&](const juce::ValueTree& solo)
    {
        track::setSoloed(solo, true, nullptr);
        const double level = levels();
        track::setSoloed(solo, false, nullptr);
        return level;
    };

    CHECK_THAT(levels(), WithinAbs(0.5, 0.01)); // VCA at 0 dB: both at full level
    track::setVolumeDb(vca, -6.0, nullptr);
    CHECK_THAT(levelOf(a), WithinAbs(0.25 * dbToGain(-6.0), 0.005)); // the member follows the VCA
    CHECK_THAT(levelOf(b), WithinAbs(0.25, 0.005));                  // the other does not

    track::setMuted(vca, true, nullptr);
    CHECK_THAT(levels(), WithinAbs(0.25, 0.005)); // only b
    track::setMuted(vca, false, nullptr);

    CHECK_THAT(levelOf(vca), WithinAbs(0.25 * dbToGain(-6.0), 0.005)); // soloing the VCA solos its members

    // Nested: a VCA in a VCA multiplies; an inactive VCA does nothing.
    auto outer = rig.session.addTrack(TrackKind::vca, "Outer");
    track::setVca(vca, track::getId(outer), nullptr);
    track::setVolumeDb(outer, -6.0, nullptr);
    CHECK_THAT(levelOf(a), WithinAbs(0.25 * dbToGain(-12.0), 0.005));
    track::setActive(outer, false, nullptr);
    CHECK_THAT(levelOf(a), WithinAbs(0.25 * dbToGain(-6.0), 0.005));
    track::setActive(outer, true, nullptr);

    // VCA automation (a ramp) multiplies over time.
    auto lane = rig.session.getAutomation(outer, automation::volumeTarget(), true, 1.0);
    rig.session.addAutomationPoint(lane, 0.0, 60.0 / 66.0); // 0 dB
    rig.session.addAutomationPoint(lane, 10.0, 60.0 / 66.0);
    CHECK_THAT(levelOf(a), WithinAbs(0.25 * dbToGain(-6.0), 0.005));
}

TEST_CASE("Deleting a VCA Master, or duplicating a member without it, keeps the sound", "[engine][vca][tracks]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 5 * 48000, 0.25f), kRate);

    Rig rig;
    auto a = rig.audioTrackWith(tone);
    track::setVolumeDb(a, -3.0, nullptr);
    auto vca = rig.session.addTrack(TrackKind::vca, "VCA");
    track::setVca(a, track::getId(vca), nullptr);
    track::setVolumeDb(vca, -10.0, nullptr); // ignored: the VCA's automation rules
    auto lane = rig.session.getAutomation(vca, automation::volumeTarget(), true, 60.0 / 66.0); // 0 dB from 0 s ...
    rig.session.addAutomationPoint(lane, 1.0, 60.0 / 66.0); // ... to 1 s ...
    rig.session.addAutomationPoint(lane, 3.0, 48.0 / 66.0); // ... -12 dB at 3 s

    const auto levelsAt = [&rig](std::initializer_list<double> times)
    {
        rig.left.clear();
        rig.publish();
        rig.play();
        rig.render(4.0);
        std::vector<double> result;
        for (const double t : times)
            result.push_back(test::peak(rig.left, at(t), at(t + 0.05)));
        return result;
    };

    const auto before = levelsAt({0.5, 2.0, 3.5});
    CHECK_THAT(before[0], WithinAbs(0.25 * dbToGain(-3.0), 0.003));
    CHECK_THAT(before[2], WithinAbs(0.25 * dbToGain(-15.0), 0.003));

    // Duplicated without its VCA assignment: the copy sounds like its source (solo it to hear it alone).
    Session::DuplicateOptions options;
    options.vcaAssignment = false;
    auto copy = rig.session.duplicateTrack(a, options, a);
    CHECK(track::getVca(copy) == 0);
    track::setSoloed(copy, true, nullptr);
    const auto copied = levelsAt({0.5, 2.0, 3.5});
    track::setSoloed(copy, false, nullptr);
    rig.session.removeTrack(copy);
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK_THAT(copied[i], WithinAbs(before[i], 0.005));

    // Deleting the VCA coalesces its automation into the member.
    rig.session.coalesceVca(a, vca);
    rig.session.removeTrack(vca);
    CHECK(track::getVca(a) == 0);
    const auto after = levelsAt({0.5, 2.0, 3.5});
    for (std::size_t i = 0; i < before.size(); ++i)
        CHECK_THAT(after[i], WithinAbs(before[i], 0.005));
}

TEST_CASE("Master Faders: a last fader and post-fader devices on a bus or the main output", "[engine][masterfader][tracks]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 96000, 0.25f), kRate);

    Rig rig;
    auto a = rig.audioTrackWith(tone);
    auto b = rig.audioTrackWith(tone);
    auto bus = rig.session.addTrack(TrackKind::bus, "Group");
    track::setOutput(a, track::getId(bus), nullptr);

    const auto level = [&rig]
    {
        rig.left.clear();
        rig.publish();
        rig.play();
        rig.render(0.5);
        return test::peak(rig.left, at(0.3), at(0.5));
    };
    CHECK_THAT(level(), WithinAbs(0.5, 0.005));

    // On the bus: a and b sum 0.25 * g + 0.25.
    auto busFader = rig.session.addTrack(TrackKind::masterFader, "Group Master");
    track::setControlledBus(busFader, track::getId(bus), nullptr);
    track::setVolumeDb(busFader, -6.0, nullptr);
    CHECK_THAT(level(), WithinAbs(0.25 * dbToGain(-6.0) + 0.25, 0.005));
    track::setMuted(busFader, true, nullptr);
    CHECK_THAT(level(), WithinAbs(0.25, 0.005));
    track::setMuted(busFader, false, nullptr);

    // On the main output: everything.
    auto mainFader = rig.session.addTrack(TrackKind::masterFader, "Master 1");
    track::setVolumeDb(mainFader, -6.0, nullptr);
    CHECK_THAT(level(), WithinAbs((0.25 * dbToGain(-6.0) + 0.25) * dbToGain(-6.0), 0.005));

    // A second master fader on the same path does nothing (one per path).
    auto extra = rig.session.addTrack(TrackKind::masterFader, "Master 2");
    track::setVolumeDb(extra, -20.0, nullptr);
    CHECK_THAT(level(), WithinAbs((0.25 * dbToGain(-6.0) + 0.25) * dbToGain(-6.0), 0.005));
    rig.session.removeTrack(extra);

    // Post-fader devices count for delay compensation.
    rig.session.addDevice(mainFader, "builtin:limiter", "Limiter");
    const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
    CHECK(graph->totalLatency == 240);
    CHECK(graph->hasMasterFader);
    CHECK(graph->masterFaderDevices.size() == 1);
    // ... and neither VCAs nor Master Faders are rendered as tracks.
    for (const auto& rt : graph->tracks)
        CHECK(isRendered(rt.kind));
}

TEST_CASE("A click track ticks in time with the transport", "[engine][click][tracks]")
{
    Rig rig;
    auto click = rig.session.addTrack(TrackKind::bus, "Click");
    track::setFormat(click, TrackFormat::mono, nullptr);
    rig.session.addDevice(click, "builtin:click", "Click");

    rig.publish();
    rig.render(0.3); // stopped: silent
    CHECK(test::peak(rig.left) < 1e-9);

    rig.left.clear();
    rig.play(1.0); // from beat 2 (1 s = 2 beats at 120 bpm)
    rig.render(1.2);
    std::vector<std::size_t> starts;
    std::size_t quiet = 1000;
    for (std::size_t i = 0; i < rig.left.size(); ++i)
    {
        const bool loud = std::abs(rig.left[i]) > 1.0e-3;
        if (loud && quiet >= 200)
            starts.push_back(i);
        quiet = loud ? 0 : quiet + 1;
    }
    REQUIRE(starts.size() >= 3);
    CHECK(starts[0] <= 1);                   // beat 2 at 1.0 s
    CHECK(starts[1] - at(0.5) <= 1);         // beat 3 at 1.5 s
    CHECK(starts[2] - at(1.0) <= 1);         // beat 4 at 2.0 s
    CHECK(std::abs(rig.right[at(0.5) + 20]) > 0.0); // a mono bus: both sides
}

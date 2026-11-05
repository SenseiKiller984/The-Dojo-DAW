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

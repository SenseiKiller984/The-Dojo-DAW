#include "core/rt/RealtimeGuard.h"
#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/ChannelLayout.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/DeviceManager.h"
#include "session/Exporter.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::session;
using engine::ChannelLayout;
using engine::Speaker;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;
constexpr double kMinus3dB = 0.70710678118654752;

/** A pass-through processor that reports `latency` samples (for delay compensation). */
class LatentProcessor final : public juce::AudioProcessor
{
public:
    explicit LatentProcessor(int latency)
        : juce::AudioProcessor(BusesProperties()
                                   .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                   .withOutput("Output", juce::AudioChannelSet::stereo(), true))
    {
        setLatencySamples(latency);
    }

    const juce::String getName() const override { return "Latent"; }
    void prepareToPlay(double, int) override {}
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
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
};

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    if (device::getType(device) == "test:latency3000")
        return std::make_unique<LatentProcessor>(3000);
    auto processor = devices::createBuiltinDevice(device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

double gainOf(const engine::ChannelMatrix& m, const ChannelLayout& to, Speaker out, const ChannelLayout& from, Speaker in)
{
    return m.gain[static_cast<std::size_t>(to.indexOf(out))][static_cast<std::size_t>(from.indexOf(in))];
}

/** A file whose channel k holds the constant 0.05 * (k + 1). */
juce::File steps(const juce::File& file, int numChannels, double seconds = 1.0)
{
    std::vector<std::vector<float>> channels;
    for (int k = 0; k < numChannels; ++k)
        channels.emplace_back(static_cast<std::size_t>(seconds * kRate), 0.05f * static_cast<float>(k + 1));
    return test::writeMultichannelWav(file, channels, kRate);
}

/** A session on a real engine with `outputs` device outputs. */
struct Rig
{
    explicit Rig(int outputs)
        : engine(2)
        , devices(session, createDevice)
        , numOutputs(outputs)
    {
        engine.prepare(kRate, kBlock, outputs, 0);
        devices.setPlayHead(engine.getPlayHead());
        devices.prepareAll(kRate, kBlock);
        builder.setRenderFormat(kRate, kBlock);
        out.resize(static_cast<std::size_t>(outputs));
    }

    std::unique_ptr<engine::RenderGraph> build() { return builder.build(session, assets, &devices); }

    void publish(std::unique_ptr<engine::RenderGraph> graph)
    {
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
    }
    void publish() { publish(build()); }

    void play()
    {
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
    }

    void render(double seconds)
    {
        for (auto& channel : out)
            channel.clear();
        std::vector<std::vector<float>> buffers(static_cast<std::size_t>(numOutputs), std::vector<float>(kBlock));
        std::vector<float*> pointers;
        for (auto& b : buffers)
            pointers.push_back(b.data());
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, pointers.data(), numOutputs, kBlock);
            for (std::size_t ch = 0; ch < buffers.size(); ++ch)
                out[ch].insert(out[ch].end(), buffers[ch].begin(), buffers[ch].end());
            engine.releaseRetiredGraphs();
        }
    }

    /** Output `channel` in the middle of the render (after every fade and ramp). */
    double level(int channel) const { return out[static_cast<std::size_t>(channel)][out[0].size() / 2]; }

    juce::ValueTree trackWith(const juce::File& file, TrackFormat format, const juce::String& name = "Track")
    {
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = session.addTrack(TrackKind::audio, name);
        track::setFormat(t, format, nullptr);
        session.addClip(t, file, 0.0, asset->getLengthSeconds());
        return t;
    }

    engine::AudioEngine engine;
    Session session;
    AudioAssetCache assets;
    DeviceManager devices;
    GraphBuilder builder;
    int numOutputs;
    std::vector<std::vector<float>> out;
};
} // namespace

TEST_CASE("3D beds: 5.1.4, 7.1.4 and 9.1.6 speakers, their order and where they are", "[engine][immersive]")
{
    const auto b514 = ChannelLayout::surround514();
    const auto b714 = ChannelLayout::surround714();
    const auto b916 = ChannelLayout::surround916();

    // The channel orders: L R C LFE, the ear-level surrounds, then wides, then the top layer.
    REQUIRE(b514.numChannels == 10);
    REQUIRE(b714.numChannels == 12);
    REQUIRE(b916.numChannels == 16);
    CHECK(b514.indexOf(Speaker::leftSurround) == 4);
    CHECK(b514.indexOf(Speaker::topFrontLeft) == 6);
    CHECK(b514.indexOf(Speaker::topRearRight) == 9);
    CHECK_FALSE(b514.has(Speaker::leftRear));
    CHECK(b714.indexOf(Speaker::leftRear) == 6);
    CHECK(b714.indexOf(Speaker::topFrontLeft) == 8);
    CHECK(b714.indexOf(Speaker::topRearRight) == 11);
    CHECK(b916.indexOf(Speaker::leftWide) == 8);
    CHECK(b916.indexOf(Speaker::rightWide) == 9);
    CHECK(b916.indexOf(Speaker::topFrontLeft) == 10);
    CHECK(b916.indexOf(Speaker::topMiddleLeft) == 12);
    CHECK(b916.indexOf(Speaker::topRearRight) == 15);

    CHECK(b514.hasHeights());
    CHECK(b916.hasHeights());
    CHECK_FALSE(ChannelLayout::surround71().hasHeights());
    CHECK(engine::isHeight(Speaker::topMiddleRight));
    CHECK_FALSE(engine::isHeight(Speaker::leftWide));
    CHECK(engine::sideOf(Speaker::topRearLeft) == -1);
    CHECK(engine::sideOf(Speaker::rightWide) == 1);
    CHECK(juce::String(engine::shortName(Speaker::topFrontLeft)) == "Ltf");
    CHECK(juce::String(engine::shortName(Speaker::topMiddleRight)) == "Rtm");
    CHECK(juce::String(engine::shortName(Speaker::leftWide)) == "Lw");

    // Where they are (ITU-R BS.2051: left positive): the surrounds and tops depend on the family.
    const auto at = [](const ChannelLayout& layout, Speaker speaker) { return engine::directionOf(layout, speaker); };
    CHECK(at(b514, Speaker::leftSurround).azimuth == 110.0);
    CHECK(at(b514, Speaker::topFrontLeft).azimuth == 30.0);
    CHECK(at(b514, Speaker::topFrontLeft).elevation == 30.0);
    CHECK(at(b514, Speaker::topRearRight).azimuth == -110.0);
    CHECK(at(b714, Speaker::leftSurround).azimuth == 90.0);
    CHECK(at(b714, Speaker::leftRear).azimuth == 135.0);
    CHECK(at(b714, Speaker::topFrontRight).azimuth == -45.0);
    CHECK(at(b714, Speaker::topRearLeft).azimuth == 135.0);
    CHECK(at(b916, Speaker::leftWide).azimuth == 60.0);
    CHECK(at(b916, Speaker::topMiddleRight).azimuth == -90.0);
    CHECK(at(b916, Speaker::topMiddleRight).elevation == 30.0);
    CHECK(at(b916, Speaker::centre).elevation == 0.0);

    // The session formats.
    CHECK(getLayout(TrackFormat::surround514) == b514);
    CHECK(getLayout(TrackFormat::surround714) == b714);
    CHECK(getLayout(TrackFormat::surround916) == b916);
    CHECK(trackFormatFromString("7.1.4") == TrackFormat::surround714);
    // Files with 10, 12 and 16 channels are read in these orders.
    CHECK(fileChannelFor(12, b714, b714.indexOf(Speaker::topRearLeft)) == 10);
    CHECK(fileChannelFor(16, ChannelLayout::surround71(), 7) == 7);
    CHECK(fileChannelFor(10, b714, b714.indexOf(Speaker::topFrontRight)) == b514.indexOf(Speaker::topFrontRight));
    CHECK(fileChannelFor(10, b714, b714.indexOf(Speaker::leftRear)) == -1); // (5.1.4 has no rears)
}

TEST_CASE("3D beds fold down by speaker: tops into the layer below, wides into front and surround", "[engine][immersive]")
{
    const auto b514 = ChannelLayout::surround514();
    const auto b714 = ChannelLayout::surround714();
    const auto b916 = ChannelLayout::surround916();
    const auto s71 = ChannelLayout::surround71();
    const auto s51 = ChannelLayout::surround51();
    const auto stereo = ChannelLayout::stereo();

    // 7.1.4 -> 7.1: the ear layer passes; each top into the speaker below it at -3 dB.
    const auto to71 = engine::conversionMatrix(b714, s71);
    CHECK(gainOf(to71, s71, Speaker::leftRear, b714, Speaker::leftRear) == 1.0);
    CHECK_THAT(gainOf(to71, s71, Speaker::left, b714, Speaker::topFrontLeft), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(to71, s71, Speaker::rightRear, b714, Speaker::topRearRight), WithinAbs(kMinus3dB, 1e-12));
    CHECK(gainOf(to71, s71, Speaker::right, b714, Speaker::topFrontLeft) == 0.0);

    // 5.1.4 -> 5.1: no rears, so the top rears go into the surrounds (one step, -3 dB).
    const auto to51 = engine::conversionMatrix(b514, s51);
    CHECK_THAT(gainOf(to51, s51, Speaker::leftSurround, b514, Speaker::topRearLeft), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(to51, s51, Speaker::right, b514, Speaker::topFrontRight), WithinAbs(kMinus3dB, 1e-12));

    // 9.1.6 -> 7.1.4: the tops pass, the top middles go into the sides, the wides split.
    const auto to714 = engine::conversionMatrix(b916, b714);
    CHECK(gainOf(to714, b714, Speaker::topFrontLeft, b916, Speaker::topFrontLeft) == 1.0);
    CHECK_THAT(gainOf(to714, b714, Speaker::leftSurround, b916, Speaker::topMiddleLeft), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(to714, b714, Speaker::left, b916, Speaker::leftWide), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(to714, b714, Speaker::leftSurround, b916, Speaker::leftWide), WithinAbs(kMinus3dB, 1e-12));

    // 9.1.6 -> stereo, by the same steps: a top front at -3 dB; a top middle and a top rear (each
    // into the surround, which folds into the front) at -6, as the rear itself; a wide (no
    // surround to share with) at -3; nothing crosses sides.
    const auto toStereo = engine::conversionMatrix(b916, stereo);
    CHECK_THAT(gainOf(toStereo, stereo, Speaker::left, b916, Speaker::topFrontLeft), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(toStereo, stereo, Speaker::left, b916, Speaker::topMiddleLeft), WithinAbs(0.5, 1e-12));
    CHECK_THAT(gainOf(toStereo, stereo, Speaker::left, b916, Speaker::topRearLeft), WithinAbs(0.5, 1e-12));
    CHECK_THAT(gainOf(toStereo, stereo, Speaker::left, b916, Speaker::leftRear), WithinAbs(0.5, 1e-12));
    CHECK_THAT(gainOf(toStereo, stereo, Speaker::left, b916, Speaker::leftWide), WithinAbs(kMinus3dB, 1e-12));
    CHECK(gainOf(toStereo, stereo, Speaker::right, b916, Speaker::topMiddleLeft) == 0.0);

    // Up into a 3D bed: what both have passes, the heights get nothing.
    const auto up = engine::conversionMatrix(s51, b714);
    CHECK(gainOf(up, b714, Speaker::leftSurround, s51, Speaker::leftSurround) == 1.0);
    for (const auto top : {Speaker::topFrontLeft, Speaker::topRearRight})
        for (int in = 0; in < s51.numChannels; ++in)
            CHECK(up.gain[static_cast<std::size_t>(b714.indexOf(top))][static_cast<std::size_t>(in)] == 0.0);

    // Effects run on the speaker pairs (and C, LFE alone): nine groups for 9.1.6.
    const auto groups = engine::channelGroupsOf(b916);
    REQUIRE(groups.count == 9);
    CHECK(groups.groups[3].first == b916.indexOf(Speaker::leftWide));
    CHECK(groups.groups[3].second == b916.indexOf(Speaker::rightWide));
    CHECK(groups.groups[5].first == b916.indexOf(Speaker::topMiddleLeft));
    CHECK(groups.groups[7].second == -1); // (the centre, alone)
    CHECK(engine::channelGroupsOf(b714).count == 7);
    CHECK(engine::channelGroupsOf(b514).count == 6);
}

TEST_CASE("A 7.1.4 track plays its twelve channels to a 7.1.4 master, or folded down", "[engine][immersive]")
{
    test::TempFolder temp;
    const auto file = steps(temp.dir.getChildFile("twelve.wav"), 12);
    const auto b714 = ChannelLayout::surround714();

    SECTION("twelve outputs: one per channel, meters for all of them")
    {
        Rig rig(12);
        rig.session.setMasterFormat(TrackFormat::surround714);
        auto t = rig.trackWith(file, TrackFormat::surround714);
        rig.publish();
        rig.play();
        rig.render(0.5);
        for (int ch = 0; ch < 12; ++ch)
            CHECK_THAT(rig.level(ch), WithinAbs(0.05 * (ch + 1), 1e-5));
        const auto slot = rig.builder.getSlot(track::getId(t));
        CHECK(rig.engine.getSlotCapacity(slot).channels >= 12);
        const auto peaks = rig.engine.collectTrackPeaks(slot);
        CHECK_THAT(peaks.peak[11], WithinAbs(0.6, 1e-4));
        const auto telemetry = rig.engine.collectTelemetry();
        CHECK(telemetry.masterChannels == 12);
        CHECK_THAT(telemetry.masterPeaks[static_cast<std::size_t>(b714.indexOf(Speaker::topRearRight))], WithinAbs(0.6, 1e-4));

        // Balance acts on the left and right speakers of every layer, the tops too.
        track::setPan(t, -1.0, nullptr);
        rig.publish();
        rig.render(0.5);
        CHECK_THAT(rig.level(b714.indexOf(Speaker::topFrontLeft)), WithinAbs(0.45, 1e-5));
        CHECK_THAT(rig.level(b714.indexOf(Speaker::topFrontRight)), WithinAbs(0.0, 1e-6));
        CHECK_THAT(rig.level(b714.indexOf(Speaker::centre)), WithinAbs(0.15, 1e-5));
    }

    SECTION("a 7.1.4 master on a stereo device: the output is folded down")
    {
        Rig rig(2);
        rig.session.setMasterFormat(TrackFormat::surround714);
        rig.trackWith(file, TrackFormat::surround714);
        rig.publish();
        rig.play();
        rig.render(0.5);
        const auto fold = engine::conversionMatrix(b714, ChannelLayout::stereo());
        double left = 0.0;
        for (int in = 0; in < 12; ++in)
            left += fold.gain[0][static_cast<std::size_t>(in)] * 0.05 * (in + 1);
        CHECK_THAT(rig.level(0), WithinAbs(left, 1e-5));
        CHECK(rig.level(0) > 0.05 + kMinus3dB * 0.45); // (L and its top front at least)
    }

    SECTION("a 7.1.4 track into a 5.1 master: the heights and rears fold into the layer below")
    {
        Rig rig(6);
        rig.session.setMasterFormat(TrackFormat::surround51);
        rig.trackWith(file, TrackFormat::surround714);
        rig.publish();
        rig.play();
        rig.render(0.5);
        const auto s51 = ChannelLayout::surround51();
        // Ls: its own 0.25, the rear 0.35 and the top rear 0.55 (no rears here) each at -3 dB.
        CHECK_THAT(rig.level(s51.indexOf(Speaker::leftSurround)), WithinAbs(0.25 + kMinus3dB * (0.35 + 0.55), 1e-5));
        // L: its own 0.05 and the top front 0.45 at -3 dB.
        CHECK_THAT(rig.level(s51.indexOf(Speaker::left)), WithinAbs(0.05 + kMinus3dB * 0.45, 1e-5));
    }
}

TEST_CASE("A 9.1.6 session renders sixty stereo tracks: every slot grows to sixteen channels", "[engine][immersive]")
{
    test::TempFolder temp;
    std::vector<std::vector<float>> quiet(2, std::vector<float>(static_cast<std::size_t>(kRate), 0.01f));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("quiet.wav"), quiet, kRate);

    Rig rig(16);
    rig.session.setMasterFormat(TrackFormat::surround916);
    std::vector<juce::ValueTree> tracks;
    for (int i = 0; i < 60; ++i)
        tracks.push_back(rig.trackWith(file, TrackFormat::stereo, "Track " + juce::String(i + 1)));

    // The first graph brings room for every slot; the next needs none.
    auto first = rig.build();
    CHECK(first->slotBuffers.size() == 60);
    for (const auto& room : first->slotBuffers)
        CHECK(room.channels == 16);
    rig.publish(std::move(first));
    CHECK(rig.build()->slotBuffers.empty());

    rig.play();
    rig.render(0.5);
    // All sixty play (once a session could only render 32 tracks into a surround master).
    const auto b916 = ChannelLayout::surround916();
    CHECK_THAT(rig.level(b916.indexOf(Speaker::left)), WithinAbs(0.6, 1e-4));
    CHECK_THAT(rig.level(b916.indexOf(Speaker::right)), WithinAbs(0.6, 1e-4));
    CHECK_THAT(rig.level(b916.indexOf(Speaker::topFrontLeft)), WithinAbs(0.0, 1e-9));
    for (const auto& t : tracks)
        CHECK(rig.engine.getSlotCapacity(rig.builder.getSlot(track::getId(t))).channels == 16);
}

TEST_CASE("Slot room: delay rings as long as the delay needs, kept when it shrinks, carried by graphs", "[engine][immersive]")
{
    test::TempFolder temp;
    std::vector<std::vector<float>> quiet(2, std::vector<float>(static_cast<std::size_t>(kRate), 0.1f));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("quiet.wav"), quiet, kRate);

    SECTION("rings")
    {
        Rig rig(12);
        rig.session.setMasterFormat(TrackFormat::surround714);
        auto latent = rig.trackWith(file, TrackFormat::stereo, "Latent");
        auto plain = rig.trackWith(file, TrackFormat::stereo, "Plain");
        auto device = rig.session.addDevice(latent, "test:latency3000", "Latent");
        rig.publish();
        rig.play();
        rig.render(0.25);

        // The plain track waits 3000 samples for the latent one: a 4096-sample ring.
        const auto plainSlot = rig.builder.getSlot(track::getId(plain));
        const auto latentSlot = rig.builder.getSlot(track::getId(latent));
        CHECK(rig.engine.getSlotCapacity(plainSlot).channels == 12);
        CHECK(rig.engine.getSlotCapacity(plainSlot).delaySamples == 4096);
        CHECK(rig.engine.getSlotCapacity(latentSlot).delaySamples == engine::kMinWideDelaySamples);
        rig.render(0.5);
        CHECK_THAT(rig.level(0), WithinAbs(0.2, 1e-5)); // (both tracks, aligned)

        // Without the latency the ring stays (slots never shrink); no new room is needed.
        rig.session.removeDevice(device);
        auto graph = rig.build();
        CHECK(graph->slotBuffers.empty());
        rig.publish(std::move(graph));
        rig.render(0.25);
        CHECK(rig.engine.getSlotCapacity(plainSlot).delaySamples == 4096);
    }

    SECTION("a graph replaced before it was posted passes its room on")
    {
        juce::MessageManager::getInstance();
        engine::AudioEngine engine(2);
        engine::AudioDeviceHost host(engine);
        engine::EngineController controller(engine, host);
        engine.prepare(kRate, kBlock, 12, 0);
        Session session;
        AudioAssetCache assets;
        DeviceManager devices(session, createDevice);
        devices.prepareAll(kRate, kBlock);
        GraphBuilder builder;
        builder.setRenderFormat(kRate, kBlock);
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);

        // G0 is posted (in flight); G1 waits; G2 replaces G1 before it is ever posted.
        controller.publishGraph(builder.build(session, assets, &devices));
        session.setMasterFormat(TrackFormat::surround714);
        auto t = session.addTrack(TrackKind::audio, "Late");
        session.addClip(t, file, 0.0, asset->getLengthSeconds());
        auto g1 = builder.build(session, assets, &devices);
        REQUIRE(g1->slotBuffers.size() == 1);
        controller.publishGraph(std::move(g1));
        track::setVolumeDb(t, -6.0, nullptr);
        auto g2 = builder.build(session, assets, &devices);
        CHECK(g2->slotBuffers.empty()); // (the builder counts on G1's room)
        controller.publishGraph(std::move(g2));

        std::vector<std::vector<float>> buffers(12, std::vector<float>(kBlock));
        std::vector<float*> pointers;
        for (auto& b : buffers)
            pointers.push_back(b.data());
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        for (int block = 0; block < 40; ++block)
        {
            engine.process(nullptr, 0, pointers.data(), 12, kBlock);
            controller.pump(); // (posts G2 once G0 is in)
        }
        CHECK(engine.getSlotCapacity(builder.getSlot(track::getId(t))).channels == 12);
        CHECK_THAT(buffers[0][kBlock / 2], WithinAbs(0.1 * std::pow(10.0, -6.0 / 20.0), 1e-5));
    }
}

TEST_CASE("3D bed exports: 5.1.4 with its WAV channel mask, 7.1.4 and 9.1.6 as discrete channels", "[session][export][immersive]")
{
    test::TempFolder temp;
    ExportSettings settings;
    settings.format = ExportSettings::Format::wav;
    settings.bitDepth = 24;

    const auto writeAndRead = [&](const ChannelLayout& layout, const juce::String& name)
    {
        const auto file = temp.dir.getChildFile(name);
        juce::String error;
        {
            auto writer = Exporter::createWriter(file, settings, kRate, layout.numChannels, error, &layout);
            REQUIRE(writer != nullptr);
            juce::AudioBuffer<float> buffer(layout.numChannels, 64);
            for (int ch = 0; ch < layout.numChannels; ++ch)
                juce::FloatVectorOperations::fill(buffer.getWritePointer(ch), 0.05f * static_cast<float>(ch + 1), 64);
            REQUIRE(writer->writeFromAudioSampleBuffer(buffer, 0, 64));
        }
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(file.createInputStream().release(), true));
        REQUIRE(reader != nullptr);
        REQUIRE(static_cast<int>(reader->numChannels) == layout.numChannels);
        juce::AudioBuffer<float> back(layout.numChannels, 64);
        reader->read(&back, 0, 64, 0, true, true);
        for (int ch = 0; ch < layout.numChannels; ++ch)
            CHECK_THAT(back.getSample(ch, 10), WithinAbs(0.05 * (ch + 1), 1e-4)); // (the format's own order)
        return reader->getChannelLayout();
    };

    CHECK(writeAndRead(ChannelLayout::surround514(), "514.wav") == juce::AudioChannelSet::create5point1point4());
    CHECK(writeAndRead(ChannelLayout::surround51(), "51.wav") == juce::AudioChannelSet::create5point1());
    CHECK(writeAndRead(ChannelLayout::surround714(), "714.wav").isDiscreteLayout());
    CHECK(writeAndRead(ChannelLayout::surround916(), "916.wav").isDiscreteLayout());
    // 7.1 is written without a mask (its rears would be labelled front-of-centre), in its own order.
    (void) writeAndRead(ChannelLayout::surround71(), "71.wav");

    // FLAC holds up to 8 channels.
    settings.format = ExportSettings::Format::flac;
    juce::String error;
    const auto layout = ChannelLayout::surround714();
    CHECK(Exporter::createWriter(temp.dir.getChildFile("714.flac"), settings, kRate, 12, error, &layout) == nullptr);
    CHECK(error.contains("FLAC holds up to 8 channels"));
}

TEST_CASE("Rendering 3D beds and the slots growing into them never touches the heap", "[engine][immersive][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    test::TempFolder temp;
    const auto sixteen = steps(temp.dir.getChildFile("sixteen.wav"), 16, 2.0);
    const auto twelve = steps(temp.dir.getChildFile("twelve.wav"), 12, 2.0);
    const auto stereo = steps(temp.dir.getChildFile("stereo.wav"), 2, 2.0);

    Rig rig(16);
    rig.session.setMasterFormat(TrackFormat::surround916);
    auto bed = rig.trackWith(sixteen, TrackFormat::surround916, "Bed");
    rig.session.addDevice(bed, "builtin:eq", "EQ");
    rig.trackWith(twelve, TrackFormat::surround714, "Heights");
    auto music = rig.trackWith(stereo, TrackFormat::stereo, "Music");
    rig.session.addDevice(music, "builtin:compressor", "Comp");
    rig.session.addDevice(rig.session.getMaster(), "builtin:limiter", "Master Limiter");

    // Five graphs made up front; the first carries the room, swapped in on the audio thread.
    std::vector<std::unique_ptr<engine::RenderGraph>> graphs;
    for (int i = 0; i < 5; ++i)
        graphs.push_back(rig.build());
    CHECK_FALSE(graphs.front()->slotBuffers.empty());

    REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
    std::vector<std::vector<float>> buffers(16, std::vector<float>(kBlock));
    std::vector<float*> pointers;
    for (auto& b : buffers)
        pointers.push_back(b.data());

    const auto before = rt::getRealtimeViolationCount();
    for (int block = 0; block < 300; ++block)
    {
        if (block % 60 == 0)
        {
            engine::command::SwapGraph swap;
            swap.graph = graphs[static_cast<std::size_t>(block / 60)].release();
            REQUIRE(rig.engine.post(swap));
        }
        rig.engine.process(nullptr, 0, pointers.data(), 16, kBlock);
        rig.engine.releaseRetiredGraphs();
    }
    CHECK(rt::getRealtimeViolationCount() == before);
    CHECK(test::peak(buffers[static_cast<std::size_t>(ChannelLayout::surround916().indexOf(Speaker::topMiddleLeft))]) > 0.0f);
}

TEST_CASE("An export is not monitoring: a 7.1.4 master renders twelve channels whatever the surround mode", "[engine][export][immersive]")
{
    test::TempFolder temp;
    const auto file = steps(temp.dir.getChildFile("twelve.wav"), 12);

    Rig rig(12);
    rig.session.setMasterFormat(TrackFormat::surround714);
    rig.trackWith(file, TrackFormat::surround714);
    rig.publish();

    // Monitoring through the headphone virtualizer: a live device hears two ears.
    engine::command::SetSurroundMonitoring monitoring;
    monitoring.mode = engine::command::SurroundMonitoring::virtualizer;
    REQUIRE(rig.engine.post(monitoring));
    rig.play();
    rig.render(0.25);
    CHECK(rig.engine.collectTelemetry().surroundRendering == engine::SurroundRendering::virtualizer);
    CHECK(std::abs(rig.level(5)) < 1e-9); // (the virtualizer feeds outputs 1 and 2 only)

    // An offline render of the same graph: every channel to its own output.
    REQUIRE(rig.engine.beginOfflineRender());
    rig.engine.applyCommandsOffline();
    rig.engine.setOfflineTransport(0.0, true, true);
    std::vector<std::vector<float>> buffers(12, std::vector<float>(kBlock));
    std::vector<float*> pointers;
    for (auto& b : buffers)
        pointers.push_back(b.data());
    for (int block = 0; block < 40; ++block)
        rig.engine.renderOffline(pointers.data(), 12, kBlock);
    rig.engine.endOfflineRender();
    for (int ch = 0; ch < 12; ++ch)
        CHECK_THAT(buffers[static_cast<std::size_t>(ch)][kBlock / 2], WithinAbs(0.05 * (ch + 1), 1e-5));
}

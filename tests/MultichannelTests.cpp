#include "devices/BuiltinProcessor.h"
#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "core/rt/RealtimeGuard.h"
#include "engine/ChannelLayout.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    auto processor = devices::createBuiltinDevice(device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

double gainOf(const engine::ChannelMatrix& m, const ChannelLayout& to, Speaker out, const ChannelLayout& from, Speaker in)
{
    return m.gain[static_cast<std::size_t>(to.indexOf(out))][static_cast<std::size_t>(from.indexOf(in))];
}

/** A session on a real engine with `outputs` device outputs and `inputs` device inputs. */
struct Rig
{
    explicit Rig(int outputs, int inputs = 0)
        : engine(2)
        , devices(session, createDevice)
        , numOutputs(outputs)
        , numInputs(inputs)
    {
        engine.prepare(kRate, kBlock, outputs, inputs);
        devices.setPlayHead(engine.getPlayHead());
        devices.prepareAll(kRate, kBlock);
        builder.setRenderFormat(kRate, kBlock);
        out.resize(static_cast<std::size_t>(outputs));
    }

    void publish()
    {
        auto graph = builder.build(session, assets, &devices);
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
    }

    void play()
    {
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
    }

    /** Renders `seconds`, the device inputs carrying `inputValues` (constant per channel). */
    void render(double seconds, const std::vector<float>& inputValues = {})
    {
        for (auto& channel : out)
            channel.clear();
        std::vector<std::vector<float>> buffers(static_cast<std::size_t>(numOutputs), std::vector<float>(kBlock));
        std::vector<float*> pointers;
        for (auto& b : buffers)
            pointers.push_back(b.data());
        std::vector<std::vector<float>> inputBuffers(static_cast<std::size_t>(numInputs), std::vector<float>(kBlock));
        std::vector<const float*> inputPointers;
        for (std::size_t ch = 0; ch < inputBuffers.size(); ++ch)
        {
            std::fill(inputBuffers[ch].begin(), inputBuffers[ch].end(), ch < inputValues.size() ? inputValues[ch] : 0.0f);
            inputPointers.push_back(inputBuffers[ch].data());
        }

        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(numInputs > 0 ? inputPointers.data() : nullptr, numInputs, pointers.data(), numOutputs, kBlock);
            for (std::size_t ch = 0; ch < buffers.size(); ++ch)
                out[ch].insert(out[ch].end(), buffers[ch].begin(), buffers[ch].end());
            engine.releaseRetiredGraphs();
        }
    }

    /** Output `channel` in the middle of the render (after every fade and ramp). */
    double level(int channel) const { return out[static_cast<std::size_t>(channel)][out[0].size() / 2]; }

    juce::ValueTree trackWith(const juce::File& file, TrackFormat format)
    {
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = session.addTrack(TrackKind::audio, "Track");
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
    int numInputs;
    std::vector<std::vector<float>> out;
};

/** A file whose channel k holds the constant 0.1 * (k + 1). */
juce::File steps(const juce::File& file, int numChannels, double seconds = 1.0)
{
    std::vector<std::vector<float>> channels;
    for (int k = 0; k < numChannels; ++k)
        channels.emplace_back(static_cast<std::size_t>(seconds * kRate), 0.1f * static_cast<float>(k + 1));
    return test::writeMultichannelWav(file, channels, kRate);
}
} // namespace

TEST_CASE("Track formats: names, channel counts and speaker orders", "[session][multichannel]")
{
    CHECK(getAllTrackFormats().size() == 7);
    CHECK(getNumChannels(TrackFormat::mono) == 1);
    CHECK(getNumChannels(TrackFormat::stereo) == 2);
    CHECK(getNumChannels(TrackFormat::lcrLfe) == 4);
    CHECK(getNumChannels(TrackFormat::surround51) == 6);
    CHECK(getNumChannels(TrackFormat::surround71) == 8);
    CHECK(getNumChannels(TrackFormat::dolbyDigital) == 6);
    CHECK(getNumChannels(TrackFormat::dts) == 6);
    CHECK(getDisplayName(TrackFormat::dolbyDigital) == "Dolby Digital 5.1");
    for (const auto format : getAllTrackFormats())
        CHECK(trackFormatFromString(toString(format)) == format);

    // Dolby Digital: L C R Ls Rs LFE; DTS: L R Ls Rs C LFE.
    const auto dd = getLayout(TrackFormat::dolbyDigital);
    CHECK(dd.indexOf(Speaker::centre) == 1);
    CHECK(dd.indexOf(Speaker::lfe) == 5);
    const auto dts = getLayout(TrackFormat::dts);
    CHECK(dts.indexOf(Speaker::leftSurround) == 2);
    CHECK(dts.indexOf(Speaker::centre) == 4);
    CHECK(getLayout(TrackFormat::surround71).indexOf(Speaker::rightRear) == 7);

    // Saved with the session; the master is never mono.
    Session session;
    auto t = session.addTrack(TrackKind::audio, "Surround");
    track::setFormat(t, TrackFormat::surround71, nullptr);
    CHECK(track::getFormat(juce::ValueTree::fromXml(*t.createXml())) == TrackFormat::surround71);
    CHECK(session.getMasterFormat() == TrackFormat::stereo);
    session.beginTransaction("Master Format");
    session.setMasterFormat(TrackFormat::dts);
    CHECK(session.getMasterFormat() == TrackFormat::dts);
    session.beginTransaction("Master Format");
    session.setMasterFormat(TrackFormat::mono);
    CHECK(session.getMasterFormat() == TrackFormat::stereo);
    session.getUndoManager().undo();
    CHECK(session.getMasterFormat() == TrackFormat::dts);
}

TEST_CASE("Layouts convert by speaker: fold-downs, reordering and channel groups", "[engine][multichannel]")
{
    const auto stereo = ChannelLayout::stereo();
    const auto s51 = ChannelLayout::surround51();
    const auto s71 = ChannelLayout::surround71();
    const auto dd = ChannelLayout::dolbyDigital();

    CHECK(engine::conversionMatrix(stereo, stereo).isIdentity());
    CHECK(engine::conversionMatrix(s51, s51).isIdentity());

    // 5.1 into stereo (ITU): centre and surrounds at -3 dB, no LFE.
    const auto down = engine::conversionMatrix(s51, stereo);
    CHECK_THAT(gainOf(down, stereo, Speaker::left, s51, Speaker::left), WithinAbs(1.0, 1e-12));
    CHECK_THAT(gainOf(down, stereo, Speaker::left, s51, Speaker::centre), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(down, stereo, Speaker::right, s51, Speaker::centre), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(down, stereo, Speaker::left, s51, Speaker::leftSurround), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(down, stereo, Speaker::right, s51, Speaker::leftSurround), WithinAbs(0.0, 1e-12));
    CHECK_THAT(gainOf(down, stereo, Speaker::left, s51, Speaker::lfe) + gainOf(down, stereo, Speaker::right, s51, Speaker::lfe),
               WithinAbs(0.0, 1e-12));

    // Same speakers in another order: a pure reordering.
    const auto reorder = engine::conversionMatrix(dd, s51);
    for (int o = 0; o < 6; ++o)
    {
        double sum = 0.0;
        for (int i = 0; i < 6; ++i)
            sum += reorder.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)];
        CHECK_THAT(sum, WithinAbs(1.0, 1e-12));
    }
    CHECK_THAT(gainOf(reorder, s51, Speaker::centre, dd, Speaker::centre), WithinAbs(1.0, 1e-12));
    CHECK_THAT(gainOf(reorder, s51, Speaker::lfe, dd, Speaker::lfe), WithinAbs(1.0, 1e-12));

    // 7.1 into 5.1: the rears join the surrounds at -3 dB; 5.1 into 7.1: nothing is lost.
    const auto seven = engine::conversionMatrix(s71, s51);
    CHECK_THAT(gainOf(seven, s51, Speaker::leftSurround, s71, Speaker::leftRear), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(gainOf(seven, s51, Speaker::leftSurround, s71, Speaker::leftSurround), WithinAbs(1.0, 1e-12));
    const auto up = engine::conversionMatrix(s51, s71);
    CHECK_THAT(gainOf(up, s71, Speaker::rightSurround, s51, Speaker::rightSurround), WithinAbs(1.0, 1e-12));

    // Channel groups: speaker pairs, then the centre and LFE alone; the front pair first.
    const auto groups = engine::channelGroupsOf(s51);
    REQUIRE(groups.count == 4);
    CHECK((groups.groups[0].first == 0 && groups.groups[0].second == 1));
    CHECK((groups.groups[1].first == 4 && groups.groups[1].second == 5));
    CHECK(groups.groups[2].second == -1);
    CHECK(groups.groups[3].second == -1);
    CHECK(engine::channelGroupsOf(s71).count == 5);
    CHECK(engine::channelGroupsOf(ChannelLayout::lcrLfe()).count == 3);
    const auto ddGroups = engine::channelGroupsOf(dd);
    CHECK((ddGroups.groups[0].first == 0 && ddGroups.groups[0].second == 2)); // L and R, around the centre
    CHECK(engine::channelGroupsOf(stereo).count == 1);
}

TEST_CASE("The surround panner moves a signal front to rear and into the centre", "[engine][multichannel]")
{
    const auto s51 = ChannelLayout::surround51();
    const auto s71 = ChannelLayout::surround71();
    const auto stereo = ChannelLayout::stereo();
    const auto g = [](const engine::ChannelMatrix& m, const ChannelLayout& to, Speaker out, int in)
    { return m.gain[static_cast<std::size_t>(to.indexOf(out))][static_cast<std::size_t>(in)]; };

    // Front: left and right speakers only.
    auto m = engine::surroundPanMatrix(s51, 0.0, -1.0, 0.0);
    CHECK_THAT(g(m, s51, Speaker::left, 0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::right, 1), WithinAbs(1.0, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::leftSurround, 0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::centre, 0), WithinAbs(0.0, 1e-12));

    // Rear and halfway (equal power).
    m = engine::surroundPanMatrix(s51, 0.0, 1.0, 0.0);
    CHECK_THAT(g(m, s51, Speaker::leftSurround, 0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::left, 0), WithinAbs(0.0, 1e-12));
    m = engine::surroundPanMatrix(s51, 0.0, 0.0, 0.0);
    CHECK_THAT(g(m, s51, Speaker::left, 0), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::leftSurround, 0), WithinAbs(kMinus3dB, 1e-12));

    // Centre: a centred signal moves into the centre speaker; a hard-panned one stays put.
    m = engine::surroundPanMatrix(s51, 0.0, -1.0, 1.0);
    CHECK_THAT(g(m, s51, Speaker::left, 0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::centre, 0), WithinAbs(kMinus3dB, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::centre, 1), WithinAbs(kMinus3dB, 1e-12));
    m = engine::surroundPanMatrix(s51, -1.0, -1.0, 1.0);
    CHECK_THAT(g(m, s51, Speaker::left, 0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::centre, 0), WithinAbs(0.0, 1e-12));
    CHECK_THAT(g(m, s51, Speaker::right, 1), WithinAbs(0.0, 1e-12)); // (balance: the right side is off)

    // 7.1: front, side (0) and rear (+1); the LFE never gets the panned signal.
    m = engine::surroundPanMatrix(s71, 0.0, 0.0, 0.0);
    CHECK_THAT(g(m, s71, Speaker::leftSurround, 0), WithinAbs(1.0, 1e-12));
    m = engine::surroundPanMatrix(s71, 0.0, 1.0, 0.0);
    CHECK_THAT(g(m, s71, Speaker::leftRear, 0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(g(m, s71, Speaker::lfe, 0) + g(m, s71, Speaker::lfe, 1), WithinAbs(0.0, 1e-12));

    // Into stereo it is the plain balance pan.
    m = engine::surroundPanMatrix(stereo, 0.5, 1.0, 1.0);
    CHECK_THAT(g(m, stereo, Speaker::left, 0), WithinAbs(0.5, 1e-12));
    CHECK_THAT(g(m, stereo, Speaker::right, 1), WithinAbs(1.0, 1e-12));
}

TEST_CASE("A 5.1 track plays its six channels to a 5.1 master, or folded down to stereo", "[engine][multichannel]")
{
    test::TempFolder temp;
    const auto file = steps(temp.dir.getChildFile("six.wav"), 6);

    SECTION("six outputs: one per channel")
    {
        Rig rig(6);
        rig.session.setMasterFormat(TrackFormat::surround51);
        auto t = rig.trackWith(file, TrackFormat::surround51);
        rig.publish();
        rig.play();
        rig.render(0.5);
        for (int ch = 0; ch < 6; ++ch)
            CHECK_THAT(rig.level(ch), WithinAbs(0.1 * (ch + 1), 1e-5));
        CHECK(rig.builder.getSlot(track::getId(t)) >= engine::kFirstWideSlot);

        // The track's meter has six channels.
        const auto peaks = rig.engine.collectTrackPeaks(rig.builder.getSlot(track::getId(t)));
        for (int ch = 0; ch < 6; ++ch)
            CHECK_THAT(peaks.peak[static_cast<std::size_t>(ch)], WithinAbs(0.1 * (ch + 1), 1e-4));
        const auto telemetry = rig.engine.collectTelemetry();
        CHECK(telemetry.masterChannels == 6);
        CHECK_THAT(telemetry.masterPeaks[5], WithinAbs(0.6, 1e-4));

        // Volume and balance act on every channel (balance on the left/right ones).
        track::setVolumeDb(t, -6.0, nullptr);
        track::setPan(t, -0.5, nullptr);
        rig.publish();
        rig.render(0.5);
        const double v = std::pow(10.0, -6.0 / 20.0);
        CHECK_THAT(rig.level(0), WithinAbs(0.1 * v, 1e-5));       // L
        CHECK_THAT(rig.level(1), WithinAbs(0.2 * v * 0.5, 1e-5)); // R, balanced away
        CHECK_THAT(rig.level(2), WithinAbs(0.3 * v, 1e-5));       // C
        CHECK_THAT(rig.level(5), WithinAbs(0.6 * v * 0.5, 1e-5)); // Rs
    }

    SECTION("a stereo master: the track folds down")
    {
        Rig rig(2);
        rig.trackWith(file, TrackFormat::surround51);
        rig.publish();
        rig.play();
        rig.render(0.5);
        CHECK_THAT(rig.level(0), WithinAbs(0.1 + kMinus3dB * 0.3 + kMinus3dB * 0.5, 1e-5));
        CHECK_THAT(rig.level(1), WithinAbs(0.2 + kMinus3dB * 0.3 + kMinus3dB * 0.6, 1e-5));
    }

    SECTION("a 5.1 master on a stereo device: the output is folded down")
    {
        Rig rig(2);
        rig.session.setMasterFormat(TrackFormat::surround51);
        rig.trackWith(file, TrackFormat::surround51);
        rig.publish();
        rig.play();
        rig.render(0.5);
        CHECK_THAT(rig.level(0), WithinAbs(0.1 + kMinus3dB * 0.3 + kMinus3dB * 0.5, 1e-5));
        CHECK_THAT(rig.level(1), WithinAbs(0.2 + kMinus3dB * 0.3 + kMinus3dB * 0.6, 1e-5));
    }
}

TEST_CASE("Dolby Digital and DTS tracks reach a 5.1 master speaker by speaker", "[engine][multichannel]")
{
    test::TempFolder temp;
    const auto file = steps(temp.dir.getChildFile("six.wav"), 6);
    const auto smpte = ChannelLayout::surround51();

    for (const auto format : {TrackFormat::dolbyDigital, TrackFormat::dts})
    {
        Rig rig(6);
        rig.session.setMasterFormat(TrackFormat::surround51);
        rig.trackWith(file, format); // (a six-channel file on a six-channel track: in the track's order)
        rig.publish();
        rig.play();
        rig.render(0.5);
        const auto layout = getLayout(format);
        for (int ch = 0; ch < 6; ++ch)
        {
            // The file's channel ch is the track's speaker ch: it comes out of that speaker of the master.
            const int output = smpte.indexOf(layout.speakers[static_cast<std::size_t>(ch)]);
            CHECK_THAT(rig.level(output), WithinAbs(0.1 * (ch + 1), 1e-5));
        }
    }
}

TEST_CASE("Mono and stereo tracks are panned into a surround master", "[engine][multichannel]")
{
    test::TempFolder temp;
    const auto mono = steps(temp.dir.getChildFile("mono.wav"), 1); // 0.1
    const auto s51 = ChannelLayout::surround51();

    Rig rig(6);
    rig.session.setMasterFormat(TrackFormat::surround51);
    auto t = rig.trackWith(mono, TrackFormat::mono);
    rig.publish();
    rig.play();
    rig.render(0.3);
    CHECK_THAT(rig.level(s51.indexOf(Speaker::left)), WithinAbs(0.1, 1e-6)); // front by default
    CHECK_THAT(rig.level(s51.indexOf(Speaker::right)), WithinAbs(0.1, 1e-6));
    CHECK_THAT(rig.level(s51.indexOf(Speaker::leftSurround)), WithinAbs(0.0, 1e-6));
    CHECK_THAT(rig.level(s51.indexOf(Speaker::centre)), WithinAbs(0.0, 1e-6));

    track::setPanCentre(t, 1.0, nullptr); // dialogue: the centre speaker
    rig.publish();
    rig.render(0.3);
    CHECK_THAT(rig.level(s51.indexOf(Speaker::centre)), WithinAbs(0.1 * std::sqrt(2.0), 1e-6));
    CHECK_THAT(rig.level(s51.indexOf(Speaker::left)), WithinAbs(0.0, 1e-6));

    track::setPanCentre(t, 0.0, nullptr);
    track::setPanFrontRear(t, 1.0, nullptr); // all the way back
    rig.publish();
    rig.render(0.3);
    CHECK_THAT(rig.level(s51.indexOf(Speaker::leftSurround)), WithinAbs(0.1, 1e-6));
    CHECK_THAT(rig.level(s51.indexOf(Speaker::rightSurround)), WithinAbs(0.1, 1e-6));
    CHECK_THAT(rig.level(s51.indexOf(Speaker::left)), WithinAbs(0.0, 1e-6));
    CHECK_THAT(rig.level(s51.indexOf(Speaker::lfe)), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Effects run on every channel of a multichannel track as linked instances", "[engine][multichannel][devices]")
{
    test::TempFolder temp;
    const auto file = steps(temp.dir.getChildFile("six.wav"), 6, 3.0);

    Rig rig(6);
    rig.session.setMasterFormat(TrackFormat::surround51);
    auto t = rig.trackWith(file, TrackFormat::surround51);
    auto utility = rig.session.addDevice(t, "builtin:utility", "Utility");
    auto node = rig.devices.getNode(utility);
    REQUIRE(node != nullptr);
    auto* processor = dynamic_cast<devices::BuiltinProcessor*>(&node->getProcessor());
    REQUIRE(processor != nullptr);
    auto* gain = processor->getParameterById("gain");
    gain->setValueNotifyingHost(gain->convertTo0to1(-6.0f));

    rig.publish();
    rig.play();
    rig.render(0.5);
    const double half = std::pow(10.0, -6.0 / 20.0);
    for (int ch = 0; ch < 6; ++ch) // the pairs and the centre and LFE alone
        CHECK_THAT(rig.level(ch), WithinAbs(0.1 * (ch + 1) * half, 1e-4));
    CHECK(rig.devices.getCompanions(utility, 3).size() == 3);

    // The companions follow the primary: one change reaches every channel.
    gain->setValueNotifyingHost(gain->convertTo0to1(-12.0f));
    rig.render(0.5);
    const double quarter = std::pow(10.0, -12.0 / 20.0);
    for (int ch = 0; ch < 6; ++ch)
        CHECK_THAT(rig.level(ch), WithinAbs(0.1 * (ch + 1) * quarter, 1e-4));

    // Bypassing the primary bypasses them all.
    rig.session.beginTransaction("Bypass");
    device::setBypassed(utility, true, nullptr);
    node->setBypassed(true);
    rig.render(0.5);
    for (int ch = 0; ch < 6; ++ch)
        CHECK_THAT(rig.level(ch), WithinAbs(0.1 * (ch + 1), 1e-4));
}

TEST_CASE("Busses of other formats: sends and routing convert by speaker", "[engine][multichannel][routing]")
{
    test::TempFolder temp;
    const auto file = steps(temp.dir.getChildFile("six.wav"), 6);

    // A 5.1 track routed to a stereo bus (on a stereo master): folded down on the way.
    {
        Rig rig(2);
        auto t = rig.trackWith(file, TrackFormat::surround51);
        auto bus = rig.session.addTrack(TrackKind::bus, "Stereo Bus");
        track::setOutput(t, track::getId(bus), nullptr);
        rig.publish();
        rig.play();
        rig.render(0.5);
        CHECK_THAT(rig.level(0), WithinAbs(0.1 + kMinus3dB * 0.3 + kMinus3dB * 0.5, 1e-5));
    }

    // A stereo track sending into a 5.1 bus: its left and right arrive at L and R.
    {
        Rig rig(6);
        rig.session.setMasterFormat(TrackFormat::surround51);
        const auto stereoFile = test::writeMultichannelWav(temp.dir.getChildFile("lr.wav"),
                                                           {std::vector<float>(48000, 0.25f), std::vector<float>(48000, 0.5f)}, kRate);
        auto t = rig.trackWith(stereoFile, TrackFormat::stereo);
        track::setVolumeDb(t, kMinusInfinityDb, nullptr); // only the pre-fader send is heard
        auto bus = rig.session.addTrack(TrackKind::bus, "Surround Reverb");
        track::setFormat(bus, TrackFormat::surround51, nullptr);
        auto s = rig.session.addSend(t, bus, 0.0);
        send::setPreFader(s, true, nullptr);
        rig.publish();
        rig.play();
        rig.render(0.5);
        const auto layout = ChannelLayout::surround51();
        CHECK_THAT(rig.level(layout.indexOf(Speaker::left)), WithinAbs(0.25, 1e-5));
        CHECK_THAT(rig.level(layout.indexOf(Speaker::right)), WithinAbs(0.5, 1e-5));
        CHECK_THAT(rig.level(layout.indexOf(Speaker::centre)), WithinAbs(0.0, 1e-6));
        // The stereo track feeds a 5.1 master, so it renders in a wide slot.
        CHECK(rig.builder.getSlot(track::getId(t)) >= engine::kFirstWideSlot);
    }
}

TEST_CASE("A multichannel track monitors one input per channel", "[engine][multichannel][recording]")
{
    Rig rig(6, 8);
    rig.session.setMasterFormat(TrackFormat::surround51);
    auto t = rig.session.addTrack(TrackKind::audio, "Room");
    track::setFormat(t, TrackFormat::surround51, nullptr);
    track::setInput(t, 2, true, nullptr); // inputs 3..8
    track::setMonitor(t, engine::MonitorMode::always, nullptr);
    rig.publish();
    rig.render(0.3, {0.9f, 0.9f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f});
    for (int ch = 0; ch < 6; ++ch)
        CHECK_THAT(rig.level(ch), WithinAbs(0.1 * (ch + 1), 1e-6));
}

TEST_CASE("Stretched and disk-streamed multichannel clips keep every channel", "[engine][multichannel][streaming]")
{
    test::TempFolder temp;
    std::vector<std::vector<float>> sines;
    for (int k = 0; k < 6; ++k)
        sines.push_back(test::sine(330.0, kRate, 4 * 48000, 0.1f * static_cast<float>(k + 1)));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("sines.wav"), sines, kRate);

    SECTION("time-stretched and pitched")
    {
        Rig rig(6);
        rig.session.setMasterFormat(TrackFormat::surround51);
        auto t = rig.trackWith(file, TrackFormat::surround51);
        auto c = t.getChildWithName(ids::clip);
        clip::setStretch(c, 1.5, nullptr);
        clip::setPitch(c, 2.0, nullptr);
        rig.publish();
        rig.play();
        rig.render(1.0);
        for (int ch = 0; ch < 6; ++ch)
        {
            const auto& out = rig.out[static_cast<std::size_t>(ch)];
            CHECK_THAT(test::rms(out, out.size() / 2, out.size()), WithinAbs(0.1 * (ch + 1) / std::sqrt(2.0), 0.02 * (ch + 1)));
        }
        // Two semitones up.
        const auto& left = rig.out[0];
        CHECK_THAT(test::estimateFrequency(left, left.size() / 2, left.size(), kRate), WithinAbs(330.0 * std::pow(2.0, 2.0 / 12.0), 6.0));
    }

    SECTION("streamed from disk")
    {
        Rig rig(6);
        rig.session.setMasterFormat(TrackFormat::surround51);
        rig.assets.setStreamingThreshold(1.0);
        rig.assets.setEpochSource([&] { return rig.engine.getProcessEpoch(); });
        rig.trackWith(file, TrackFormat::surround51);
        juce::String error;
        auto asset = rig.assets.loadNow(file, error);
        REQUIRE(asset->isStreamed());
        CHECK(asset->streamed->getNumChannels() == 6);
        rig.publish();
        rig.play();
        // Give the service thread time to load the region being played.
        std::vector<double> peaks(6, 0.0);
        for (int i = 0; i < 100 && peaks[5] < 0.59; ++i)
        {
            rig.render(0.02);
            for (int ch = 0; ch < 6; ++ch)
                peaks[static_cast<std::size_t>(ch)] = std::max(peaks[static_cast<std::size_t>(ch)], test::peak(rig.out[static_cast<std::size_t>(ch)]));
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        for (int ch = 0; ch < 6; ++ch)
            CHECK_THAT(peaks[static_cast<std::size_t>(ch)], WithinAbs(0.1 * (ch + 1), 0.005));
    }
}

TEST_CASE("Rendering multichannel tracks, linked devices and a surround master never touches the heap",
          "[engine][multichannel][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    test::TempFolder temp;
    const auto six = steps(temp.dir.getChildFile("six.wav"), 6, 2.0);
    const auto mono = steps(temp.dir.getChildFile("mono.wav"), 1, 2.0);

    Rig rig(8);
    rig.session.setMasterFormat(TrackFormat::surround71);
    auto surround = rig.trackWith(six, TrackFormat::surround51);
    rig.session.addDevice(surround, "builtin:eq", "EQ");
    rig.session.addDevice(surround, "builtin:compressor", "Comp");
    auto dialogue = rig.trackWith(mono, TrackFormat::mono);
    track::setPanCentre(dialogue, 0.7, nullptr);
    rig.session.addDevice(dialogue, "builtin:reverb", "Reverb");
    auto bus = rig.session.addTrack(TrackKind::bus, "Stereo FX");
    rig.session.addSend(surround, bus, -6.0, false);
    clip::setPitch(surround.getChildWithName(ids::clip), 1.0, nullptr);
    rig.session.addDevice(rig.session.getMaster(), "builtin:limiter", "Master Limiter");

    std::vector<std::unique_ptr<engine::RenderGraph>> graphs;
    for (int i = 0; i < 5; ++i)
        graphs.push_back(rig.builder.build(rig.session, rig.assets, &rig.devices));

    REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
    std::vector<std::vector<float>> buffers(8, std::vector<float>(kBlock));
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
        rig.engine.process(nullptr, 0, pointers.data(), 8, kBlock);
        rig.engine.releaseRetiredGraphs();
    }
    CHECK(rt::getRealtimeViolationCount() == before);
    CHECK(test::peak(buffers[0]) > 0.0f);
}

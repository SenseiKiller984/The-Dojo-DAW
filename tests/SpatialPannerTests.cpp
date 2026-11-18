#include "core/rt/RealtimeGuard.h"
#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/ChannelLayout.h"
#include "engine/SpatialPanner.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>

using namespace dojo;
using namespace dojo::session;
using engine::ChannelLayout;
using engine::SpatialPanner;
using engine::Speaker;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

std::vector<ChannelLayout> layouts()
{
    return {ChannelLayout::surround51(), ChannelLayout::surround71(), ChannelLayout::surround514(), ChannelLayout::surround714(),
            ChannelLayout::surround916()};
}

Speaker mirrorOf(Speaker s)
{
    switch (s)
    {
    case Speaker::left:           return Speaker::right;
    case Speaker::right:          return Speaker::left;
    case Speaker::leftSurround:   return Speaker::rightSurround;
    case Speaker::rightSurround:  return Speaker::leftSurround;
    case Speaker::leftRear:       return Speaker::rightRear;
    case Speaker::rightRear:      return Speaker::leftRear;
    case Speaker::leftWide:       return Speaker::rightWide;
    case Speaker::rightWide:      return Speaker::leftWide;
    case Speaker::topFrontLeft:   return Speaker::topFrontRight;
    case Speaker::topFrontRight:  return Speaker::topFrontLeft;
    case Speaker::topMiddleLeft:  return Speaker::topMiddleRight;
    case Speaker::topMiddleRight: return Speaker::topMiddleLeft;
    case Speaker::topRearLeft:    return Speaker::topRearRight;
    case Speaker::topRearRight:   return Speaker::topRearLeft;
    case Speaker::centre:
    case Speaker::lfe:            break;
    }
    return s;
}

double power(const SpatialPanner::Gains& g)
{
    double p = 0.0;
    for (const double v : g)
        p += v * v;
    return p;
}

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    auto processor = devices::createBuiltinDevice(device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
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
    void publish()
    {
        engine::command::SwapGraph swap;
        swap.graph = build().release();
        REQUIRE(engine.post(swap));
    }
    void play()
    {
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
    }
    void render(double seconds, const std::function<void()>& afterBlock = {})
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
            if (afterBlock)
                afterBlock();
        }
    }
    double level(int channel) const { return out[static_cast<std::size_t>(channel)][out[0].size() / 2]; }
    double at(int channel, double seconds) const
    {
        return out[static_cast<std::size_t>(channel)][static_cast<std::size_t>(seconds * kRate)];
    }

    juce::ValueTree trackWith(const juce::File& file, TrackFormat format)
    {
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = session.addTrack(TrackKind::audio, "Object");
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

double correlation(const std::vector<float>& a, const std::vector<float>& b, std::size_t from, std::size_t count)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = from; i < from + count; ++i)
    {
        ab += static_cast<double>(a[i]) * b[i];
        aa += static_cast<double>(a[i]) * a[i];
        bb += static_cast<double>(b[i]) * b[i];
    }
    return aa > 0.0 && bb > 0.0 ? ab / std::sqrt(aa * bb) : 0.0;
}
} // namespace

TEST_CASE("VBAP: each speaker plays alone from its own direction; gains keep constant power", "[engine][immersive][panner]")
{
    std::mt19937 random(7);
    std::uniform_real_distribution<double> azimuths(-180.0, 180.0), elevations(-90.0, 90.0);
    for (const auto& layout : layouts())
    {
        SpatialPanner panner(layout);
        CAPTURE(layout.numChannels);
        REQUIRE_FALSE(panner.getTriangles().empty());
        for (int c = 0; c < layout.numChannels; ++c)
        {
            const auto speaker = layout.speakers[static_cast<std::size_t>(c)];
            if (speaker == Speaker::lfe)
                continue;
            const auto d = engine::directionOf(layout, speaker);
            const auto g = panner.pointGains(d.azimuth, d.elevation);
            CAPTURE(engine::shortName(speaker));
            CHECK_THAT(g[static_cast<std::size_t>(c)], WithinAbs(1.0, 1e-9));
            CHECK_THAT(power(g), WithinAbs(1.0, 1e-9));
        }
        for (int k = 0; k < 300; ++k)
        {
            const auto g = panner.pointGains(azimuths(random), elevations(random));
            CHECK_THAT(power(g), WithinAbs(1.0, 1e-9));
            CHECK(g[static_cast<std::size_t>(layout.indexOf(Speaker::lfe))] == 0.0);
            CHECK(*std::min_element(g.begin(), g.end()) >= 0.0);
        }
    }

    // Inside a triangle of three real speakers the gains solve p = g1 l1 + g2 l2 + g3 l3.
    SpatialPanner panner(ChannelLayout::surround714());
    int checked = 0;
    for (const auto& t : panner.getTriangles())
    {
        const auto& v = panner.getVertices();
        if (v[static_cast<std::size_t>(t.vertex[0])].channel < 0 || v[static_cast<std::size_t>(t.vertex[1])].channel < 0
            || v[static_cast<std::size_t>(t.vertex[2])].channel < 0)
            continue;
        engine::Vec3 centre;
        for (const int i : t.vertex)
        {
            centre.x += v[static_cast<std::size_t>(i)].direction.x;
            centre.y += v[static_cast<std::size_t>(i)].direction.y;
            centre.z += v[static_cast<std::size_t>(i)].direction.z;
        }
        const auto [azimuth, elevation] = engine::directionAngles(centre);
        const auto g = panner.pointGains(azimuth, elevation);
        engine::Vec3 sum;
        for (const int i : t.vertex)
        {
            const auto& vertex = v[static_cast<std::size_t>(i)];
            const double gain = g[static_cast<std::size_t>(vertex.channel)];
            sum.x += gain * vertex.direction.x;
            sum.y += gain * vertex.direction.y;
            sum.z += gain * vertex.direction.z;
        }
        const double length = std::sqrt(sum.x * sum.x + sum.y * sum.y + sum.z * sum.z);
        const double centreLength = std::sqrt(centre.x * centre.x + centre.y * centre.y + centre.z * centre.z);
        CHECK_THAT((sum.x * centre.x + sum.y * centre.y + sum.z * centre.z) / (length * centreLength), WithinAbs(1.0, 1e-9));
        ++checked;
    }
    CHECK(checked >= 8);
}

TEST_CASE("VBAP: symmetric layouts pan symmetrically, overhead and underneath included", "[engine][immersive][panner]")
{
    std::mt19937 random(11);
    std::uniform_real_distribution<double> azimuths(-180.0, 180.0), elevations(-90.0, 90.0);
    for (const auto& layout : layouts())
    {
        SpatialPanner panner(layout);
        for (int k = 0; k < 200; ++k)
        {
            const double az = azimuths(random), el = elevations(random);
            const auto g = panner.pointGains(az, el);
            const auto mirrored = panner.pointGains(-az, el);
            for (int c = 0; c < layout.numChannels; ++c)
            {
                const int m = layout.indexOf(mirrorOf(layout.speakers[static_cast<std::size_t>(c)]));
                CHECK_THAT(g[static_cast<std::size_t>(c)], WithinAbs(mirrored[static_cast<std::size_t>(m)], 1e-9));
            }
        }
    }

    // Straight up: the top layer (or, with none, the ring) shares it; straight down: the ring.
    const auto b714 = ChannelLayout::surround714();
    SpatialPanner p714(b714);
    const auto up = p714.pointGains(0.0, 90.0);
    for (const auto top : {Speaker::topFrontLeft, Speaker::topFrontRight, Speaker::topRearLeft, Speaker::topRearRight})
        CHECK_THAT(up[static_cast<std::size_t>(b714.indexOf(top))], WithinAbs(0.5, 1e-9));
    CHECK_THAT(up[static_cast<std::size_t>(b714.indexOf(Speaker::centre))], WithinAbs(0.0, 1e-9));
    const auto down = p714.pointGains(0.0, -90.0);
    for (const auto ring : {Speaker::left, Speaker::right, Speaker::centre, Speaker::leftSurround, Speaker::rightRear})
        CHECK_THAT(down[static_cast<std::size_t>(b714.indexOf(ring))], WithinAbs(1.0 / std::sqrt(7.0), 1e-9));
    CHECK_THAT(down[static_cast<std::size_t>(b714.indexOf(Speaker::topFrontLeft))], WithinAbs(0.0, 1e-9));
    // Behind and a little up: the rears and the top rears, left and right alike.
    const auto behind = p714.pointGains(180.0, 15.0);
    CHECK(behind[static_cast<std::size_t>(b714.indexOf(Speaker::leftRear))] > 0.2);
    CHECK(behind[static_cast<std::size_t>(b714.indexOf(Speaker::topRearLeft))] > 0.2);

    const auto b916 = ChannelLayout::surround916();
    const auto up916 = SpatialPanner(b916).pointGains(0.0, 90.0);
    CHECK_THAT(up916[static_cast<std::size_t>(b916.indexOf(Speaker::topMiddleLeft))], WithinAbs(1.0 / std::sqrt(6.0), 1e-9));

    // A flat layout gets a virtual speaker straight up, shared by its five speakers.
    const auto s51 = ChannelLayout::surround51();
    const auto up51 = SpatialPanner(s51).pointGains(10.0, 90.0);
    CHECK_THAT(up51[static_cast<std::size_t>(s51.indexOf(Speaker::leftSurround))], WithinAbs(1.0 / std::sqrt(5.0), 1e-9));
}

TEST_CASE("VBAP: 3.1 fills the space behind; stereo pans along its arc", "[engine][immersive][panner]")
{
    const auto lcr = ChannelLayout::lcrLfe();
    SpatialPanner p(lcr);
    const auto behind = p.pointGains(180.0, 0.0);
    CHECK_THAT(behind[static_cast<std::size_t>(lcr.indexOf(Speaker::left))], WithinAbs(std::sqrt(0.5), 1e-9));
    CHECK_THAT(behind[static_cast<std::size_t>(lcr.indexOf(Speaker::right))], WithinAbs(std::sqrt(0.5), 1e-9));
    CHECK_THAT(behind[static_cast<std::size_t>(lcr.indexOf(Speaker::centre))], WithinAbs(0.0, 1e-9));
    const auto side = p.pointGains(90.0, 0.0);
    CHECK(side[static_cast<std::size_t>(lcr.indexOf(Speaker::left))] > side[static_cast<std::size_t>(lcr.indexOf(Speaker::right))]);

    SpatialPanner stereo(ChannelLayout::stereo());
    CHECK_THAT(stereo.pointGains(30.0, 0.0)[0], WithinAbs(1.0, 1e-9));
    CHECK_THAT(stereo.pointGains(-30.0, 0.0)[1], WithinAbs(1.0, 1e-9));
    CHECK_THAT(stereo.pointGains(0.0, 0.0)[0], WithinAbs(std::sqrt(0.5), 1e-9));
    CHECK_THAT(stereo.pointGains(180.0, 0.0)[1], WithinAbs(std::sqrt(0.5), 1e-9)); // (behind: folded to the front)
    CHECK_THAT(stereo.pointGains(90.0, 0.0)[0], WithinAbs(1.0, 1e-9));
    CHECK_THAT(stereo.pointGains(0.0, 90.0)[0], WithinAbs(std::sqrt(0.5), 1e-9));
}

TEST_CASE("Spread: a wider source plays from more speakers with the same power; distance surrounds", "[engine][immersive][panner]")
{
    const auto b714 = ChannelLayout::surround714();
    SpatialPanner p(b714);
    const auto c = static_cast<std::size_t>(b714.indexOf(Speaker::centre));
    const auto l = static_cast<std::size_t>(b714.indexOf(Speaker::left));
    const auto lfe = static_cast<std::size_t>(b714.indexOf(Speaker::lfe));

    CHECK_THAT(p.spreadGains(0.0, 0.0, 0.0)[c], WithinAbs(1.0, 1e-9));
    const auto some = p.spreadGains(0.0, 0.0, 0.25);
    CHECK_THAT(power(some), WithinAbs(1.0, 1e-9));
    CHECK(some[c] > some[l]);
    CHECK(some[l] > 0.1);
    const auto all = p.spreadGains(0.0, 0.0, 1.0);
    CHECK_THAT(power(all), WithinAbs(1.0, 1e-9));
    double lowest = 1.0, highest = 0.0;
    for (std::size_t ch = 0; ch < 12; ++ch)
    {
        if (ch == lfe)
            continue;
        lowest = std::min(lowest, all[ch]);
        highest = std::max(highest, all[ch]);
    }
    CHECK(lowest > 0.05);
    CHECK(highest / lowest < 4.0);
    // Wider and wider: the centre's share falls steadily.
    double previous = 2.0;
    for (const double spread : {0.0, 0.2, 0.4, 0.6, 0.8, 1.0})
    {
        const double share = p.spreadGains(0.0, 0.0, spread)[c];
        CHECK(share < previous + 1e-9);
        previous = share;
    }

    CHECK_THAT(SpatialPanner::effectiveSpread(0.2, 1.0), WithinAbs(0.2, 1e-12));
    CHECK_THAT(SpatialPanner::effectiveSpread(0.2, 0.0), WithinAbs(1.0, 1e-12));
    CHECK_THAT(SpatialPanner::effectiveSpread(0.0, 0.5), WithinAbs(0.5, 1e-12));
}

TEST_CASE("Decorrelation filters: every channel keeps the spectrum, the channels are uncorrelated", "[engine][immersive][panner]")
{
    engine::Decorrelator decorrelator;
    decorrelator.prepare(kRate, 12);
    REQUIRE(decorrelator.getNumChannels() == 12);
    std::mt19937 random(3);
    std::normal_distribution<double> noise(0.0, 0.3);
    std::vector<double> input(48'000);
    for (auto& v : input)
        v = noise(random);
    std::vector<std::vector<float>> outputs(12, std::vector<float>(input.size()));
    std::vector<float> in(input.begin(), input.end());
    for (int c = 0; c < 12; ++c)
    {
        std::vector<double> out(input.size());
        decorrelator.process(c, input.data(), out.data(), static_cast<int>(input.size()));
        std::copy(out.begin(), out.end(), outputs[static_cast<std::size_t>(c)].begin());
        double inPower = 0.0, outPower = 0.0;
        for (std::size_t i = 4800; i < input.size(); ++i)
        {
            inPower += input[i] * input[i];
            outPower += out[i] * out[i];
        }
        CHECK_THAT(outPower / inPower, WithinAbs(1.0, 0.08)); // (all-pass)
        CHECK(std::abs(correlation(outputs[static_cast<std::size_t>(c)], in, 4800, 40'000)) < 0.75);
    }
    for (int a = 0; a < 12; ++a)
        for (int b = a + 1; b < 12; ++b)
            CHECK(std::abs(correlation(outputs[static_cast<std::size_t>(a)], outputs[static_cast<std::size_t>(b)], 4800, 40'000)) < 0.35);
}

TEST_CASE("3D panner settings: saved with the track, azimuth wrapped, the rest clamped; lanes map to them",
          "[session][immersive][panner]")
{
    Session session;
    auto t = session.addTrack(TrackKind::audio, "Object");
    CHECK_FALSE(track::isPan3d(t));
    CHECK(track::getDistance(t) == 1.0);
    track::setPan3d(t, true, nullptr);
    track::setAzimuth(t, 190.0, nullptr);
    track::setElevation(t, 120.0, nullptr);
    track::setDistance(t, -0.5, nullptr);
    track::setSize(t, 0.25, nullptr);
    CHECK_THAT(track::getAzimuth(t), WithinAbs(-170.0, 1e-9));
    CHECK(track::getElevation(t) == 90.0);
    CHECK(track::getDistance(t) == 0.0);
    const auto copy = juce::ValueTree::fromXml(*t.createXml());
    CHECK(track::isPan3d(copy));
    CHECK_THAT(track::getAzimuth(copy), WithinAbs(-170.0, 1e-9));
    CHECK(track::getSize(copy) == 0.25);

    using namespace automation;
    CHECK(isSpatialTarget(azimuthTarget()));
    CHECK_FALSE(isSpatialTarget(panTarget()));
    CHECK(spatialToNormalised(azimuthTarget(), 0.0) == 0.5);
    CHECK(spatialToNormalised(elevationTarget(), 90.0) == 1.0);
    CHECK(spatialFromNormalised(azimuthTarget(), 0.75) == 90.0);
    CHECK(spatialFromNormalised(elevationTarget(), 0.25) == -45.0);
    CHECK(spatialFromNormalised(sizeTarget(), 0.4) == 0.4);
}

TEST_CASE("The 3D panner in the engine: where a track plays, automation and LFOs moving it, size", "[engine][immersive][panner]")
{
    test::TempFolder temp;
    const auto b714 = ChannelLayout::surround714();
    const auto mono = test::writeMultichannelWav(temp.dir.getChildFile("mono.wav"),
                                                 {std::vector<float>(static_cast<std::size_t>(3.0 * kRate), 0.5f)}, kRate);

    SECTION("a mono track at a position plays from the speakers VBAP gives it")
    {
        Rig rig(12);
        rig.session.setMasterFormat(TrackFormat::surround714);
        auto t = rig.trackWith(mono, TrackFormat::mono);
        track::setPan3d(t, true, nullptr);
        SpatialPanner panner(b714);
        for (const auto& [az, el] : std::vector<std::pair<double, double>>{{30.0, 0.0}, {45.0, 30.0}, {0.0, 90.0}, {120.0, 20.0}, {-100.0, -20.0}})
        {
            track::setAzimuth(t, az, nullptr);
            track::setElevation(t, el, nullptr);
            rig.publish();
            rig.play();
            rig.render(0.25);
            const auto expected = panner.pointGains(az, el);
            CAPTURE(az, el);
            for (int ch = 0; ch < 12; ++ch)
                CHECK_THAT(rig.level(ch), WithinAbs(0.5 * expected[static_cast<std::size_t>(ch)], 1e-5));
        }
        // Off again: the surround pad's front-centre placement (L and R).
        track::setPan3d(t, false, nullptr);
        rig.publish();
        rig.render(0.25);
        CHECK_THAT(rig.level(b714.indexOf(Speaker::left)), WithinAbs(0.5, 1e-5));
        CHECK_THAT(rig.level(b714.indexOf(Speaker::topFrontLeft)), WithinAbs(0.0, 1e-6));
    }

    SECTION("a stereo track's sides sit 30 degrees either side of its position")
    {
        std::vector<std::vector<float>> two{std::vector<float>(static_cast<std::size_t>(kRate), 0.3f),
                                            std::vector<float>(static_cast<std::size_t>(kRate), 0.6f)};
        const auto stereo = test::writeMultichannelWav(temp.dir.getChildFile("stereo.wav"), two, kRate);
        Rig rig(12);
        rig.session.setMasterFormat(TrackFormat::surround714);
        auto t = rig.trackWith(stereo, TrackFormat::stereo);
        track::setPan3d(t, true, nullptr);
        rig.publish();
        rig.play();
        rig.render(0.5);
        CHECK_THAT(rig.level(b714.indexOf(Speaker::left)), WithinAbs(0.3, 1e-5));
        CHECK_THAT(rig.level(b714.indexOf(Speaker::right)), WithinAbs(0.6, 1e-5));
        CHECK_THAT(rig.level(b714.indexOf(Speaker::centre)), WithinAbs(0.0, 1e-6));
    }

    SECTION("automation lanes move it")
    {
        Rig rig(12);
        rig.session.setMasterFormat(TrackFormat::surround714);
        auto t = rig.trackWith(mono, TrackFormat::mono);
        track::setPan3d(t, true, nullptr);
        // Azimuth 0 until 0.5 s, then 90 (the left side); elevation 0 until 1.5 s, then 30.
        auto azimuth = rig.session.getAutomation(t, automation::azimuthTarget(), true, 0.5);
        rig.session.addAutomationPoint(azimuth, 0.5, 0.5);
        rig.session.addAutomationPoint(azimuth, 0.5001, automation::spatialToNormalised(automation::azimuthTarget(), 90.0));
        auto elevation = rig.session.getAutomation(t, automation::elevationTarget(), true, 0.5);
        rig.session.addAutomationPoint(elevation, 1.5, 0.5);
        rig.session.addAutomationPoint(elevation, 1.5001, automation::spatialToNormalised(automation::elevationTarget(), 30.0));
        rig.publish();
        rig.play();
        const int slot = rig.builder.getSlot(track::getId(t));
        float latestAzimuth = 0.0f;
        rig.render(2.0, [&] { latestAzimuth = rig.engine.getMovingSpatial(slot, 0); });
        CHECK_THAT(rig.at(b714.indexOf(Speaker::centre), 0.25), WithinAbs(0.5, 1e-5));
        CHECK_THAT(rig.at(b714.indexOf(Speaker::leftSurround), 1.0), WithinAbs(0.5, 1e-5)); // (Ls at 90 in 7.1.4)
        const auto above = SpatialPanner(b714).pointGains(90.0, 30.0);
        CHECK_THAT(rig.at(b714.indexOf(Speaker::topFrontLeft), 1.9), WithinAbs(0.5 * above[static_cast<std::size_t>(b714.indexOf(Speaker::topFrontLeft))], 1e-5));
        CHECK_THAT(latestAzimuth, WithinAbs(90.0, 0.01)); // (the engine reports where it is)
        CHECK(std::isnan(rig.engine.getMovingSpatial(slot, 2))); // (distance: its own setting)
    }

    SECTION("an LFO sweeps its azimuth")
    {
        Rig rig(12);
        rig.session.setMasterFormat(TrackFormat::surround714);
        auto t = rig.trackWith(mono, TrackFormat::mono);
        track::setPan3d(t, true, nullptr);
        auto lfo = rig.session.addDevice(t, "builtin:lfo", "LFO");
        REQUIRE(rig.session.addModulation(lfo, track::getId(t), automation::azimuthTarget(), 0.5).isValid());
        auto graph = rig.build();
        REQUIRE(graph->modulators.size() == 1);
        REQUIRE(graph->modulators.front().targets.size() == 1);
        CHECK(graph->modulators.front().targets.front().kind == engine::ModulationTarget::Kind::azimuth);
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(rig.engine.post(swap));
        rig.play();
        const int slot = rig.builder.getSlot(track::getId(t));
        float lowest = 1000.0f, highest = -1000.0f;
        rig.render(2.0, [&]
                   {
                       const float az = rig.engine.getMovingSpatial(slot, 0);
                       REQUIRE_FALSE(std::isnan(az));
                       lowest = std::min(lowest, az);
                       highest = std::max(highest, az);
                   });
        CHECK(lowest < -150.0f);
        CHECK(highest > 150.0f);
    }

    SECTION("size: its share of the power plays diffuse, through each speaker's own filter")
    {
        std::mt19937 random(5);
        std::normal_distribution<float> noise(0.0f, 0.2f);
        std::vector<float> samples(static_cast<std::size_t>(2.0 * kRate));
        for (auto& v : samples)
            v = noise(random);
        const auto noiseFile = test::writeMultichannelWav(temp.dir.getChildFile("noise.wav"), {samples}, kRate);

        Rig rig(12);
        rig.session.setMasterFormat(TrackFormat::surround714);
        auto t = rig.trackWith(noiseFile, TrackFormat::mono);
        track::setPan3d(t, true, nullptr);
        track::setAzimuth(t, 180.0, nullptr); // (behind: the rears, in phase)
        rig.publish();
        rig.play();
        rig.render(1.0);
        const auto lrs = static_cast<std::size_t>(b714.indexOf(Speaker::leftRear));
        const auto rrs = static_cast<std::size_t>(b714.indexOf(Speaker::rightRear));
        CHECK(correlation(rig.out[lrs], rig.out[rrs], 4800, 30'000) > 0.99);

        track::setSize(t, 1.0, nullptr);
        rig.publish();
        rig.render(1.0);
        CHECK(std::abs(correlation(rig.out[lrs], rig.out[rrs], 4800, 30'000)) < 0.35);
        // ... from all around (the whole sphere), at the same total power.
        double outPower = 0.0;
        for (std::size_t ch = 0; ch < 12; ++ch)
        {
            double p = 0.0;
            for (std::size_t i = 9600; i < 40'000; ++i)
                p += static_cast<double>(rig.out[ch][i]) * rig.out[ch][i];
            outPower += p;
            if (ch != static_cast<std::size_t>(b714.indexOf(Speaker::lfe)))
                CHECK(p > 0.0);
        }
        double inPower = 0.0;
        for (std::size_t i = 9600; i < 40'000; ++i)
            inPower += static_cast<double>(samples[i + static_cast<std::size_t>(kRate)]) * samples[i + static_cast<std::size_t>(kRate)];
        CHECK_THAT(outPower / inPower, WithinAbs(1.0, 0.1));
    }
}

TEST_CASE("Rendering 3D-panned tracks (moving, spread, diffuse) never touches the heap", "[engine][immersive][panner][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    test::TempFolder temp;
    std::vector<float> samples(static_cast<std::size_t>(2.0 * kRate));
    for (std::size_t i = 0; i < samples.size(); ++i)
        samples[i] = 0.3f * static_cast<float>(std::sin(0.03 * static_cast<double>(i)));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("tone.wav"), {samples, samples}, kRate);

    Rig rig(16);
    rig.session.setMasterFormat(TrackFormat::surround916);
    for (int i = 0; i < 3; ++i)
    {
        auto t = rig.trackWith(file, i == 0 ? TrackFormat::mono : TrackFormat::stereo);
        track::setPan3d(t, true, nullptr);
        track::setSize(t, 0.3 * i, nullptr);
        track::setDistance(t, 1.0 - 0.3 * i, nullptr);
        auto lane = rig.session.getAutomation(t, automation::azimuthTarget(), true, 0.5);
        rig.session.addAutomationPoint(lane, 0.0, 0.0);
        rig.session.addAutomationPoint(lane, 2.0, 1.0);
    }
    std::vector<std::unique_ptr<engine::RenderGraph>> graphs;
    for (int i = 0; i < 3; ++i)
        graphs.push_back(rig.build());

    REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
    std::vector<std::vector<float>> buffers(16, std::vector<float>(kBlock));
    std::vector<float*> pointers;
    for (auto& b : buffers)
        pointers.push_back(b.data());
    const auto before = rt::getRealtimeViolationCount();
    for (int block = 0; block < 300; ++block)
    {
        if (block % 100 == 0)
        {
            engine::command::SwapGraph swap;
            swap.graph = graphs[static_cast<std::size_t>(block / 100)].release();
            REQUIRE(rig.engine.post(swap));
        }
        rig.engine.process(nullptr, 0, pointers.data(), 16, kBlock);
        rig.engine.releaseRetiredGraphs();
    }
    CHECK(rt::getRealtimeViolationCount() == before);
    CHECK(test::peak(buffers[0]) + test::peak(buffers[10]) > 0.0f);
}

#include "core/rt/RealtimeGuard.h"
#include "engine/AudioEngine.h"
#include "engine/dsp/ClipRenderer.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>
#include <vector>

using namespace dojo;
using namespace dojo::engine;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48'000.0;

/** Decoded audio stand-in: owns sample data and exposes it the way a RenderClip expects. */
struct FakeAsset
{
    FakeAsset(int numChannels, int numFrames, float value)
        : data(static_cast<std::size_t>(numChannels),
               std::vector<float>(static_cast<std::size_t>(numFrames), value))
    {
        for (auto& channel : data)
            pointers.push_back(channel.data());
    }

    std::vector<std::vector<float>> data;
    std::vector<const float*> pointers;
};

RenderClip makeClip(const std::shared_ptr<FakeAsset>& asset, double start, double length, double sourceRate)
{
    RenderClip clip;
    clip.startSeconds = start;
    clip.endSeconds = start + length;
    clip.channels = asset->pointers.data();
    clip.numChannels = static_cast<int>(asset->pointers.size());
    clip.sourceLength = static_cast<SampleCount>(asset->data[0].size());
    clip.sourceSampleRate = sourceRate;
    return clip;
}

std::unique_ptr<RenderGraph> makeGraph(const std::shared_ptr<FakeAsset>& asset, double volume,
                                       double pan, std::uint64_t serial)
{
    auto graph = std::make_unique<RenderGraph>();
    graph->serial = serial;
    RenderTrack track;
    track.trackId = 1;
    track.slot = 0;
    track.volume = volume;
    track.pan = pan;
    track.clips.push_back(makeClip(asset, 0.0, 10.0, kRate));
    graph->tracks.push_back(std::move(track));
    graph->levels = {{0}};
    graph->masterSources = {0};
    graph->keepAlive.push_back(asset);
    return graph;
}

struct Device
{
    explicit Device(int numSamples)
        : left(static_cast<std::size_t>(numSamples))
        , right(static_cast<std::size_t>(numSamples))
        , channels{left.data(), right.data()}
    {
    }

    void render(AudioEngine& engine, int numBlocks = 1)
    {
        for (int i = 0; i < numBlocks; ++i)
            engine.process(nullptr, 0, channels, 2, static_cast<int>(left.size()));
    }

    std::vector<float> left;
    std::vector<float> right;
    float* channels[2];
};

void swapIn(AudioEngine& engine, std::unique_ptr<RenderGraph> graph)
{
    command::SwapGraph swap;
    swap.graph = graph.release();
    REQUIRE(engine.post(swap));
}
} // namespace

TEST_CASE("Clip renderer copies same-rate audio exactly and declicks its edges", "[engine][clip]")
{
    auto asset = std::make_shared<FakeAsset>(1, 4800, 0.5f);
    const auto clip = makeClip(asset, 0.0, 0.1, kRate); // 100 ms, mono

    std::vector<Sample> left(4800, 0.0);
    std::vector<Sample> right(4800, 0.0);
    REQUIRE(dsp::renderClip(clip, 0.0, 1.0 / kRate, 4800, left.data(), right.data()));

    CHECK(left[0] == 0.0);                          // fade-in starts from silence
    CHECK_THAT(left[2400], WithinAbs(0.5, 1e-12));  // body is bit-exact
    CHECK_THAT(right[2400], WithinAbs(0.5, 1e-12)); // mono is played on both sides
    CHECK(left[4799] < 0.05);                       // fade-out ends near silence
}

TEST_CASE("Clip renderer resamples sources recorded at another rate", "[engine][clip]")
{
    auto asset = std::make_shared<FakeAsset>(2, 24'000, 0.25f); // 1 s at 24 kHz
    const auto clip = makeClip(asset, 0.0, 1.0, 24'000.0);

    std::vector<Sample> left(48'000, 0.0);
    std::vector<Sample> right(48'000, 0.0);
    REQUIRE(dsp::renderClip(clip, 0.0, 1.0 / kRate, 48'000, left.data(), right.data()));

    CHECK_THAT(left[24'000], WithinAbs(0.25, 1e-9)); // plays for the full second at device rate
    CHECK_THAT(right[40'000], WithinAbs(0.25, 1e-9));
}

TEST_CASE("Clip renderer only touches the samples inside the clip", "[engine][clip]")
{
    auto asset = std::make_shared<FakeAsset>(1, 48'000, 1.0f);
    const auto clip = makeClip(asset, 0.5, 0.25, kRate); // 0.5 s .. 0.75 s

    std::vector<Sample> left(512, 0.0);
    std::vector<Sample> right(512, 0.0);

    CHECK_FALSE(dsp::renderClip(clip, 0.0, 1.0 / kRate, 512, left.data(), right.data()));  // before
    CHECK_FALSE(dsp::renderClip(clip, 0.75, 1.0 / kRate, 512, left.data(), right.data())); // after
    CHECK(dsp::renderClip(clip, 0.6, 1.0 / kRate, 512, left.data(), right.data()));        // inside
}

TEST_CASE("Tracks play only while the transport runs, with their pan gains", "[engine][graph]")
{
    AudioEngine engine;
    engine.prepare(kRate, 256, 2);
    auto asset = std::make_shared<FakeAsset>(1, 480'000, 0.5f);
    swapIn(engine, makeGraph(asset, 1.0, 1.0, 1)); // full volume, hard right

    Device device(256);
    device.render(engine, 4);
    CHECK(engine.getAppliedGraphSerial() == 1);
    CHECK(*std::max_element(device.right.begin(), device.right.end()) == 0.0f); // stopped: silent

    REQUIRE(engine.post(command::SetPlaying{true}));
    device.render(engine, 4);
    CHECK_THAT(device.right[128], WithinAbs(0.5, 1e-6));
    CHECK_THAT(device.left[128], WithinAbs(0.0, 1e-6));

    const auto peak = engine.collectTrackPeak(0);
    CHECK_THAT(peak.right, WithinAbs(0.5, 1e-6));
    CHECK(peak.left < 1e-6f);
}

TEST_CASE("Swapped-out graphs come back to the message thread for deletion", "[engine][graph]")
{
    auto asset = std::make_shared<FakeAsset>(1, 4800, 0.5f);
    std::weak_ptr<FakeAsset> watch = asset;

    {
        AudioEngine engine;
        engine.prepare(kRate, 256, 2);
        Device device(256);

        swapIn(engine, makeGraph(asset, 1.0, 1.0, 1));
        device.render(engine);
        swapIn(engine, std::make_unique<RenderGraph>()); // graph 1 is retired by this swap
        asset.reset();

        device.render(engine);
        CHECK_FALSE(watch.expired()); // still parked in the return queue
        engine.releaseRetiredGraphs();
        CHECK(watch.expired()); // destroyed here, on the "message" thread
    }
}

TEST_CASE("Destroying the engine reclaims queued and current graphs", "[engine][graph]")
{
    auto asset = std::make_shared<FakeAsset>(1, 4800, 0.5f);
    std::weak_ptr<FakeAsset> watch = asset;

    {
        AudioEngine engine;
        engine.prepare(kRate, 256, 2);
        Device device(256);
        swapIn(engine, makeGraph(asset, 1.0, 1.0, 1));
        device.render(engine);                         // graph 1 is now current
        swapIn(engine, makeGraph(asset, 1.0, 1.0, 2)); // graph 2 never reaches the audio thread
        asset.reset();
        CHECK_FALSE(watch.expired());
    }

    CHECK(watch.expired());
}

TEST_CASE("A sample-rate change keeps the playhead at the same time", "[engine][transport]")
{
    AudioEngine engine;
    engine.prepare(48'000.0, 256, 2);
    REQUIRE(engine.post(command::Locate{96'000})); // 2 s
    Device device(256);
    device.render(engine);

    engine.prepare(96'000.0, 256, 2);
    device.render(engine);
    CHECK(engine.collectTelemetry().playheadSamples == 192'000); // still 2 s
}

TEST_CASE("Rendering and swapping graphs never touches the heap on the audio thread", "[engine][graph][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    AudioEngine engine;
    engine.prepare(kRate, 256, 2);
    auto asset = std::make_shared<FakeAsset>(2, 480'000, 0.25f);
    REQUIRE(engine.post(command::SetPlaying{true}));

    // Build every graph up front: allocation is fine here, on the "message thread".
    std::vector<std::unique_ptr<RenderGraph>> graphs;
    for (std::uint64_t i = 1; i <= 40; ++i)
        graphs.push_back(makeGraph(asset, 0.5 + 0.01 * static_cast<double>(i), 0.0, i));

    Device device(256);
    const auto before = rt::getRealtimeViolationCount();

    for (std::size_t block = 0; block < 400; ++block)
    {
        if (block % 10 == 0)
            swapIn(engine, std::move(graphs[block / 10]));

        device.render(engine);
        engine.releaseRetiredGraphs(); // message-thread side; not counted (outside the RT section)
    }

    CHECK(rt::getRealtimeViolationCount() == before);
}

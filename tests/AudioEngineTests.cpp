#include "core/rt/RealtimeGuard.h"
#include "engine/AudioEngine.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace dojo;
using namespace dojo::engine;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kSampleRate = 48'000.0;
constexpr int kBlockSize = 256;

/** Owns float "device" buffers and exposes them the way a driver does (array of channel pointers). */
struct DeviceBuffers
{
    DeviceBuffers(int numChannels, int numSamples)
        : storage(static_cast<std::size_t>(numChannels),
                  std::vector<float>(static_cast<std::size_t>(numSamples), 1.0f))
    {
        for (auto& channel : storage)
            pointers.push_back(channel.data());
    }

    float* const* channels() { return pointers.data(); }
    int numChannels() const { return static_cast<int>(pointers.size()); }

    float peak(std::size_t channel) const
    {
        float result = 0.0f;
        for (float v : storage[channel])
            result = std::max(result, std::abs(v));
        return result;
    }

    std::vector<std::vector<float>> storage; // pre-filled with garbage (1.0f) to prove we overwrite it
    std::vector<float*> pointers;
};

void render(AudioEngine& engine, DeviceBuffers& buffers, int numSamples, int numBlocks = 1)
{
    for (int i = 0; i < numBlocks; ++i)
        engine.process(nullptr, 0, buffers.channels(), buffers.numChannels(), numSamples);
}
} // namespace

TEST_CASE("An unprepared engine outputs silence", "[engine]")
{
    AudioEngine engine;
    DeviceBuffers out(2, kBlockSize);

    render(engine, out, kBlockSize);

    CHECK(out.peak(0) == 0.0f);
    CHECK(out.peak(1) == 0.0f);
}

TEST_CASE("A prepared, idle engine outputs silence", "[engine]")
{
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2);
    DeviceBuffers out(2, kBlockSize);

    render(engine, out, kBlockSize);

    CHECK(out.peak(0) == 0.0f);
    CHECK(out.peak(1) == 0.0f);
    CHECK(engine.collectTelemetry().processedBlocks == 1);
}

TEST_CASE("The test tone reaches its target level on every channel", "[engine]")
{
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2);
    REQUIRE(engine.post(command::SetTestTone{true, 1000.0, 0.5}));

    DeviceBuffers out(2, kBlockSize);
    render(engine, out, kBlockSize, 10); // well past the 20 ms fade-in

    CHECK_THAT(out.peak(0), WithinAbs(0.5, 0.005));
    CHECK_THAT(out.peak(1), WithinAbs(0.5, 0.005));

    const auto telemetry = engine.collectTelemetry();
    CHECK_THAT(telemetry.masterPeakLeft, WithinAbs(0.5, 0.005));
    CHECK_THAT(telemetry.masterPeakRight, WithinAbs(0.5, 0.005));
    CHECK(telemetry.clippedSamples == 0);
}

TEST_CASE("Master gain is applied with smoothing", "[engine]")
{
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2);
    REQUIRE(engine.post(command::SetTestTone{true, 1000.0, 0.5}));
    REQUIRE(engine.post(command::SetMasterGain{0.5}));

    DeviceBuffers out(2, kBlockSize);
    render(engine, out, kBlockSize, 20); // > 50 ms gain ramp

    CHECK_THAT(out.peak(0), WithinAbs(0.25, 0.005));
}

TEST_CASE("Telemetry peaks are 'max since last read'", "[engine]")
{
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2);
    REQUIRE(engine.post(command::SetTestTone{true, 1000.0, 0.5}));

    DeviceBuffers out(2, kBlockSize);
    render(engine, out, kBlockSize, 10);

    CHECK(engine.collectTelemetry().masterPeakLeft > 0.4f);
    CHECK(engine.collectTelemetry().masterPeakLeft == 0.0f); // reset by the previous read
}

TEST_CASE("The transport advances only while playing and honours locate", "[engine][transport]")
{
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2);
    DeviceBuffers out(2, kBlockSize);

    render(engine, out, kBlockSize, 4);
    CHECK(engine.collectTelemetry().playheadSamples == 0);

    REQUIRE(engine.post(command::SetPlaying{true}));
    render(engine, out, kBlockSize, 4);
    auto telemetry = engine.collectTelemetry();
    CHECK(telemetry.playing);
    CHECK(telemetry.playheadSamples == 4 * kBlockSize);

    REQUIRE(engine.post(command::SetPlaying{false}));
    render(engine, out, kBlockSize, 4);
    telemetry = engine.collectTelemetry();
    CHECK_FALSE(telemetry.playing);
    CHECK(telemetry.playheadSamples == 4 * kBlockSize);

    REQUIRE(engine.post(command::Locate{96'000}));
    render(engine, out, kBlockSize);
    CHECK(engine.collectTelemetry().playheadSamples == 96'000);
}

TEST_CASE("Blocks larger than the prepared size are rendered in sub-blocks", "[engine]")
{
    AudioEngine engine;
    engine.prepare(kSampleRate, 64, 2);
    REQUIRE(engine.post(command::SetPlaying{true}));
    REQUIRE(engine.post(command::SetTestTone{true, 440.0, 0.25}));

    DeviceBuffers out(2, 1000); // 1000 > 64 and not a multiple of it
    render(engine, out, 1000, 3);

    CHECK(engine.collectTelemetry().playheadSamples == 3000);
    CHECK_THAT(out.peak(0), WithinAbs(0.25, 0.005));
}

TEST_CASE("Null and surplus output channels are handled", "[engine]")
{
    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2);
    REQUIRE(engine.post(command::SetTestTone{true, 1000.0, 0.5}));

    DeviceBuffers out(4, kBlockSize); // device reports more channels than prepared
    out.pointers[1] = nullptr;        // ...and one inactive channel

    render(engine, out, kBlockSize, 10);

    CHECK(out.peak(0) > 0.4f);
    CHECK(out.peak(1) == 1.0f); // null channel untouched (still the garbage fill)
    CHECK(out.peak(2) == 0.0f); // surplus channels are silenced
    CHECK(out.peak(3) == 0.0f);
}

TEST_CASE("The audio callback never touches the heap", "[engine][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    AudioEngine engine;
    engine.prepare(kSampleRate, kBlockSize, 2);
    DeviceBuffers out(2, 2048);

    const auto violationsBefore = rt::getRealtimeViolationCount();

    // Exercise every command type and odd block sizes while rendering ~10 seconds of audio.
    for (int block = 0; block < 2000; ++block)
    {
        switch (block % 5)
        {
        case 0:
            REQUIRE(engine.post(command::SetPlaying{(block / 5) % 2 == 0}));
            break;
        case 1:
            REQUIRE(engine.post(command::SetTestTone{block % 2 == 1, 220.0 + block, 0.2}));
            break;
        case 2:
            REQUIRE(engine.post(command::SetMasterGain{0.1 + (block % 10) * 0.1}));
            break;
        case 3:
            REQUIRE(engine.post(command::Locate{block * 100}));
            break;
        default:
            break;
        }

        const int numSamples = (block % 7 == 0) ? 2048 : 1 + (block * 37) % kBlockSize;
        render(engine, out, numSamples);

        if (block % 50 == 0)
            (void)engine.collectTelemetry();
    }

    CHECK(rt::getRealtimeViolationCount() == violationsBefore);
}

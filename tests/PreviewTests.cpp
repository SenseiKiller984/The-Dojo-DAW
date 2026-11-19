#include "core/rt/RealtimeGuard.h"
#include "engine/AudioEngine.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <thread>

using namespace dojo;
using namespace dojo::engine;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

struct PreviewRig
{
    PreviewRig()
    {
        formats.registerBasicFormats();
        engine.prepare(kRate, kBlock, 2);
        service = std::make_unique<PreviewService>(engine, formats);
        service->setVolume(1.0f);
    }

    ~PreviewRig()
    {
        service.reset();
        engine.release();
    }

    bool waitForDecode()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!service->isDecodeFinished())
        {
            if (std::chrono::steady_clock::now() > deadline)
                return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return true;
    }

    /** Renders `frames` frames like the device would; returns left and right. */
    std::pair<std::vector<float>, std::vector<float>> render(int frames)
    {
        std::vector<float> left, right;
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < frames; done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            left.insert(left.end(), l.begin(), l.end());
            right.insert(right.end(), r.begin(), r.end());
        }
        return {left, right};
    }

    juce::AudioFormatManager formats;
    AudioEngine engine{0};
    std::unique_ptr<PreviewService> service;
    test::TempFolder temp;
};
} // namespace

TEST_CASE("Preview plays a file at the device rate on top of the mix", "[preview]")
{
    PreviewRig rig;
    // 0.5 s of a 441 Hz sine at 44.1 kHz, mono: must come out at 441 Hz on both channels.
    const auto file = test::writeWav(rig.temp.dir.getChildFile("tone.wav"), test::sine(441.0, 44'100.0, 22'050), 44'100.0);

    const auto violationsBefore = rt::getRealtimeViolationCount();
    REQUIRE(rig.service->preview(file));
    REQUIRE(rig.waitForDecode());
    CHECK(rig.service->getCurrentFile() == file);

    auto [left, right] = rig.render(static_cast<int>(kRate * 0.4));
    CHECK(rig.service->isPlaying());
    CHECK(test::peak(left, 4800) > 0.45);
    CHECK(test::peak(right, 4800) > 0.45);
    CHECK(test::peak(left) < 0.55);
    CHECK(std::abs(test::estimateFrequency(left, 4800, left.size(), kRate) - 441.0) < 2.0);

    // Not part of the mix: the master meter saw nothing.
    CHECK(rig.engine.collectTelemetry().masterPeakLeft == 0.0f);

    // It ends by itself once the file is over (0.5 s).
    auto [tailLeft, tailRight] = rig.render(static_cast<int>(kRate * 0.2));
    CHECK_FALSE(rig.service->isPlaying());
    CHECK(test::peak(tailLeft, static_cast<std::size_t>(kRate * 0.12)) == 0.0);
    CHECK(rig.service->getProgress() > 0.99f);

    if (rt::isAllocationTrapInstalled())
        CHECK(rt::getRealtimeViolationCount() == violationsBefore);
}

TEST_CASE("Preview stops with a short fade, and a new preview replaces the old one", "[preview]")
{
    PreviewRig rig;
    const auto a = test::writeWav(rig.temp.dir.getChildFile("a.wav"), test::sine(300.0, kRate, 48'000), kRate);
    const auto b = test::writeWav(rig.temp.dir.getChildFile("b.wav"), test::sine(1200.0, kRate, 48'000), kRate);

    REQUIRE(rig.service->preview(a));
    REQUIRE(rig.waitForDecode());
    rig.render(4800);
    REQUIRE(rig.service->isPlaying());

    rig.service->stop();
    auto [fadeLeft, fadeRight] = rig.render(2048);
    CHECK_FALSE(rig.service->isPlaying());
    CHECK(test::peak(fadeLeft, 512) == 0.0); // faded out within ~5 ms

    REQUIRE(rig.service->preview(a));
    REQUIRE(rig.waitForDecode());
    rig.render(2400);
    REQUIRE(rig.service->preview(b));
    REQUIRE(rig.waitForDecode());
    auto [left, right] = rig.render(9600);
    CHECK(std::abs(test::estimateFrequency(left, 2400, left.size(), kRate) - 1200.0) < 5.0);
    CHECK(rig.service->getCurrentFile() == b);
}

TEST_CASE("Previews are not rendered into exports", "[preview]")
{
    PreviewRig rig;
    const auto file = test::writeWav(rig.temp.dir.getChildFile("loud.wav"), test::sine(500.0, kRate, 48'000), kRate);
    REQUIRE(rig.service->preview(file));
    REQUIRE(rig.waitForDecode());
    rig.render(1024);
    REQUIRE(rig.service->isPlaying());

    REQUIRE(rig.engine.beginOfflineRender());
    std::vector<float> l(kBlock), r(kBlock);
    float* channels[2] = {l.data(), r.data()};
    float peak = 0.0f;
    for (int i = 0; i < 20; ++i)
    {
        rig.engine.renderOffline(channels, 2, kBlock);
        for (int s = 0; s < kBlock; ++s)
            peak = std::max({peak, std::abs(l[static_cast<std::size_t>(s)]), std::abs(r[static_cast<std::size_t>(s)])});
    }
    rig.engine.endOfflineRender();
    CHECK(peak == 0.0f);
}

TEST_CASE("Preview refuses non-audio files and a stopped device", "[preview]")
{
    PreviewRig rig;
    const auto text = rig.temp.dir.getChildFile("notes.txt");
    text.replaceWithText("not audio");
    CHECK_FALSE(rig.service->preview(text));
    CHECK_FALSE(rig.service->preview(rig.temp.dir.getChildFile("missing.wav")));

    const auto file = test::writeWav(rig.temp.dir.getChildFile("tone.wav"), test::sine(500.0, kRate, 4800), kRate);
    rig.engine.release();
    CHECK_FALSE(rig.service->preview(file));
}

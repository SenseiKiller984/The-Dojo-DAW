// Spectrum overlays: the engine's taps of watched tracks' sound, the analysis, and the EQ showing
// other tracks' spectra (and its sidechain's) over its own, shading where they mask it.

#include "devices/BuiltinProcessor.h"
#include "devices/DeviceRegistry.h"
#include "devices/Equaliser.h"
#include "engine/AudioEngine.h"
#include "engine/TrackTaps.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/EditorState.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/SpectrumAnalysis.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

std::vector<double> sine(double frequency, double amplitude, int count)
{
    std::vector<double> samples(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
        samples[static_cast<std::size_t>(i)] = amplitude * std::sin(2.0 * juce::MathConstants<double>::pi * frequency * i / kRate);
    return samples;
}

void writeTone(engine::TrackTaps& taps, std::uint32_t track, double frequency, double amplitude, int count = 8192)
{
    const auto tone = sine(frequency, amplitude, count);
    const double* channels[1] = {tone.data()};
    taps.write(track, channels, 1, count);
}

double rms(const std::vector<float>& x)
{
    double total = 0.0;
    for (const float v : x)
        total += static_cast<double>(v) * static_cast<double>(v);
    return std::sqrt(total / static_cast<double>(std::max<std::size_t>(1, x.size())));
}
} // namespace

TEST_CASE("Track taps: watched tracks' sound, counted watches, nothing written for the rest", "[engine][spectrum]")
{
    engine::TrackTaps taps;
    std::vector<float> out(64);
    CHECK_FALSE(taps.copy(7, out.data(), 64));
    writeTone(taps, 7, 1000.0, 0.5); // (not watched: not kept)
    REQUIRE(taps.watch(7));
    REQUIRE(taps.copy(7, out.data(), 64));
    CHECK(rms(out) == 0.0);

    // The newest samples, oldest first; stereo summed to mono.
    const std::vector<double> left{1.0, 2.0, 3.0}, right{3.0, 4.0, 5.0};
    const double* channels[2] = {left.data(), right.data()};
    taps.write(7, channels, 2, 3);
    std::vector<float> last(3);
    REQUIRE(taps.copy(7, last.data(), 3));
    CHECK(last == std::vector<float>{2.0f, 3.0f, 4.0f});

    // Watches are counted: the tap stays until the last one ends.
    REQUIRE(taps.watch(7));
    taps.unwatch(7);
    CHECK(taps.isWatched(7));
    taps.unwatch(7);
    CHECK_FALSE(taps.isWatched(7));

    // Sixteen at a time.
    for (std::uint32_t id = 1; id <= engine::TrackTaps::kTaps; ++id)
        CHECK(taps.watch(id));
    CHECK_FALSE(taps.watch(99));
    taps.unwatch(3);
    CHECK(taps.watch(99));
    CHECK_FALSE(taps.watch(0));

    // The rings live on the heap: the engine still fits on a thread's stack (1 MB on Windows).
    CHECK(sizeof(engine::TrackTaps) < 256);
    CHECK(sizeof(engine::AudioEngine) < 128 * 1024);
}

TEST_CASE("Track taps: the engine writes a watched track's post-fader sound", "[engine][spectrum]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(1000.0, kRate, 48000, 0.2f), kRate);
    const auto level = [&](double faderDb)
    {
        engine::AudioEngine engine(2);
        session::Session session;
        session::AudioAssetCache assets;
        session::DeviceManager devices(session, [](const juce::ValueTree& d, double, int, juce::String&)
                                       { return devices::createBuiltinDevice(session::device::getType(d)); });
        session::GraphBuilder builder;
        engine.prepare(kRate, kBlock, 2);
        devices.setPlayHead(engine.getPlayHead());
        devices.prepareAll(kRate, kBlock);
        builder.setRenderFormat(kRate, kBlock);
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = session.addTrack();
        session.addClip(t, file, 0.0, asset->getLengthSeconds());
        session::track::setVolumeDb(t, faderDb, nullptr);
        auto graph = builder.build(session, assets, &devices);
        graph->serial = 1;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        REQUIRE(engine.getTrackTaps().watch(session::track::getId(t)));
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < 14400; done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            engine.releaseRetiredGraphs();
        }
        std::vector<float> tap(4096);
        REQUIRE(engine.getTrackTaps().copy(session::track::getId(t), tap.data(), 4096));
        return rms(tap);
    };
    const double unity = level(0.0);
    CHECK(unity > 0.05);
    CHECK_THAT(20.0 * std::log10(level(-6.0) / unity), WithinAbs(-6.0, 0.1));
}

TEST_CASE("Spectrum analysis: levels at chosen frequencies, masking and the stretches masked", "[ui][spectrum]")
{
    const auto frequencies = ui::spectrum::logFrequencies(20.0, 20000.0, 240);
    CHECK(frequencies.size() == 240);
    CHECK_THAT(frequencies.front(), WithinAbs(20.0, 1e-9));
    CHECK_THAT(frequencies.back(), WithinAbs(20000.0, 1e-6));

    // A full-scale sine reads about 0 dB at its frequency and nothing far from it.
    ui::spectrum::Analyser analyser(12);
    const auto tone = sine(1000.0, 1.0, analyser.size());
    std::vector<float> samples(tone.begin(), tone.end());
    std::vector<float> levels;
    analyser.analyse(samples.data(), kRate, frequencies, levels);
    const auto at = [&](double hz)
    {
        const auto it = std::min_element(frequencies.begin(), frequencies.end(), [&](double a, double b) { return std::abs(a - hz) < std::abs(b - hz); });
        return levels[static_cast<std::size_t>(it - frequencies.begin())];
    };
    CHECK_THAT(at(1000.0), WithinAbs(0.0, 1.6));
    CHECK(at(10000.0) < -80.0f);
    CHECK(at(100.0) < -80.0f);
    // Smoothing: silence lets it fall gradually.
    std::vector<float> silence(samples.size(), 0.0f);
    analyser.analyse(silence.data(), kRate, frequencies, levels, 0.6f, 0.25f);
    CHECK(at(1000.0) < -1.0f);
    CHECK(at(1000.0) > -40.0f);

    // Masking: none 6 dB under, half level with it, full 6 dB over; nothing below the floor.
    const auto amount = ui::spectrum::masking({-20.0f, -20.0f, -20.0f, -20.0f, -80.0f}, {-26.0f, -20.0f, -14.0f, -80.0f, -10.0f});
    CHECK(amount == std::vector<float>{0.0f, 0.5f, 1.0f, 0.0f, 0.0f});
    const auto ranges = ui::spectrum::maskedRanges({50, 60, 70, 80, 90, 100, 110}, {0.6f, 0.9f, 0.1f, 0.0f, 0.5f, 0.5f, 0.5f});
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0].low == 50.0);
    CHECK(ranges[0].high == 60.0);
    CHECK(ranges[1].low == 90.0);
    CHECK(ranges[1].high == 110.0);
}

TEST_CASE("The EQ overlays other tracks' spectra and its sidechain's, and names where they mask its track", "[ui][spectrum]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    ui::EditorState editorState;
    engine::TrackTaps taps;
    auto bass = session.addTrack(session::TrackKind::audio, "Bass");
    auto kick = session.addTrack(session::TrackKind::audio, "Kick");
    auto hats = session.addTrack(session::TrackKind::audio, "Hats");
    auto device = session.addDevice(bass, devices::Equaliser::kTypeId, "EQ");
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto& eq = dynamic_cast<devices::Equaliser&>(node->getProcessor());
    eq.prepareToPlay(kRate, kBlock);
    CHECK(node->hasSidechainInput());

    // The bass track: 80 Hz through the EQ (flat), the sidechain silent.
    const auto play = [&](double bassAmplitude, double sideAmplitude)
    {
        juce::AudioBuffer<double> buffer(4, 8192);
        const auto b = sine(80.0, bassAmplitude, 8192), s = sine(80.0, sideAmplitude, 8192);
        for (int i = 0; i < 8192; ++i)
        {
            buffer.setSample(0, i, b[static_cast<std::size_t>(i)]);
            buffer.setSample(1, i, b[static_cast<std::size_t>(i)]);
            buffer.setSample(2, i, s[static_cast<std::size_t>(i)]);
            buffer.setSample(3, i, s[static_cast<std::size_t>(i)]);
        }
        juce::MidiBuffer midi;
        eq.processBlock(buffer, midi);
    };
    play(0.25, 0.0);

    int width = 0;
    auto editor = ui::createDeviceEditor(*node, ui::DeviceEditorContext{project, editorState, device, bass, &taps}, width);
    REQUIRE(editor != nullptr);
    editor->setSize(width, 260);
    auto [names, readout] = ui::eqEditor::inspect(*editor);
    CHECK(names.isEmpty());
    CHECK(readout.isEmpty());

    // Overlaying the kick (80 Hz, louder): watched, shown in its colour, and masking the bass there.
    ui::eqEditor::setOverlays(*editor, {session::track::getId(kick), session::track::getId(hats)});
    CHECK(taps.isWatched(session::track::getId(kick)));
    CHECK(device.getProperty("overlays").toString() == juce::String(session::track::getId(kick)) + "," + juce::String(session::track::getId(hats)));
    writeTone(taps, session::track::getId(kick), 80.0, 0.5);
    writeTone(taps, session::track::getId(hats), 9000.0, 0.5);
    std::tie(names, readout) = ui::eqEditor::inspect(*editor);
    CHECK(names == juce::StringArray{"KICK", "HATS"});
    CHECK(readout.startsWith("MASKED "));
    CHECK(readout.endsWith("BY KICK"));
    (void) editor->createComponentSnapshot(editor->getLocalBounds());

    // Quieter than the bass, far under it: no masking.
    writeTone(taps, session::track::getId(kick), 80.0, 0.01);
    for (int i = 0; i < 20; ++i)
        std::tie(names, readout) = ui::eqEditor::inspect(*editor);
    CHECK(readout.isEmpty());

    // The sidechain (picked in the device's header) shows too, and masks.
    session::device::setSidechain(device, session::track::getId(kick), nullptr);
    play(0.25, 0.5);
    std::tie(names, readout) = ui::eqEditor::inspect(*editor);
    CHECK(names == juce::StringArray{"SC KICK", "KICK", "HATS"});
    CHECK(readout.endsWith("BY SC KICK"));

    // Taking an overlay away lets go of its tap; so does closing the editor.
    device.setProperty("overlays", juce::String(session::track::getId(hats)), nullptr);
    CHECK_FALSE(taps.isWatched(session::track::getId(kick)));
    CHECK(taps.isWatched(session::track::getId(hats)));
    editor.reset();
    CHECK_FALSE(taps.isWatched(session::track::getId(hats)));
}

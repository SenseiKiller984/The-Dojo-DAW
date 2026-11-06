#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/WaveformPainter.h"
#include "ui/components/ArrangerView.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using Catch::Matchers::WithinAbs;

namespace
{
/** Rows of `image` (within columns [x0, x1)) where most pixels are brighter than `threshold`. */
std::vector<bool> brightRows(const juce::Image& image, juce::Rectangle<int> area, float threshold)
{
    std::vector<bool> rows;
    for (int y = area.getY(); y < area.getBottom(); ++y)
    {
        int bright = 0;
        for (int x = area.getX(); x < area.getRight(); ++x)
            bright += image.getPixelAt(x, y).getBrightness() > threshold ? 1 : 0;
        rows.push_back(bright * 2 > area.getWidth());
    }
    return rows;
}

int count(const std::vector<bool>& rows, std::size_t begin, std::size_t end)
{
    int n = 0;
    for (std::size_t i = begin; i < std::min(end, rows.size()); ++i)
        n += rows[i] ? 1 : 0;
    return n;
}

/** A stereo WAV: silence on the left, a loud tone on the right. */
juce::File writeRightOnly(const juce::File& file, double sampleRate, int frames)
{
    juce::AudioBuffer<float> buffer(2, frames);
    buffer.clear();
    const auto tone = test::sine(220.0, sampleRate, frames, 0.95f);
    std::copy(tone.begin(), tone.end(), buffer.getWritePointer(1));

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(sampleRate)
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(32)
                                                  .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
    REQUIRE(writer != nullptr);
    writer->writeFromAudioSampleBuffer(buffer, 0, frames);
    return file;
}
} // namespace

TEST_CASE("Waveform lanes: stereo gets two (left above right) when there is room", "[ui][waveform]")
{
    const juce::Rectangle<float> tall(10.0f, 20.0f, 300.0f, 41.0f);

    const auto mono = ui::waveform::lanes(tall, 1);
    REQUIRE(mono.size() == 1);
    CHECK(mono[0] == tall);

    const auto stereo = ui::waveform::lanes(tall, 2);
    REQUIRE(stereo.size() == 2);
    CHECK_THAT(stereo[0].getHeight(), WithinAbs(20.0, 1.0e-4));
    CHECK_THAT(stereo[1].getHeight(), WithinAbs(20.0, 1.0e-4));
    CHECK(stereo[0].getY() == tall.getY());
    CHECK(stereo[1].getBottom() == tall.getBottom());
    CHECK(stereo[0].getBottom() < stereo[1].getY()); // a gap for the divider, no overlap

    // Too short for two readable channels: one merged lane.
    CHECK(ui::waveform::lanes(tall.withHeight(18.0f), 2).size() == 1);
}

TEST_CASE("Waveform painter draws each channel in its own lane", "[ui][waveform]")
{
    juce::Image image(juce::Image::ARGB, 100, 41, true);
    {
        juce::Graphics g(image);
        // Left channel silent, right channel full scale.
        ui::waveform::paintChannels(g, image.getBounds().toFloat(), 2, 0, 100, juce::Colours::white,
                                    [](int channel, int) { return channel == 0 ? std::pair{0.0f, 0.0f} : std::pair{-1.0f, 1.0f}; });
    }
    const auto rows = brightRows(image, image.getBounds(), 0.5f);
    CHECK(count(rows, 0, 20) == 1);   // left: only the centre line
    CHECK(count(rows, 21, 41) == 20); // right: the whole lane

    // Merged (not enough height): one lane with the extremes of both channels.
    juce::Image small(juce::Image::ARGB, 100, 16, true);
    {
        juce::Graphics g(small);
        ui::waveform::paintChannels(g, small.getBounds().toFloat(), 2, 0, 100, juce::Colours::white,
                                    [](int channel, int) { return channel == 0 ? std::pair{0.0f, 0.0f} : std::pair{-1.0f, 1.0f}; });
    }
    CHECK(count(brightRows(small, small.getBounds(), 0.5f), 0, 16) == 16);
}

TEST_CASE("The arranger shows a stereo clip's two channels", "[ui][waveform]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    ui::EditorState editorState;

    auto& session = project.getSession();
    const auto stereoFile = writeRightOnly(temp.dir.getChildFile("right only.wav"), 48'000.0, 96'000);
    const auto monoFile = test::writeWav(temp.dir.getChildFile("mono.wav"), test::sine(220.0, 48'000.0, 96'000, 0.95f), 48'000.0);
    auto stereoTrack = session.addTrack(session::TrackKind::audio, "Stereo");
    auto monoTrack = session.addTrack(session::TrackKind::audio, "Mono");
    const auto stereoClip = session.addClip(stereoTrack, stereoFile, 0.0, 2.0, 0.0);
    const auto monoClip = session.addClip(monoTrack, monoFile, 0.0, 2.0, 0.0);
    project.getAssets().request(stereoFile, {});
    project.getAssets().request(monoFile, {});
    REQUIRE(test::pumpMessagesUntil([&]
                                    {
                                        return project.getAssets().find(stereoFile) != nullptr && project.getAssets().find(monoFile) != nullptr;
                                    }));
    CHECK(project.getAssets().find(stereoFile)->peaks.numChannels == 2);

    ui::ArrangerView arranger(project, controller, editorState);
    arranger.setSize(1400, 500);
    const auto snapshot = arranger.createComponentSnapshot(arranger.getLocalBounds(), true, 1.0f);

    // The waveform area: the clip body under its 16 px title, 3 px margins; middle columns only.
    const auto waveformArea = [&](const juce::ValueTree& clip)
    {
        const auto area = arranger.getClipArea(clip);
        REQUIRE(!area.isEmpty());
        return area.withTrimmedTop(16 + 3).withTrimmedBottom(3).reduced(area.getWidth() / 4, 0);
    };

    // Waveform pixels are far brighter than the clip's background.
    const auto stereoArea = waveformArea(stereoClip);
    const auto background = snapshot.getPixelAt(stereoArea.getCentreX(), stereoArea.getY() + 1).getBrightness();
    const auto stereoRows = brightRows(snapshot, stereoArea, background + 0.15f);
    const auto half = static_cast<std::size_t>(stereoArea.getHeight() / 2);
    INFO("lane height " << half);
    CHECK(count(stereoRows, 0, half) <= 2);                                // left lane: silent
    CHECK(count(stereoRows, half + 1, stereoRows.size()) >= static_cast<int>(half) - 3); // right lane: full

    // A mono clip is one lane over the whole height.
    const auto monoArea = waveformArea(monoClip);
    const auto monoRows = brightRows(snapshot, monoArea, background + 0.15f);
    CHECK(count(monoRows, 0, monoRows.size()) >= static_cast<int>(monoRows.size()) - 4);
}

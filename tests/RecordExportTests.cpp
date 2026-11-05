#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

struct MessageThread
{
    MessageThread() { juce::MessageManager::getInstance(); } // this thread is the message thread
};

/** The application's object graph, without an audio device (the test drives process()). */
struct App
{
    App()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); })
    {
    }

    MessageThread messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    Project project;
};

struct DecodedFile
{
    double sampleRate = 0.0;
    juce::AudioBuffer<float> audio;
};

DecodedFile decode(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    REQUIRE(reader != nullptr);
    DecodedFile decoded;
    decoded.sampleRate = reader->sampleRate;
    decoded.audio.setSize(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&decoded.audio, 0, decoded.audio.getNumSamples(), 0, true, true);
    return decoded;
}

int indexOfPeak(const juce::AudioBuffer<float>& audio, int channel)
{
    int best = 0;
    for (int i = 0; i < audio.getNumSamples(); ++i)
        if (std::abs(audio.getSample(channel, i)) > std::abs(audio.getSample(channel, best)))
            best = i;
    return best;
}
} // namespace

TEST_CASE("Recording an armed track writes its input and places the clip on the timeline", "[record]")
{
    test::TempFolder temp;
    App app;
    app.engine.prepare(kRate, kBlock, 2, 2);
    auto& session = app.project.getSession();
    REQUIRE(app.project.saveAs(temp.dir.getChildFile("song.dojo")).wasOk());

    auto t = session.addTrack("Vocal");
    track::setInput(t, 0, true, nullptr);
    track::setArmed(t, true, nullptr);
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    track::setArmed(keys, true, nullptr);
    app.project.getCompiler().rebuildNow();

    Recorder recorder(app.project, app.controller);
    REQUIRE(recorder.start().isEmpty());
    CHECK(recorder.isRecording());
    app.controller.locateSeconds(1.0); // posted after Play; both apply before the first block

    // The "audio device": 1 s of input with an impulse 0.5 s in, and a MIDI note.
    std::vector<float> inL(kBlock), inR(kBlock), outL(kBlock), outR(kBlock);
    const float* inputs[2] = {inL.data(), inR.data()};
    float* outputs[2] = {outL.data(), outR.data()};
    for (int block = 0; block < static_cast<int>(kRate) / kBlock; ++block)
    {
        std::fill(inL.begin(), inL.end(), 0.0f);
        std::fill(inR.begin(), inR.end(), 0.0f);
        const int impulse = 24000 - block * kBlock;
        if (impulse >= 0 && impulse < kBlock)
            inL[static_cast<std::size_t>(impulse)] = inR[static_cast<std::size_t>(impulse)] = 0.7f;
        if (block == 10)
            (void)app.engine.getLiveMidiQueue().push(juce::MidiMessage::noteOn(1, 64, 0.5f));
        if (block == 100)
            (void)app.engine.getLiveMidiQueue().push(juce::MidiMessage::noteOff(1, 64));
        app.engine.process(inputs, 2, outputs, 2, kBlock);
    }

    recorder.stop();
    app.engine.process(inputs, 2, outputs, 2, kBlock); // acknowledges StopCapture
    REQUIRE(test::pumpMessagesUntil([&] { return !recorder.isBusy(); }));

    const auto clipTree = t.getChildWithName(ids::clip);
    REQUIRE(clipTree.isValid());
    CHECK_THAT(clip::getStart(clipTree), WithinAbs(1.0, 1e-9)); // no device latency here
    CHECK(clip::getFile(clipTree).isAChildOf(temp.dir));

    const auto recorded = decode(clip::getFile(clipTree));
    CHECK(recorded.audio.getNumChannels() == 2);
    CHECK(indexOfPeak(recorded.audio, 0) == 24000);
    CHECK_THAT(recorded.audio.getSample(0, 24000), WithinAbs(0.7, 1e-4));

    const auto midi = keys.getChildWithName(ids::midiClip);
    REQUIRE(midi.isValid());
    const auto n = midi.getChildWithName(ids::note);
    REQUIRE(n.isValid());
    CHECK(note::getNote(n) == 64);
    CHECK_THAT(note::getStart(n), WithinAbs(10 * kBlock / kRate * 2.0, 0.02)); // beats at 120 bpm
    CHECK_THAT(note::getLength(n), WithinAbs(90 * kBlock / kRate * 2.0, 0.02));

    // One undo step removes both takes.
    REQUIRE(session.getUndoManager().undo());
    CHECK_FALSE(t.getChildWithName(ids::clip).isValid());
    CHECK_FALSE(keys.getChildWithName(ids::midiClip).isValid());
}

TEST_CASE("Recording without armed tracks explains what to do", "[record]")
{
    App app;
    app.engine.prepare(kRate, kBlock, 2, 2);
    app.project.getSession().addTrack();
    Recorder recorder(app.project, app.controller);
    CHECK(recorder.start().contains("Arm a track"));
    CHECK_FALSE(recorder.isBusy());
}

TEST_CASE("Offline export renders the mix and stems aligned to the timeline", "[export]")
{
    test::TempFolder temp;
    App app; // no device: the exporter prepares the engine itself
    auto& session = app.project.getSession();
    const auto impulse = test::writeImpulse(temp.dir.getChildFile("imp.wav"), 4800, 48000, kRate, 0.5f);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(impulse, error) != nullptr);

    auto a = session.addTrack("Kick");
    session.addClip(a, impulse, 0.5, 1.0);
    session.addDevice(a, "builtin:limiter", "Limiter"); // adds latency the export must remove
    auto b = session.addTrack("Snare");
    session.addClip(b, impulse, 0.5, 1.0);
    session.setLoop(true, 0.0, 0.25); // exports ignore the loop

    Exporter exporter(app.project, app.controller);
    std::optional<juce::Result> result;
    juce::Array<juce::File> written;
    exporter.onFinished = [&](const juce::Result& r, const juce::Array<juce::File>& files)
    {
        result = r;
        written = files;
    };

    ExportSettings settings;
    settings.startSeconds = 0.0;
    settings.endSeconds = 2.0;
    settings.tailSeconds = 0.5;
    settings.includeStems = true;

    SECTION("WAV")
    {
        settings.destination = temp.dir.getChildFile("mix.wav");
        settings.bitDepth = 32;
        REQUIRE(exporter.start(settings).isEmpty());
        REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 30'000));
        INFO(result->getErrorMessage());
        REQUIRE(result->wasOk());
        CHECK(written.size() == 3);

        const auto mix = decode(settings.destination);
        CHECK(mix.audio.getNumSamples() == static_cast<int>(2.5 * kRate));
        CHECK(indexOfPeak(mix.audio, 0) == 24000 + 4800); // clip at 0.5 s + impulse at 0.1 s: aligned
        CHECK_THAT(mix.audio.getSample(0, 28800), WithinAbs(1.0, 1e-4)); // both tracks together

        const auto stem = decode(Exporter::getStemFile(settings, "Kick"));
        CHECK(indexOfPeak(stem.audio, 0) == 28800);
        CHECK_THAT(stem.audio.getSample(0, 28800), WithinAbs(0.5, 1e-4));
        CHECK_FALSE(app.engine.isPrepared()); // released again
    }

    SECTION("FLAC")
    {
        settings.destination = temp.dir.getChildFile("mix.flac");
        settings.format = ExportSettings::Format::flac;
        settings.includeStems = false;
        REQUIRE(exporter.start(settings).isEmpty());
        REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 30'000));
        REQUIRE(result->wasOk());
        const auto mix = decode(settings.destination);
        CHECK(indexOfPeak(mix.audio, 0) == 28800);
        CHECK_THAT(mix.audio.getSample(0, 28800), WithinAbs(1.0, 1e-3));
    }

    SECTION("MP3")
    {
        settings.destination = temp.dir.getChildFile("mix.mp3");
        settings.format = ExportSettings::Format::mp3;
        settings.includeStems = false;
        REQUIRE(exporter.start(settings).isEmpty());
        REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 30'000));
        REQUIRE(result->wasOk());
        const auto mix = decode(settings.destination);
        CHECK(mix.sampleRate == kRate);
        CHECK(mix.audio.getNumSamples() >= static_cast<int>(2.5 * kRate));
        CHECK(mix.audio.getMagnitude(0, 0, mix.audio.getNumSamples()) > 0.3f);
    }
}

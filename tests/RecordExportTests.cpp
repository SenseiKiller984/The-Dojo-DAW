#include "devices/DeviceRegistry.h"
#include "devices/TheGlue.h"
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

namespace
{
/** Drives the engine like an audio device: every input sample is `inputLevel` (DC, both channels). */
struct Device
{
    explicit Device(App& a)
        : app(a)
    {
    }

    /** Runs `seconds` and returns the left output. */
    std::vector<float> run(double seconds, float inputLevel)
    {
        std::vector<float> out;
        std::vector<float> inL(kBlock, inputLevel), inR(kBlock, inputLevel), outL(kBlock), outR(kBlock);
        const float* inputs[2] = {inL.data(), inR.data()};
        float* outputs[2] = {outL.data(), outR.data()};
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            app.engine.process(inputs, 2, outputs, 2, kBlock);
            out.insert(out.end(), outL.begin(), outL.end());
        }
        return out;
    }

    App& app;
};

double meanOf(const std::vector<float>& x, double fromSeconds, double toSeconds)
{
    const auto from = static_cast<std::size_t>(fromSeconds * kRate);
    const auto to = std::min(x.size(), static_cast<std::size_t>(toSeconds * kRate));
    double sum = 0.0;
    for (auto i = from; i < to; ++i)
        sum += x[i];
    return sum / static_cast<double>(to - from);
}

double toneOf(const std::vector<float>& x, double fromSeconds, double toSeconds)
{
    const auto from = static_cast<std::size_t>(fromSeconds * kRate);
    const auto to = std::min(x.size(), static_cast<std::size_t>(toSeconds * kRate));
    const double coefficient = 2.0 * std::cos(2.0 * 3.14159265358979323846 * 440.0 / kRate);
    double s1 = 0.0, s2 = 0.0;
    for (auto i = from; i < to; ++i)
    {
        const double s0 = x[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coefficient * s1 * s2)) / static_cast<double>(to - from);
}

std::vector<juce::ValueTree> clipsOf(const juce::ValueTree& t)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& c : t)
        if (c.hasType(ids::clip))
            clips.push_back(c);
    std::sort(clips.begin(), clips.end(), [](const auto& a, const auto& b) { return clip::getStart(a) < clip::getStart(b); });
    return clips;
}
} // namespace

TEST_CASE("Stems line up with the timeline also inside a folder that has latency", "[export][pdc]")
{
    // A folder with a limiter (240 samples) holding a track, next to a plain track: the track inside
    // meets its folder's input 240 samples before the master's. Each stem skips its own delay.
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    const auto impulse = test::writeImpulse(temp.dir.getChildFile("imp.wav"), 4800, 48000, kRate, 0.5f);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(impulse, error) != nullptr);

    auto inside = session.addTrack("Inside");
    session.addClip(inside, impulse, 0.5, 1.0);
    auto folder = session.groupIntoFolder({inside}, "Folder");
    session.addDevice(folder, "builtin:limiter", "Limiter");
    auto outside = session.addTrack("Outside");
    session.addClip(outside, impulse, 0.5, 1.0);

    Exporter exporter(app.project, app.controller);
    std::optional<juce::Result> result;
    exporter.onFinished = [&](const juce::Result& r, const juce::Array<juce::File>&) { result = r; };
    ExportSettings settings;
    settings.startSeconds = 0.0;
    settings.endSeconds = 2.0;
    settings.tailSeconds = 0.5;
    settings.includeStems = true;
    settings.bitDepth = 32;
    settings.destination = temp.dir.getChildFile("mix.wav");
    REQUIRE(exporter.start(settings).isEmpty());
    REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 30'000));
    REQUIRE(result->wasOk());

    for (const auto* name : {"Inside", "Folder", "Outside"})
    {
        INFO(name);
        const auto stem = decode(Exporter::getStemFile(settings, name));
        CHECK(indexOfPeak(stem.audio, 0) == 28800);
    }
    const auto mix = decode(settings.destination);
    CHECK(indexOfPeak(mix.audio, 0) == 28800);
    CHECK_THAT(mix.audio.getSample(0, 28800), WithinAbs(1.0, 1e-3));
}

TEST_CASE("Offline export switches devices to their offline oversampling and stays aligned", "[export][pdc]")
{
    // The Glue: 2x in real time (50 samples of latency), 8x offline (65 samples). The export must
    // re-compile the delay compensation after switching, or the mix would be 15 samples late.
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    const auto impulse = test::writeImpulse(temp.dir.getChildFile("imp.wav"), 4800, 48000, kRate, 0.5f);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(impulse, error) != nullptr);

    auto glued = session.addTrack("Glued");
    session.addClip(glued, impulse, 0.5, 1.0);
    const auto device = session.addDevice(glued, devices::TheGlue::kTypeId, "The Glue");
    const auto node = app.project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto* glue = dynamic_cast<devices::TheGlue*>(&node->getProcessor());
    REQUIRE(glue != nullptr);
    glue->getParameterById("threshold")->setValueNotifyingHost(1.0f); // 0 dBFS: no compression
    const int realtime = glue->getLatencySamples();
    CHECK(realtime == glue->latencyFor(false));
    CHECK(glue->latencyFor(true) > realtime);
    auto plain = session.addTrack("Plain");
    session.addClip(plain, impulse, 0.5, 1.0);

    Exporter exporter(app.project, app.controller);
    std::optional<juce::Result> result;
    exporter.onFinished = [&](const juce::Result& r, const juce::Array<juce::File>&) { result = r; };
    ExportSettings settings;
    settings.startSeconds = 0.0;
    settings.endSeconds = 2.0;
    settings.tailSeconds = 0.5;
    settings.includeStems = true;
    settings.bitDepth = 32;
    settings.destination = temp.dir.getChildFile("mix.wav");
    REQUIRE(exporter.start(settings).isEmpty());
    REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 30'000));
    INFO(result->getErrorMessage());
    REQUIRE(result->wasOk());

    const auto mix = decode(settings.destination);
    CHECK(indexOfPeak(mix.audio, 0) == 28800);
    CHECK(mix.audio.getSample(0, 28800) > 0.9f); // both impulses on the same sample
    const auto stem = decode(Exporter::getStemFile(settings, "Glued"));
    CHECK(indexOfPeak(stem.audio, 0) == 28800);
    // Back to real time afterwards.
    CHECK_FALSE(glue->isNonRealtime());
    CHECK(glue->getLatencySamples() == realtime);
}

TEST_CASE("Clearing a range trims, splits and removes clips", "[session][record]")
{
    Session session;
    auto t = session.addTrack("Take");
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile("x.wav");
    session.addClip(t, file, 0.0, 4.0);
    session.addClip(t, file, 5.0, 1.0);
    session.addClip(t, file, 7.0, 2.0);
    session.addClip(t, file, 10.0, 10.0, 1.0);

    session.beginTransaction("Clear");
    session.clearRange(t, 3.0, 7.5);
    session.clearRange(t, 12.0, 15.0);
    auto clips = clipsOf(t);
    REQUIRE(clips.size() == 4);
    CHECK_THAT(clip::getStart(clips[0]), WithinAbs(0.0, 1e-9));
    CHECK_THAT(clip::getEnd(clips[0]), WithinAbs(3.0, 1e-9)); // trimmed at the range start
    CHECK_THAT(clip::getStart(clips[1]), WithinAbs(7.5, 1e-9)); // its start moved to the range end...
    CHECK_THAT(clip::getEnd(clips[1]), WithinAbs(9.0, 1e-9));
    CHECK_THAT(clip::getOffset(clips[1]), WithinAbs(0.5, 1e-9)); // ...and its source with it
    CHECK_THAT(clip::getEnd(clips[2]), WithinAbs(12.0, 1e-9)); // split around the range
    CHECK_THAT(clip::getStart(clips[3]), WithinAbs(15.0, 1e-9));
    CHECK_THAT(clip::getOffset(clips[3]), WithinAbs(6.0, 1e-9));

    session.getUndoManager().undo();
    CHECK(clipsOf(t).size() == 4);
    CHECK_THAT(clip::getEnd(clipsOf(t)[0]), WithinAbs(4.0, 1e-9));
}

TEST_CASE("Record-enabled tracks: Auto Input, Input Only, and punch recording", "[record][monitoring]")
{
    test::TempFolder temp;
    App app;
    app.engine.prepare(kRate, kBlock, 2, 2);
    auto& session = app.project.getSession();
    REQUIRE(app.project.saveAs(temp.dir.getChildFile("punch.dojo")).wasOk());

    // The track already holds a take: a 440 Hz tone over 4 s.
    const auto tone = test::writeWav(temp.dir.getChildFile("old take.wav"), test::sine(440.0, kRate, 4 * 48000, 0.25f), kRate);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(tone, error) != nullptr);
    auto t = session.addTrack("Vocal");
    session.addClip(t, tone, 0.0, 4.0);
    track::setInput(t, 0, true, nullptr);
    track::setArmed(t, true, nullptr);
    REQUIRE(track::getMonitor(t) == engine::MonitorMode::automatic);
    app.project.getCompiler().rebuildNow();
    Device device(app);

    // Auto Input (the default), stopped: the input (DC 0.5).
    auto out = device.run(0.2, 0.5f);
    CHECK_THAT(meanOf(out, 0.1, 0.2), WithinAbs(0.5, 0.01));

    // Playing (not recording): the track's clip, not the input.
    app.controller.locateSeconds(0.0);
    app.controller.play();
    out = device.run(0.5, 0.5f);
    CHECK_THAT(toneOf(out, 0.1, 0.5), WithinAbs(0.25, 0.01));
    CHECK_THAT(meanOf(out, 0.1, 0.5), WithinAbs(0.0, 0.01));

    // Input Only: the input, never the clip.
    session.setInputOnlyMonitoring(true);
    app.project.getCompiler().rebuildNow();
    out = device.run(0.5, 0.5f);
    CHECK_THAT(meanOf(out, 0.1, 0.5), WithinAbs(0.5, 0.01));
    CHECK(toneOf(out, 0.1, 0.5) < 0.01);
    app.controller.stop();
    session.setInputOnlyMonitoring(false);

    // Punch in at 1 s, out at 2 s (the loop range; the loop itself stays off).
    session.setLoop(false, 1.0, 2.0);
    session.setPunchInEnabled(true);
    session.setPunchOutEnabled(true);
    app.project.getCompiler().rebuildNow();
    device.run(0.05, 0.5f);

    Recorder recorder(app.project, app.controller);
    REQUIRE(recorder.start().isEmpty());
    app.controller.locateSeconds(0.0);
    out = device.run(3.0, 0.5f);
    recorder.stop();
    device.run(0.01, 0.5f); // acknowledges StopCapture
    REQUIRE(test::pumpMessagesUntil([&] { return !recorder.isBusy(); }));

    // What was heard: the old take, the input while punched in, the old take again.
    CHECK_THAT(toneOf(out, 0.2, 0.95), WithinAbs(0.25, 0.01));
    CHECK_THAT(meanOf(out, 0.2, 0.95), WithinAbs(0.0, 0.01));
    CHECK_THAT(meanOf(out, 1.05, 1.95), WithinAbs(0.5, 0.01));
    CHECK(toneOf(out, 1.05, 1.95) < 0.01);
    CHECK_THAT(toneOf(out, 2.05, 2.9), WithinAbs(0.25, 0.01));

    // What was kept: the new take between 1 and 2 s, replacing the old one there.
    const auto clips = clipsOf(t);
    REQUIRE(clips.size() == 3);
    CHECK(clip::getFile(clips[0]) == tone);
    CHECK_THAT(clip::getEnd(clips[0]), WithinAbs(1.0, 1e-9));
    CHECK(clip::getFile(clips[1]) != tone);
    CHECK_THAT(clip::getStart(clips[1]), WithinAbs(1.0, 1e-9));
    CHECK_THAT(clip::getEnd(clips[1]), WithinAbs(2.0, 1e-9));
    CHECK_THAT(clip::getOffset(clips[1]), WithinAbs(1.0, 1e-9)); // the recording started at 0
    CHECK(clip::getFile(clips[2]) == tone);
    CHECK_THAT(clip::getStart(clips[2]), WithinAbs(2.0, 1e-9));
    CHECK_THAT(clip::getOffset(clips[2]), WithinAbs(2.0, 1e-9));

    const auto recorded = decode(clip::getFile(clips[1]));
    CHECK_THAT(recorded.audio.getSample(0, static_cast<int>(1.5 * kRate)), WithinAbs(0.5, 1e-3));

    // One undo step brings the old take back whole.
    REQUIRE(session.getUndoManager().undo());
    REQUIRE(clipsOf(t).size() == 1);
    CHECK_THAT(clip::getEnd(clipsOf(t)[0]), WithinAbs(4.0, 1e-9));
}

TEST_CASE("Recording a 5.1 track writes one channel per input", "[record][multichannel]")
{
    test::TempFolder temp;
    App app;
    app.engine.prepare(kRate, kBlock, 6, 8);
    auto& session = app.project.getSession();
    REQUIRE(app.project.saveAs(temp.dir.getChildFile("surround.dojo")).wasOk());

    auto t = session.addTrack("Room");
    track::setFormat(t, TrackFormat::surround51, nullptr);
    track::setInput(t, 2, true, nullptr); // In 3-8
    track::setArmed(t, true, nullptr);
    app.project.getCompiler().rebuildNow();

    Recorder recorder(app.project, app.controller);
    REQUIRE(recorder.start().isEmpty());

    // Input k carries the constant 0.05 * k.
    std::vector<std::vector<float>> in(8, std::vector<float>(kBlock));
    std::vector<std::vector<float>> out(6, std::vector<float>(kBlock));
    std::vector<const float*> inputs;
    std::vector<float*> outputs;
    for (std::size_t k = 0; k < in.size(); ++k)
    {
        std::fill(in[k].begin(), in[k].end(), 0.05f * static_cast<float>(k));
        inputs.push_back(in[k].data());
    }
    for (auto& o : out)
        outputs.push_back(o.data());
    for (int block = 0; block < static_cast<int>(kRate) / (2 * kBlock); ++block)
        app.engine.process(inputs.data(), 8, outputs.data(), 6, kBlock);

    recorder.stop();
    app.engine.process(inputs.data(), 8, outputs.data(), 6, kBlock);
    REQUIRE(test::pumpMessagesUntil([&] { return !recorder.isBusy(); }));

    const auto clipTree = t.getChildWithName(ids::clip);
    REQUIRE(clipTree.isValid());
    const auto recorded = decode(clip::getFile(clipTree));
    REQUIRE(recorded.audio.getNumChannels() == 6);
    for (int ch = 0; ch < 6; ++ch)
        CHECK_THAT(recorded.audio.getSample(ch, recorded.audio.getNumSamples() / 2), WithinAbs(0.05 * (ch + 2), 1e-4));
}

TEST_CASE("A 5.1 mix exports as six channels (stems too); MP3 gets the stereo fold-down", "[export][multichannel]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    std::vector<std::vector<float>> steps;
    for (int k = 0; k < 6; ++k)
        steps.emplace_back(static_cast<std::size_t>(2 * kRate), 0.1f * static_cast<float>(k + 1));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("six.wav"), steps, kRate);
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(file, error) != nullptr);

    session.setMasterFormat(TrackFormat::surround51);
    auto t = session.addTrack("Surround");
    track::setFormat(t, TrackFormat::surround51, nullptr);
    session.addClip(t, file, 0.0, 2.0);

    Exporter exporter(app.project, app.controller);
    std::optional<juce::Result> result;
    exporter.onFinished = [&](const juce::Result& r, const juce::Array<juce::File>&) { result = r; };

    ExportSettings settings;
    settings.startSeconds = 0.0;
    settings.endSeconds = 2.0;

    SECTION("WAV: one channel per speaker")
    {
        settings.destination = temp.dir.getChildFile("mix.wav");
        settings.bitDepth = 32;
        settings.includeStems = true;
        REQUIRE(exporter.start(settings).isEmpty());
        REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 30'000));
        INFO(result->getErrorMessage());
        REQUIRE(result->wasOk());

        const auto mix = decode(settings.destination);
        REQUIRE(mix.audio.getNumChannels() == 6);
        for (int ch = 0; ch < 6; ++ch)
            CHECK_THAT(mix.audio.getSample(ch, static_cast<int>(kRate)), WithinAbs(0.1 * (ch + 1), 1e-4));
        const auto stem = decode(Exporter::getStemFile(settings, "Surround"));
        REQUIRE(stem.audio.getNumChannels() == 6);
        CHECK_THAT(stem.audio.getSample(4, static_cast<int>(kRate)), WithinAbs(0.5, 1e-4));
    }

    SECTION("MP3: stereo")
    {
        settings.destination = temp.dir.getChildFile("mix.mp3");
        settings.format = ExportSettings::Format::mp3;
        REQUIRE(exporter.start(settings).isEmpty());
        REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 30'000));
        REQUIRE(result->wasOk());
        const auto mix = decode(settings.destination);
        REQUIRE(mix.audio.getNumChannels() == 2);
        const double third = 0.70710678118654752;
        CHECK_THAT(mix.audio.getSample(0, static_cast<int>(kRate)), WithinAbs(0.1 + third * 0.3 + third * 0.5, 0.03));
        CHECK_THAT(mix.audio.getSample(1, static_cast<int>(kRate)), WithinAbs(0.2 + third * 0.3 + third * 0.6, 0.03));
    }
}

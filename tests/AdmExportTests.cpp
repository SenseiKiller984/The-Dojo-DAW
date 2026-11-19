#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AdmMetadata.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "Bw64Reader.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>

using namespace dojo;
using namespace dojo::session;
using namespace dojo::test::bw64;
using Catch::Matchers::WithinAbs;
using engine::ChannelLayout;
using engine::Speaker;
using ObjectRendering = GraphBuilder::ObjectRendering;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

struct MessageThread
{
    MessageThread() { juce::MessageManager::getInstance(); }
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

juce::File constant(const juce::File& file, int channels, double seconds, float first, float step)
{
    std::vector<std::vector<float>> audio;
    for (int c = 0; c < channels; ++c)
        audio.emplace_back(static_cast<std::size_t>(seconds * kRate), first + step * static_cast<float>(c));
    return test::writeMultichannelWav(file, audio, kRate);
}

/** Plays `seconds` from the start; the outputs' last block. */
std::vector<std::vector<float>> play(App& app, int outputs, double seconds)
{
    std::vector<std::vector<float>> buffers(static_cast<std::size_t>(outputs), std::vector<float>(kBlock));
    std::vector<float*> pointers;
    for (auto& b : buffers)
        pointers.push_back(b.data());
    app.controller.locateSeconds(0.0);
    app.controller.play();
    for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
    {
        app.controller.pump();
        app.engine.process(nullptr, 0, pointers.data(), outputs, kBlock);
        app.engine.releaseRetiredGraphs();
    }
    return buffers;
}

const juce::XmlElement* channelFormatNamed(const juce::XmlElement& afe, const juce::String& name)
{
    for (const auto* e : afe.getChildWithTagNameIterator("audioChannelFormat"))
        if (e->getStringAttribute("audioChannelFormatName") == name)
            return e;
    return nullptr;
}

struct Block
{
    double start = 0.0, duration = 0.0; ///< seconds
    double x = 0.0, y = 0.0, z = 0.0, width = 0.0;
};
double seconds(const juce::String& time)
{
    const auto parts = juce::StringArray::fromTokens(time, ":", {});
    return parts[0].getIntValue() * 3600.0 + parts[1].getIntValue() * 60.0 + parts[2].getDoubleValue();
}
std::vector<Block> blocksOf(const juce::XmlElement& channelFormat)
{
    std::vector<Block> blocks;
    for (const auto* b : channelFormat.getChildWithTagNameIterator("audioBlockFormat"))
    {
        Block block;
        block.start = seconds(b->getStringAttribute("rtime"));
        block.duration = seconds(b->getStringAttribute("duration"));
        for (const auto* p : b->getChildWithTagNameIterator("position"))
        {
            const auto c = p->getStringAttribute("coordinate");
            (c == "X" ? block.x : c == "Y" ? block.y : block.z) = p->getAllSubText().getDoubleValue();
        }
        if (const auto* w = b->getChildByName("width"))
            block.width = w->getAllSubText().getDoubleValue();
        blocks.push_back(block);
    }
    return blocks;
}
} // namespace

TEST_CASE("ADM objects in the engine: in the bed while monitoring, apart on their own channels, on outputs of their own",
          "[adm][engine]")
{
    test::TempFolder temp;
    App app;
    app.engine.prepare(kRate, kBlock, 14, 0);
    auto& session = app.project.getSession();
    juce::String error;
    const auto pad = constant(temp.dir.getChildFile("pad.wav"), 2, 2.0, 0.1f, 0.0f);
    const auto bird = constant(temp.dir.getChildFile("bird.wav"), 1, 2.0, 0.2f, 0.0f);
    REQUIRE(app.project.getAssets().loadNow(pad, error) != nullptr);
    REQUIRE(app.project.getAssets().loadNow(bird, error) != nullptr);

    session.setMasterFormat(TrackFormat::surround714);
    auto p = session.addTrack("Pad");
    session.addClip(p, pad, 0.0, 2.0);
    auto b = session.addTrack("Bird");
    track::setFormat(b, TrackFormat::mono, nullptr);
    session.addClip(b, bird, 0.0, 2.0);
    track::setPan3d(b, true, nullptr);
    track::setAzimuth(b, 30.0, nullptr); // (at the left speaker)
    track::setObject(b, true, nullptr);
    const auto left = ChannelLayout::surround714().indexOf(Speaker::left);
    auto& compiler = app.project.getCompiler();

    // Monitoring: panned into the bed like any 3D-panned track.
    compiler.rebuildNow();
    CHECK(compiler.getObjects().empty());
    const auto inBed = play(app, 14, 0.3);
    const float bedWithBird = inBed[static_cast<std::size_t>(left)][kBlock / 2];

    // Apart: the bed without it; its slot holds its own channel.
    compiler.setObjectRendering(ObjectRendering::apart);
    compiler.rebuildNow();
    REQUIRE(compiler.getObjects().size() == 1);
    const auto object = compiler.getObjects().front();
    CHECK(object.numChannels == 1);
    CHECK(object.trackId == track::getId(b));
    const auto apart = play(app, 14, 0.3);
    CHECK_THAT(bedWithBird - apart[static_cast<std::size_t>(left)][kBlock / 2], WithinAbs(0.2, 1e-4));
    CHECK_THAT(app.engine.getSlotOutput(object.slot).getSample(0, kBlock / 2), WithinAbs(0.2, 1e-5));
    CHECK(std::abs(apart[12][kBlock / 2]) < 1e-9); // (not on an output)

    // Outputs: the bed on 1-12, the object on 13 (for an external renderer).
    compiler.setObjectRendering(ObjectRendering::outputs);
    compiler.rebuildNow();
    const auto outputs = play(app, 14, 0.3);
    CHECK(app.engine.collectTelemetry().surroundRendering == engine::SurroundRendering::renderer);
    CHECK_THAT(outputs[static_cast<std::size_t>(left)][kBlock / 2], WithinAbs(apart[static_cast<std::size_t>(left)][kBlock / 2], 1e-6));
    CHECK_THAT(outputs[12][kBlock / 2], WithinAbs(0.2, 1e-5));
    CHECK(std::abs(outputs[13][kBlock / 2]) < 1e-9);

    // A stereo object: two channels; a muted one is silent but still there.
    track::setFormat(b, TrackFormat::stereo, nullptr);
    compiler.rebuildNow();
    REQUIRE(compiler.getObjects().size() == 1);
    CHECK(compiler.getObjects().front().numChannels == 2);
    const auto stereo = play(app, 14, 0.3);
    CHECK_THAT(stereo[12][kBlock / 2], WithinAbs(0.2, 1e-5));
    CHECK_THAT(stereo[13][kBlock / 2], WithinAbs(0.2, 1e-5)); // (a mono file on a stereo track: both sides)

    // Only 3D-panned tracks can be objects.
    track::setPan3d(b, false, nullptr);
    compiler.rebuildNow();
    CHECK(compiler.getObjects().empty());
}

TEST_CASE("ADM BWF export: the bed and the objects with their movement, sample-aligned", "[adm][export]")
{
    test::TempFolder temp;
    App app; // (no device: the exporter prepares the engine)
    auto& session = app.project.getSession();
    juce::String error;
    const auto bedFile = constant(temp.dir.getChildFile("bed.wav"), 12, 3.0, 0.01f, 0.01f);
    const auto padFile = constant(temp.dir.getChildFile("pad.wav"), 2, 3.0, 0.05f, 0.0f);
    const auto click = test::writeImpulse(temp.dir.getChildFile("click.wav"), 24000, 96000, kRate, 0.5f); // (at 0.5 s into it)
    for (const auto& f : {bedFile, padFile, click})
        REQUIRE(app.project.getAssets().loadNow(f, error) != nullptr);

    session.setMasterFormat(TrackFormat::surround714);
    auto bed = session.addTrack("Bed");
    track::setFormat(bed, TrackFormat::surround714, nullptr);
    session.addClip(bed, bedFile, 0.0, 3.0);
    session.addDevice(bed, "builtin:limiter", "Limiter"); // (latency: the objects must still line up)

    // A bird that flies from straight ahead (30 up) round to the left side over two seconds.
    auto bird = session.addTrack("Bird");
    track::setFormat(bird, TrackFormat::mono, nullptr);
    session.addClip(bird, click, 0.5, 2.0); // (its click at 1.0 s)
    track::setPan3d(bird, true, nullptr);
    track::setElevation(bird, 30.0, nullptr);
    track::setObject(bird, true, nullptr);
    auto lane = session.getAutomation(bird, automation::azimuthTarget(), true, 0.5); // (its first point: 0 degrees at 0 s)
    session.addAutomationPoint(lane, 2.0, automation::spatialToNormalised(automation::azimuthTarget(), 90.0));

    // A stereo pad: two objects, its sides 30 degrees either side of straight ahead, half size.
    auto pad = session.addTrack("Pad");
    session.addClip(pad, padFile, 0.0, 3.0);
    track::setPan3d(pad, true, nullptr);
    track::setSize(pad, 0.5, nullptr);
    track::setObject(pad, true, nullptr);

    Exporter exporter(app.project, app.controller);
    std::optional<juce::Result> result;
    juce::Array<juce::File> written;
    exporter.onFinished = [&](const juce::Result& r, const juce::Array<juce::File>& files)
    {
        result = r;
        written = files;
    };
    ExportSettings settings;
    settings.format = ExportSettings::Format::admBwf;
    settings.destination = temp.dir.getChildFile("atmos.wav");
    settings.startSeconds = 0.0;
    settings.endSeconds = 3.0;
    settings.tailSeconds = 0.0;
    settings.bitDepth = 24;
    settings.includeStems = true; // (ignored: one file)
    REQUIRE(exporter.start(settings).isEmpty());
    REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 60'000));
    INFO(result->getErrorMessage());
    REQUIRE(result->wasOk());
    REQUIRE(written.size() == 1);
    CHECK(app.project.getCompiler().getObjectRendering() == ObjectRendering::inBed); // (put back)

    const auto parsed = parse(settings.destination);
    CHECK(parsed.header == "RIFF");
    const auto* fmt = parsed.find("fmt ");
    REQUIRE(fmt != nullptr);
    CHECK(u16(fmt->data, 2) == 15); // 12 bed channels, the bird, the pad's two sides
    CHECK(u16(fmt->data, 14) == 24);

    // chna: the bed's speakers (7.1.4 common definitions), then the objects.
    const auto entries = chnaEntries(*parsed.find("chna"));
    REQUIRE(entries.size() == 15);
    CHECK(entries[0].trackRef == "AT_00010001_01");
    CHECK(entries[11].packRef == "AP_00010017");
    CHECK(entries[12].trackRef == "AT_00031001_01");
    CHECK(entries[13].packRef == "AP_00031002");
    CHECK(entries[14].packRef == "AP_00031003");

    // The audio: the bed without the objects; the objects on their channels, on the timeline.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(settings.destination));
    REQUIRE(reader != nullptr);
    REQUIRE(reader->numChannels == 15);
    CHECK(reader->lengthInSamples == static_cast<juce::int64>(3.0 * kRate));
    juce::AudioBuffer<float> audio(15, static_cast<int>(reader->lengthInSamples));
    REQUIRE(reader->read(&audio, 0, audio.getNumSamples(), 0, true, true));
    for (int c = 0; c < 12; ++c)
    {
        CHECK_THAT(audio.getSample(c, static_cast<int>(1.5 * kRate)), WithinAbs(0.01 * (c + 1), 1e-4));
        CHECK_THAT(audio.getSample(c, static_cast<int>(1.0 * kRate)), WithinAbs(0.01 * (c + 1), 1e-4)); // (the click is not here)
    }
    int peak = 0;
    for (int i = 0; i < audio.getNumSamples(); ++i)
        if (std::abs(audio.getSample(12, i)) > std::abs(audio.getSample(12, peak)))
            peak = i;
    CHECK(peak == static_cast<int>(1.0 * kRate)); // the click, exactly where it is on the timeline
    CHECK_THAT(audio.getSample(12, peak), WithinAbs(0.5, 1e-4));
    CHECK_THAT(audio.getSample(13, static_cast<int>(kRate)), WithinAbs(0.05, 1e-4));
    CHECK_THAT(audio.getSample(14, static_cast<int>(kRate)), WithinAbs(0.05, 1e-4));

    // The metadata.
    const auto* axml = parsed.find("axml");
    REQUIRE(axml != nullptr);
    const auto xml = juce::XmlDocument::parse(text(axml->data, 0, static_cast<std::size_t>(axml->size)));
    REQUIRE(xml != nullptr);
    const auto* afe = xml->getChildByName("coreMetadata")->getChildByName("format")->getChildByName("audioFormatExtended");
    REQUIRE(afe != nullptr);
    CHECK(afe->getChildByName("audioProgramme")->getStringAttribute("end") == "00:00:03.00000");

    // The bird: many blocks along its flight, from straight ahead at 30 degrees up to the left side.
    const auto* birdFormat = channelFormatNamed(*afe, "Bird");
    REQUIRE(birdFormat != nullptr);
    const auto flight = blocksOf(*birdFormat);
    REQUIRE(flight.size() > 10);
    double covered = 0.0;
    for (std::size_t k = 0; k < flight.size(); ++k)
    {
        CHECK_THAT(flight[k].start, WithinAbs(covered, 2e-5)); // (contiguous)
        covered = flight[k].start + flight[k].duration;
        if (k > 0)
            CHECK(flight[k].x <= flight[k - 1].x + 1e-9); // (always further left)
        CHECK_THAT(flight[k].z, WithinAbs(1.0, 1e-6));   // (30 degrees up: the ceiling's edge)
        // Each block holds where the bird is at its end (a straight lane: 45 degrees a second).
        const double end = flight[k].start + flight[k].duration;
        const auto expected = engine::adm::polarToCartesian(std::min(90.0, 45.0 * end), 30.0, 1.0);
        INFO("block " << k << " ends at " << end << " s");
        CHECK_THAT(flight[k].x, WithinAbs(expected.x, 2e-3));
        CHECK_THAT(flight[k].y, WithinAbs(expected.y, 2e-3));
    }
    CHECK_THAT(covered, WithinAbs(3.0, 2e-5));
    const auto ahead = engine::adm::polarToCartesian(0.0, 30.0, 1.0);
    const auto side = engine::adm::polarToCartesian(90.0, 30.0, 1.0);
    CHECK_THAT(flight.front().x, WithinAbs(ahead.x, 0.02)); // (one block in)
    CHECK_THAT(flight.front().y, WithinAbs(ahead.y, 0.02));
    CHECK_THAT(flight.back().x, WithinAbs(side.x, 1e-4)); // (there from 2 s on)
    CHECK_THAT(flight.back().y, WithinAbs(side.y, 1e-4));

    // The pad: still, one block per side, at +-30 degrees, half size.
    const auto* padLeft = channelFormatNamed(*afe, "Pad L");
    const auto* padRight = channelFormatNamed(*afe, "Pad R");
    REQUIRE(padLeft != nullptr);
    REQUIRE(padRight != nullptr);
    const auto l = blocksOf(*padLeft);
    const auto r = blocksOf(*padRight);
    REQUIRE(l.size() == 1);
    REQUIRE(r.size() == 1);
    CHECK_THAT(l[0].x, WithinAbs(-1.0, 1e-6));
    CHECK_THAT(r[0].x, WithinAbs(1.0, 1e-6));
    CHECK_THAT(l[0].y, WithinAbs(1.0, 1e-6));
    CHECK_THAT(l[0].width, WithinAbs(0.5, 1e-6));
    CHECK_THAT(l[0].duration, WithinAbs(3.0, 2e-5));
}

TEST_CASE("ADM BWF export without objects: the bed alone; FLAC-only limits do not apply", "[adm][export]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    juce::String error;
    const auto file = constant(temp.dir.getChildFile("wide.wav"), 16, 1.0, 0.02f, 0.01f);
    REQUIRE(app.project.getAssets().loadNow(file, error) != nullptr);
    session.setMasterFormat(TrackFormat::surround916);
    auto t = session.addTrack("Wide");
    track::setFormat(t, TrackFormat::surround916, nullptr);
    session.addClip(t, file, 0.0, 1.0);

    Exporter exporter(app.project, app.controller);
    std::optional<juce::Result> result;
    exporter.onFinished = [&](const juce::Result& r, const juce::Array<juce::File>&) { result = r; };
    ExportSettings settings;
    settings.format = ExportSettings::Format::admBwf;
    settings.destination = temp.dir.getChildFile("wide.wav");
    settings.endSeconds = 1.0;
    settings.tailSeconds = 0.0;
    settings.bitDepth = 32;
    REQUIRE(exporter.start(settings).isEmpty());
    REQUIRE(test::pumpMessagesUntil([&] { return result.has_value(); }, 60'000));
    REQUIRE(result->wasOk());

    const auto parsed = parse(settings.destination);
    CHECK(u16(parsed.find("fmt ")->data, 0) == 3); // float
    const auto entries = chnaEntries(*parsed.find("chna"));
    REQUIRE(entries.size() == 16);
    for (const auto& e : entries)
        CHECK(e.packRef == "AP_00011001"); // (9.1.6: a pack of the file's own, of common channels)
    const auto xml = juce::XmlDocument::parse(text(parsed.find("axml")->data, 0, static_cast<std::size_t>(parsed.find("axml")->size)));
    REQUIRE(xml != nullptr);
    const auto* afe = xml->getChildByName("coreMetadata")->getChildByName("format")->getChildByName("audioFormatExtended");
    CHECK(afe->getNumChildElements() == 1 + 1 + 1 + 1 + 16); // programme, content, object, pack, track UIDs
}

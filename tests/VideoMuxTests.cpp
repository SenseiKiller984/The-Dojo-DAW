#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "ui/components/ExportDialog.h"
#include "video/PlatformDecoders.h"
#include "video/VideoMux.h"
#include "video/VideoSource.h"
#include "TestAudio.h"
#include "TestSupport.h"
#include "TestVideo.h"

#if JUCE_WINDOWS
#include "MediaFoundationTestMovie.h"
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstring>
#include <optional>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using Set = juce::AudioChannelSet;

namespace
{
constexpr double kRate = 48'000.0;

std::size_t at(double seconds) { return static_cast<std::size_t>(std::llround(seconds * kRate)); }

std::vector<float> channelOf(const juce::AudioBuffer<float>& audio, int c)
{
    return {audio.getReadPointer(c), audio.getReadPointer(c) + audio.getNumSamples()};
}

/** A mix of `channels` channels, `seconds` long: channel c an impulse of (c + 1) / 10 at `first` +
    c * `step` seconds. */
juce::File impulses(const juce::File& file, int channels, double seconds, double first, double step)
{
    std::vector<std::vector<float>> audio(static_cast<std::size_t>(channels), std::vector<float>(at(seconds), 0.0f));
    for (int c = 0; c < channels; ++c)
        audio[static_cast<std::size_t>(c)][at(first + c * step)] = static_cast<float>(c + 1) / 10.0f;
    return test::writeMultichannelWav(file, audio, kRate);
}

/** The channel of `audio` whose impulse is loudest, and where it is. */
std::pair<int, std::size_t> loudest(const juce::AudioBuffer<float>& audio, int c)
{
    const auto samples = channelOf(audio, c);
    return {c, test::indexOfPeak(samples)};
}

/** Whether two pictures are the same (every 4th pixel of every 4th row). */
bool samePicture(const juce::Image& a, const juce::Image& b)
{
    if (!a.isValid() || !b.isValid() || a.getBounds() != b.getBounds())
        return false;
    for (int y = 0; y < a.getHeight(); y += 4)
        for (int x = 0; x < a.getWidth(); x += 4)
            if (a.getPixelAt(x, y) != b.getPixelAt(x, y))
                return false;
    return true;
}

/** The WAVEFORMATEXTENSIBLE channel mask of an AVI's sound (0 when it has none). */
std::uint32_t aviChannelMask(const juce::File& avi)
{
    juce::MemoryBlock data;
    avi.loadFileAsData(data);
    const auto* bytes = static_cast<const std::uint8_t*>(data.getData());
    const auto u32 = [bytes](std::size_t i)
    {
        return static_cast<std::uint32_t>(bytes[i]) | (static_cast<std::uint32_t>(bytes[i + 1]) << 8) | (static_cast<std::uint32_t>(bytes[i + 2]) << 16)
               | (static_cast<std::uint32_t>(bytes[i + 3]) << 24);
    };
    int found = 0;
    for (std::size_t i = 0; i + 48 < data.getSize(); ++i)
        if (std::memcmp(bytes + i, "strf", 4) == 0 && ++found == 2) // (the second stream's format: the sound)
            return u32(i + 4) >= 40 && (u32(i + 8) & 0xffff) == 0xfffe ? u32(i + 8 + 20) : 0;
    return 0;
}

/** The application's object graph, without an audio device. */
struct App
{
    App()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); })
    {
    }

    struct MessageThread
    {
        MessageThread() { juce::MessageManager::getInstance(); }
    } messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    session::Project project;
};

/** Adds a clip of `file`, its audio decoded first (clips load on background threads, and a render
    plays a clip still loading as silence). */
juce::ValueTree addLoadedClip(App& app, juce::ValueTree track, const juce::File& file, double start, double length)
{
    juce::String error;
    REQUIRE(app.project.getAssets().loadNow(file, error) != nullptr);
    return app.project.getSession().addClip(track, file, start, length);
}

/** Runs an export to the end. */
juce::Result runExport(App& app, const session::ExportSettings& settings, juce::Array<juce::File>* written = nullptr)
{
    session::Exporter exporter(app.project, app.controller);
    std::optional<juce::Result> result;
    exporter.onFinished = [&](const juce::Result& r, const juce::Array<juce::File>& files)
    {
        result = r;
        if (written != nullptr)
            *written = files;
    };
    if (const auto error = exporter.start(settings); error.isNotEmpty())
        return juce::Result::fail(error);
    if (!test::pumpMessagesUntil([&] { return result.has_value(); }, 60'000))
        return juce::Result::fail("timed out");
    return *result;
}

template <typename Component, typename Predicate>
Component* findChild(juce::Component& parent, Predicate&& matches)
{
    for (auto* child : parent.getChildren())
    {
        if (auto* c = dynamic_cast<Component*>(child); c != nullptr && matches(*c))
            return c;
        if (auto* found = findChild<Component>(*child, matches))
            return found;
    }
    return nullptr;
}
} // namespace

//==================================================================================================
// The built-in muxer (every platform): a Motion-JPEG AVI with PCM sound

TEST_CASE("The built-in muxer copies a Motion-JPEG AVI's frames and puts the mix under them", "[video][mux]")
{
    test::TempFolder temp;
    const auto movie = test::writeTestVideo(temp.dir.getChildFile("count.avi"), 30, 10.0); // 3 s
    // The mix: an impulse at 0.7 s on the left, at 0.5 s on the right.
    std::vector<std::vector<float>> mixData(2, std::vector<float>(at(2.0), 0.0f));
    mixData[0][at(0.7)] = 0.5f;
    mixData[1][at(0.5)] = -0.25f;
    const auto mix = test::writeMultichannelWav(temp.dir.getChildFile("mix.wav"), mixData, kRate);

    const auto support = video::muxSupport(movie);
    CHECK(support.backend == "built-in (AVI)");
    CHECK(support.extension == ".avi");
    CHECK(support.pcmExtension == ".avi");
    CHECK(support.pcm);
    CHECK(support.aacChannels == 0);
    CHECK_FALSE(support.aacHolds(2));
    // Every frame of a Motion-JPEG AVI is a key frame: a copy can start at any of them.
    CHECK_THAT(video::keyFrameAtOrBefore(movie, 0.55), WithinAbs(0.5, 1e-9));
    CHECK_THAT(video::keyFrameAtOrBefore(movie, 0.5), WithinAbs(0.5, 1e-9));
    CHECK_THAT(video::keyFrameAtOrBefore(movie, 0.0), WithinAbs(0.0, 1e-9));

    video::MuxSettings s;
    s.video = movie;
    s.from = 0.5;
    s.to = 1.5;
    s.audio = mix;
    s.audioOffset = 0.2;
    s.destination = temp.dir.getChildFile("out.avi");
    s.codec = video::MuxAudioCodec::pcm;
    std::vector<double> progress;
    const auto result = video::muxAudioIntoVideo(s, [&](double p)
                                                 {
                                                     progress.push_back(p);
                                                     return true;
                                                 });
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    REQUIRE_FALSE(progress.empty());
    CHECK_THAT(progress.back(), WithinAbs(1.0, 1e-9));

    // The picture: frames 5 to 14, as they were.
    juce::String error;
    auto original = video::openVideo(movie, error);
    auto copy = video::openVideo(s.destination, error);
    REQUIRE(copy != nullptr);
    CHECK(copy->getInfo().backend == "Motion-JPEG AVI");
    CHECK_THAT(copy->getInfo().durationSeconds, WithinAbs(1.0, 1e-9));
    CHECK_THAT(copy->getInfo().frameRate, WithinAbs(10.0, 1e-9));
    CHECK(test::frameNumberOf(copy->getFrame(0.0)) == 5);
    CHECK(test::frameNumberOf(copy->getFrame(0.95)) == 14);
    CHECK(samePicture(copy->getFrame(0.35), original->getFrame(0.85))); // (the same JPEG)

    // The sound: one second of 24-bit PCM, the mix 0.2 s in, silence before it.
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    CHECK(rate == kRate);
    REQUIRE(audio.getNumChannels() == 2);
    CHECK(audio.getNumSamples() == static_cast<int>(at(1.0)));
    CHECK(loudest(audio, 0).second == at(0.9));
    CHECK(loudest(audio, 1).second == at(0.7));
    CHECK_THAT(audio.getSample(0, static_cast<int>(at(0.9))), WithinAbs(0.5, 1e-6));
    CHECK_THAT(audio.getSample(1, static_cast<int>(at(0.7))), WithinAbs(-0.25, 1e-6));
    CHECK(test::peak(channelOf(audio, 0), 0, at(0.2)) == 0.0);
    CHECK(aviChannelMask(s.destination) == 0); // (stereo: a plain WAVEFORMATEX)

    // AAC is refused (no encoder here), the movie itself is never written over, a cancel leaves
    // nothing behind.
    auto aac = s;
    aac.codec = video::MuxAudioCodec::aac;
    aac.destination = temp.dir.getChildFile("aac.avi");
    CHECK(video::muxAudioIntoVideo(aac).getErrorMessage().contains("choose PCM"));
    CHECK_FALSE(aac.destination.exists());
    auto over = s;
    over.destination = movie;
    CHECK(video::muxAudioIntoVideo(over).failed());
    CHECK(video::openVideo(movie, error) != nullptr);
    auto cancelled = s;
    cancelled.destination = temp.dir.getChildFile("cancelled.avi");
    const auto stopped = video::muxAudioIntoVideo(cancelled, [](double p) { return p < 0.5; });
    CHECK(stopped.failed());
    CHECK(stopped.getErrorMessage().contains("cancelled"));
    CHECK_FALSE(cancelled.destination.exists());

    // Without an end, to the movie's end; a mix shorter than the stretch ends in silence.
    auto rest = s;
    rest.to = 0.0;
    rest.from = 2.0;
    rest.audioOffset = 0.0;
    rest.destination = temp.dir.getChildFile("rest.avi");
    REQUIRE(video::muxAudioIntoVideo(rest).wasOk());
    auto restCopy = video::openVideo(rest.destination, error);
    REQUIRE(restCopy != nullptr);
    CHECK_THAT(restCopy->getInfo().durationSeconds, WithinAbs(1.0, 1e-9));
    CHECK(test::frameNumberOf(restCopy->getFrame(0.0)) == 20);
}

TEST_CASE("The mix's speakers go into the movie in its container's order, named", "[video][mux]")
{
    CHECK(video::speakerBit(Set::left) == 0);
    CHECK(video::speakerBit(Set::LFE) == 3);
    CHECK(video::speakerBit(Set::leftSurround) == 4); // (5.1's surrounds are its backs)
    CHECK(video::speakerBit(Set::leftSurroundRear) == 4);
    CHECK(video::speakerBit(Set::leftSurroundSide) == 9);
    CHECK(video::speakerBit(Set::topFrontLeft) == 12);
    CHECK(video::speakerBit(Set::topRearRight) == 17);
    CHECK(video::speakerBit(Set::wideLeft) == 31);
    CHECK(video::speakerBit(Set::topSideRight) == 37);
    CHECK(video::speakerBit(Set::discreteChannel0) == -1);

    // The exporter names a layout's speakers: 5.1's surrounds are backs, 7.1's sides and backs.
    using session::Exporter;
    // (and asks for 320 kbps of AAC for each pair of channels)
    const session::ExportSettings defaults;
    CHECK(Exporter::videoAacBitrate(defaults, 1) == 320'000);
    CHECK(Exporter::videoAacBitrate(defaults, 2) == 320'000);
    CHECK(Exporter::videoAacBitrate(defaults, 6) == 960'000);
    CHECK(Exporter::videoAacBitrate(defaults, 8) == 1'280'000);
    CHECK(Exporter::movieSpeakers(engine::ChannelLayout::surround51())
          == juce::Array<Set::ChannelType>{Set::left, Set::right, Set::centre, Set::LFE, Set::leftSurround, Set::rightSurround});
    CHECK(Exporter::movieSpeakers(engine::ChannelLayout::surround71())
          == juce::Array<Set::ChannelType>{Set::left, Set::right, Set::centre, Set::LFE, Set::leftSurroundSide, Set::rightSurroundSide,
                                           Set::leftSurroundRear, Set::rightSurroundRear});
    CHECK(Exporter::movieSpeakers(engine::ChannelLayout::dolbyDigital())
          == juce::Array<Set::ChannelType>{Set::left, Set::centre, Set::right, Set::leftSurround, Set::rightSurround, Set::LFE});
    const auto tops = Exporter::movieSpeakers(engine::ChannelLayout::surround916());
    REQUIRE(tops.size() == 16);
    CHECK(tops[8] == Set::wideLeft);
    CHECK(tops[12] == Set::topSideLeft);
    CHECK(Exporter::movieSpeakers(engine::ChannelLayout::mono()) == juce::Array<Set::ChannelType>{Set::centre});

    // A Dolby Digital mix (L C R Ls Rs LFE) goes into the AVI in WAVE order (L R C LFE Ls Rs),
    // its mask saying so.
    test::TempFolder temp;
    const auto movie = test::writeTestVideo(temp.dir.getChildFile("count.avi"), 10, 10.0);
    video::MuxSettings s;
    s.video = movie;
    s.to = 1.0;
    s.audio = impulses(temp.dir.getChildFile("ac3.wav"), 6, 1.0, 0.1, 0.1);
    s.destination = temp.dir.getChildFile("ac3.avi");
    s.codec = video::MuxAudioCodec::pcm;
    s.speakers = Exporter::movieSpeakers(engine::ChannelLayout::dolbyDigital());
    REQUIRE(video::muxAudioIntoVideo(s).wasOk());
    CHECK(aviChannelMask(s.destination) == 0x3f);
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    juce::String error;
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 6);
    const int source[] = {0, 2, 1, 5, 3, 4}; // file channel k = the mix's channel source[k]
    for (int k = 0; k < 6; ++k)
    {
        INFO("channel " << k);
        CHECK(loudest(audio, k).second == at(0.1 + source[k] * 0.1));
        CHECK_THAT(audio.getSample(k, static_cast<int>(at(0.1 + source[k] * 0.1))), WithinAbs((source[k] + 1) / 10.0, 1e-6));
    }

    // 9.1.6: the twelve WAVE speakers first (L R C LFE, backs, sides, tops), then the wides and top
    // sides, which WAVE has no names for.
    s.audio = impulses(temp.dir.getChildFile("916.wav"), 16, 1.0, 0.02, 0.05);
    s.destination = temp.dir.getChildFile("916.avi");
    s.speakers = Exporter::movieSpeakers(engine::ChannelLayout::surround916());
    REQUIRE(video::muxAudioIntoVideo(s).wasOk());
    CHECK(aviChannelMask(s.destination) == (0x3fu | 0x600u | 0x1000u | 0x4000u | 0x8000u | 0x20000u));
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 16);
    // 9.1.6 is L R C LFE Ls Rs Lrs Rrs Lw Rw Ltf Rtf Ltm Rtm Ltr Rtr.
    const int wide[] = {0, 1, 2, 3, 6, 7, 4, 5, 10, 11, 14, 15, 8, 9, 12, 13};
    for (int k = 0; k < 16; ++k)
    {
        INFO("channel " << k);
        CHECK(loudest(audio, k).second == at(0.02 + wide[k] * 0.05));
    }
}

//==================================================================================================
// The exporter: the mix for the clip's stretch, into its movie

TEST_CASE("Exporting the mix into the video: the clip's stretch, from its key frame, in step with the picture", "[video][export]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    const auto movie = test::writeTestVideo(temp.dir.getChildFile("picture.avi"), 30, 10.0); // 3 s
    auto picture = session.addTrack(session::TrackKind::video, "Picture");
    // The clip shows the movie from 0.55 s on, from 1.0 s on the timeline, for a second.
    auto clip = session.addVideoClip(picture, movie, 1.0, 1.0, 0.55);
    REQUIRE(clip.isValid());
    auto dialogue = session.addTrack(session::TrackKind::audio, "Dialogue");
    addLoadedClip(app, dialogue, test::writeImpulse(temp.dir.getChildFile("click.wav"), static_cast<int>(at(1.3)), static_cast<int>(at(4.0)), kRate),
                  0.0, 4.0);

    session::ExportSettings settings;
    settings.format = session::ExportSettings::Format::video;
    settings.videoAudio = video::MuxAudioCodec::pcm;
    settings.destination = temp.dir.getChildFile("film.mp4"); // (the movie's container wins: .avi)
    settings.startSeconds = 0.0;
    settings.endSeconds = 10.0; // (the clip decides)
    settings.tailSeconds = 5.0;
    settings.includeStems = true;
    juce::Array<juce::File> written;
    const auto result = runExport(app, settings, &written);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    REQUIRE(written.size() == 1);
    const auto film = temp.dir.getChildFile("film.avi");
    CHECK(written[0] == film);
    CHECK_FALSE(temp.dir.getChildFile("film.mp4").exists());
    CHECK_FALSE(temp.dir.getChildFile("film - Dialogue.avi").exists()); // (no stems)
    CHECK_FALSE(temp.dir.getChildFile("film - mix (temporary).wav").exists()); // (the mix waited beside it, and is gone)

    // The copy starts at the frame under the clip's start (0.5 s into the movie: 0.95 on the
    // timeline) and ends with the clip (1.55 s into the movie).
    juce::String error;
    auto copy = video::openVideo(film, error);
    REQUIRE(copy != nullptr);
    CHECK(test::frameNumberOf(copy->getFrame(0.0)) == 5);
    CHECK_THAT(copy->getInfo().durationSeconds, WithinAbs(1.1, 1e-9));
    // The click at 1.3 s on the timeline, while the session shows the movie's 0.85 s (frame 8): in
    // the copy, where frame 8 is.
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    REQUIRE(video::decodeAudio(film, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 2);
    CHECK(loudest(audio, 0).second == at(1.3 - 0.95));
    CHECK(test::peak(channelOf(audio, 0)) > 0.1);
    CHECK(test::frameNumberOf(copy->getFrame(1.3 - 0.95)) == 8);

    // A clip at the timeline's start that shows the movie from 0.55 s: the copy starts 0.05 s
    // before the timeline does, so the mix starts after 0.05 s of silence.
    session.moveClip(clip, picture, 0.0);
    session::track::setMuted(dialogue, true, nullptr);
    auto second = session.addTrack(session::TrackKind::audio, "Second click");
    addLoadedClip(app, second, test::writeImpulse(temp.dir.getChildFile("early.wav"), static_cast<int>(at(0.3)), static_cast<int>(at(2.0)), kRate),
                  0.0, 2.0);
    const auto early = runExport(app, settings);
    INFO(early.getErrorMessage());
    REQUIRE(early.wasOk());
    REQUIRE(video::decodeAudio(film, audio, rate, error));
    CHECK(loudest(audio, 0).second == at(0.3 + 0.05));
    CHECK(test::peak(channelOf(audio, 0), 0, at(0.05)) == 0.0);
}

TEST_CASE("A video export says why it cannot run", "[video][export]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    session::Exporter exporter(app.project, app.controller);
    session::ExportSettings settings;
    settings.format = session::ExportSettings::Format::video;
    settings.destination = temp.dir.getChildFile("film.avi");
    settings.endSeconds = 1.0;
    CHECK(exporter.start(settings).contains("no video clip"));

    auto picture = session.addTrack(session::TrackKind::video, "Picture");
    auto clip = session.addVideoClip(picture, test::writeTestVideo(temp.dir.getChildFile("p.avi"), 20, 10.0), 0.0, 2.0);
    session::track::setMuted(picture, true, nullptr); // (a muted video track is not seen)
    CHECK(exporter.start(settings).contains("no video clip"));
    session::track::setMuted(picture, false, nullptr);

    session::clip::setLoop(clip, true, 0.5, nullptr);
    CHECK(exporter.start(settings).contains("looped"));
    session::clip::setLoop(clip, false, 0.5, nullptr);

    session.setSpeed(2.0);
    CHECK(exporter.start(settings).contains("speed"));
    session.setSpeed(1.0);

    settings.videoAudio = video::MuxAudioCodec::aac; // (the built-in AVI muxer has no AAC)
    CHECK(exporter.start(settings).contains("choose PCM"));
    CHECK_FALSE(exporter.isRunning());
}

TEST_CASE("Exporting a Dolby Digital mix into the video: its speakers in the movie's order", "[video][export]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    session.setMasterFormat(session::TrackFormat::dolbyDigital);
    auto picture = session.addTrack(session::TrackKind::video, "Picture");
    session.addVideoClip(picture, test::writeTestVideo(temp.dir.getChildFile("p.avi"), 20, 10.0), 0.0, 2.0);
    auto stems = session.addTrack(session::TrackKind::audio, "5.1 stem");
    session::track::setFormat(stems, session::TrackFormat::dolbyDigital, nullptr);
    addLoadedClip(app, stems, impulses(temp.dir.getChildFile("ac3.wav"), 6, 2.0, 0.1, 0.2), 0.0, 2.0);

    session::ExportSettings settings;
    settings.format = session::ExportSettings::Format::video;
    settings.videoAudio = video::MuxAudioCodec::pcm;
    settings.destination = temp.dir.getChildFile("film.avi");
    settings.endSeconds = 2.0;
    const auto result = runExport(app, settings);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    CHECK(aviChannelMask(settings.destination) == 0x3f);
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    juce::String error;
    REQUIRE(video::decodeAudio(settings.destination, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 6);
    const int source[] = {0, 2, 1, 5, 3, 4}; // L R C LFE Ls Rs from L C R Ls Rs LFE
    for (int k = 0; k < 6; ++k)
    {
        INFO("channel " << k);
        CHECK(loudest(audio, k).second == at(0.1 + source[k] * 0.2));
    }
}

TEST_CASE("The export dialog offers the video when there is a clip to put the mix into", "[video][ui]")
{
    test::TempFolder temp;
    App app;
    auto& session = app.project.getSession();
    session::Exporter exporter(app.project, app.controller);
    {
        ui::ExportDialog dialog(app.project, exporter);
        auto* format = findChild<juce::ComboBox>(dialog, [](juce::ComboBox& c) { return c.getItemText(0) == "WAV"; });
        REQUIRE(format != nullptr);
        REQUIRE(format->getNumItems() == 5);
        CHECK(format->getItemText(4).startsWith("Video"));
        CHECK_FALSE(format->isItemEnabled(5)); // (no video clip)
        CHECK(format->getTooltip().contains("Import Video"));
    }

    auto picture = session.addTrack(session::TrackKind::video, "Picture");
    session.addVideoClip(picture, test::writeTestVideo(temp.dir.getChildFile("Trailer.avi"), 20, 10.0), 1.0, 2.0);
    ui::ExportDialog dialog(app.project, exporter);
    auto* format = findChild<juce::ComboBox>(dialog, [](juce::ComboBox& c) { return c.getItemText(0) == "WAV"; });
    REQUIRE(format != nullptr);
    REQUIRE(format->isItemEnabled(5));
    auto* stems = findChild<juce::ToggleButton>(dialog, [](juce::ToggleButton& b) { return b.getButtonText().startsWith("Stems"); });
    auto* range = findChild<juce::ComboBox>(dialog, [](juce::ComboBox& c) { return c.getItemText(0) == "Whole session"; });
    auto* mode = findChild<juce::ComboBox>(dialog, [](juce::ComboBox& c) { return c.getItemText(0).startsWith("Offline"); });
    REQUIRE(stems != nullptr);
    REQUIRE(range != nullptr);
    REQUIRE(mode != nullptr);

    format->setSelectedId(5, juce::sendNotificationSync);
    CHECK_FALSE(stems->isVisible());
    CHECK_FALSE(range->isEnabled()); // (the clip's stretch)
    CHECK(mode->getSelectedId() == 1);
    CHECK_FALSE(mode->isItemEnabled(2));
    auto* quality = findChild<juce::ComboBox>(dialog, [](juce::ComboBox& c) { return c.getItemText(0).startsWith("PCM"); });
    REQUIRE(quality != nullptr); // (the built-in AVI muxer: PCM only)
    CHECK(quality->getNumItems() == 1);
    auto* hint = findChild<juce::Label>(dialog, [](juce::Label& l) { return l.getText().startsWith("A copy of"); });
    REQUIRE(hint != nullptr);
    CHECK(hint->isVisible());
    CHECK(hint->getText().contains("Trailer.avi"));
    CHECK(hint->getText().contains("0:01.000 - 0:03.000"));
    CHECK(hint->getText().contains("built-in (AVI)"));
    auto* destination = findChild<juce::Label>(dialog, [](juce::Label& l) { return l.getText().endsWith(".avi"); });
    CHECK(destination != nullptr); // (the movie's container)

    format->setSelectedId(1, juce::sendNotificationSync);
    CHECK(stems->isVisible());
    CHECK(range->isEnabled());
    CHECK_FALSE(hint->isVisible());
    CHECK(findChild<juce::Label>(dialog, [](juce::Label& l) { return l.getText().endsWith(".wav"); }) != nullptr);
}

//==================================================================================================
// FFmpeg (Linux): MP4, MOV, MKV... with AAC or PCM

#if DOJO_HAS_FFMPEG
namespace
{
/** Runs a command line tool to its end; false if it is not there or fails. */
bool runTool(const juce::StringArray& command, juce::String* output = nullptr)
{
    juce::ChildProcess process;
    if (!process.start(command))
        return false;
    const auto text = process.readAllProcessOutput();
    if (output != nullptr)
        *output = text.trim();
    return process.waitForProcessToFinish(60'000) && process.getExitCode() == 0;
}

/** What ffprobe says about the sound of a file ("aac,5.1"). */
juce::String probeSound(const juce::File& file)
{
    juce::String text;
    runTool({"ffprobe", "-v", "error", "-select_streams", "a", "-show_entries", "stream=codec_name,channel_layout", "-of", "csv=p=0",
             file.getFullPathName()},
            &text);
    return text;
}

juce::String probeStreams(const juce::File& file)
{
    juce::String text;
    runTool({"ffprobe", "-v", "error", "-show_entries", "stream=codec_type", "-of", "csv=p=0", file.getFullPathName()}, &text);
    return text.replace("\n", ",");
}
} // namespace

TEST_CASE("FFmpeg copies an H.264 movie's packets from a key frame and encodes the mix (AAC or PCM)", "[video][mux][ffmpeg]")
{
    test::TempFolder temp;
    const auto movie = temp.dir.getChildFile("movie.mp4");
    // Four seconds, a key frame every second (and B-frames between), with a sound of its own.
    if (!runTool({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=25:duration=4", "-f", "lavfi", "-i",
                  "sine=frequency=440:sample_rate=48000:duration=4", "-c:v", "libx264", "-pix_fmt", "yuv420p", "-x264-params",
                  "keyint=25:min-keyint=25:scenecut=0", "-bf", "2", "-c:a", "aac", "-shortest", movie.getFullPathName()})
        || !movie.existsAsFile())
        SKIP("no ffmpeg tool with libx264 to make a test movie");

    const auto support = video::muxSupport(movie);
    CHECK(support.backend.startsWith("FFmpeg"));
    CHECK(support.extension == ".mp4");
    CHECK(support.pcmExtension == ".mov"); // (MP4's own PCM is not widely read)
    CHECK(support.pcm);
    CHECK(support.aacChannels == 8);
    CHECK(support.aacHolds(6));
    CHECK_FALSE(support.aacHolds(12));
    CHECK_THAT(video::keyFrameAtOrBefore(movie, 1.7), WithinAbs(1.0, 1e-3));
    CHECK_THAT(video::keyFrameAtOrBefore(movie, 2.0), WithinAbs(2.0, 1e-3));
    CHECK_THAT(video::keyFrameAtOrBefore(movie, 0.3), WithinAbs(0.0, 1e-3));
    CHECK(video::muxSupport(temp.dir.getChildFile("clip.webm")).backend.isEmpty()); // (not there)

    std::vector<std::vector<float>> mixData(2, std::vector<float>(at(3.0), 0.0f));
    mixData[0][at(1.0)] = 0.5f;
    mixData[1][at(1.2)] = -0.5f;
    const auto mix = test::writeMultichannelWav(temp.dir.getChildFile("mix.wav"), mixData, kRate);

    video::MuxSettings s;
    s.video = movie;
    s.from = 1.0;
    s.to = 3.0;
    s.audio = mix;
    s.audioOffset = 0.25;
    s.destination = temp.dir.getChildFile("out.mp4");
    s.codec = video::MuxAudioCodec::aac;
    const auto result = video::muxAudioIntoVideo(s);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    CHECK(probeStreams(s.destination) == "video,audio"); // (the movie's own sound is gone)
    CHECK(probeSound(s.destination) == "aac,stereo");

    // The picture's packets as they were: from the key frame at 1 s, the same pictures.
    juce::String error;
    auto original = video::openVideo(movie, error);
    auto copy = video::openVideo(s.destination, error);
    REQUIRE(copy != nullptr);
    CHECK(copy->getInfo().codec == "h264");
    CHECK(copy->getInfo().width == 160);
    CHECK_THAT(copy->getInfo().frameRate, WithinAbs(25.0, 1e-6));
    CHECK_THAT(copy->getInfo().durationSeconds, WithinAbs(2.0, 0.05));
    for (const double t : {0.0, 0.52, 1.0, 1.96})
    {
        INFO("at " << t);
        CHECK(samePicture(copy->getFrame(t), original->getFrame(1.0 + t)));
    }

    // The sound: the mix 0.25 s in, in step (AAC's priming is trimmed by the file's edit list).
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    CHECK(rate == kRate);
    REQUIRE(audio.getNumChannels() == 2);
    CHECK(std::abs(audio.getNumSamples() - static_cast<int>(at(2.0))) <= 1024);
    const auto left = loudest(audio, 0).second;
    const auto right = loudest(audio, 1).second;
    INFO("left peak " << left << ", right peak " << right);
    CHECK(std::abs(static_cast<double>(left) - static_cast<double>(at(1.25))) <= 24.0);
    CHECK(std::abs(static_cast<double>(right) - static_cast<double>(at(1.45))) <= 24.0);
    CHECK(test::rms(channelOf(audio, 0), 0, at(0.2)) < 1e-3); // (no 440 Hz: only the mix)

    // PCM in a MOV: exact.
    s.codec = video::MuxAudioCodec::pcm;
    s.destination = temp.dir.getChildFile("out.mov");
    REQUIRE(video::muxAudioIntoVideo(s).wasOk());
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    CHECK(audio.getNumSamples() == static_cast<int>(at(2.0)));
    CHECK(loudest(audio, 0).second == at(1.25));
    CHECK_THAT(audio.getSample(0, static_cast<int>(at(1.25))), WithinAbs(0.5, 1e-6));
    CHECK(loudest(audio, 1).second == at(1.45));
    copy = video::openVideo(s.destination, error);
    REQUIRE(copy != nullptr);
    CHECK(samePicture(copy->getFrame(0.52), original->getFrame(1.52)));

    // PCM in an MP4 all the same: 32-bit, so that it reads back (FFmpeg 6.1 misreads 24-bit 'ipcm').
    s.destination = temp.dir.getChildFile("pcm.mp4");
    REQUIRE(video::muxAudioIntoVideo(s).wasOk());
    CHECK(probeSound(s.destination) == "pcm_s32le,stereo");
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    CHECK(audio.getNumSamples() == static_cast<int>(at(2.0)));
    CHECK(loudest(audio, 0).second == at(1.25));
    CHECK_THAT(audio.getSample(1, static_cast<int>(at(1.45))), WithinAbs(-0.5, 1e-6));

    // A cancel leaves nothing.
    s.destination = temp.dir.getChildFile("cancelled.mp4");
    CHECK(video::muxAudioIntoVideo(s, [](double p) { return p < 0.3; }).failed());
    CHECK_FALSE(s.destination.exists());
}

TEST_CASE("FFmpeg names the mix's speakers: 5.1 and 7.1 in AAC, 7.1.4 in PCM", "[video][mux][ffmpeg]")
{
    test::TempFolder temp;
    const auto movie = temp.dir.getChildFile("movie.mkv");
    if (!runTool({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=25:duration=2", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                  movie.getFullPathName()})
        || !movie.existsAsFile())
        SKIP("no ffmpeg tool with libx264 to make a test movie");
    CHECK(video::muxSupport(movie).extension == ".mkv");

    using session::Exporter;
    video::MuxSettings s;
    s.video = movie;
    s.to = 2.0;

    // DTS order (L R Ls Rs C LFE) in AAC: FFmpeg's 5.1 (L R C LFE and the backs).
    s.audio = impulses(temp.dir.getChildFile("dts.wav"), 6, 2.0, 0.1, 0.2);
    s.speakers = Exporter::movieSpeakers(engine::ChannelLayout::dts());
    s.codec = video::MuxAudioCodec::aac;
    s.destination = temp.dir.getChildFile("dts.mkv");
    auto result = video::muxAudioIntoVideo(s);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    CHECK(probeSound(s.destination) == "aac,5.1");
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    juce::String error;
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 6);
    const int dts[] = {0, 1, 4, 5, 2, 3}; // FL FR FC LFE BL BR from L R Ls Rs C LFE
    for (const int k : {0, 1, 2, 4, 5})   // (the LFE's band stops below an impulse's)
    {
        INFO("channel " << k);
        CHECK(std::abs(static_cast<double>(loudest(audio, k).second) - static_cast<double>(at(0.1 + dts[k] * 0.2))) <= 24.0);
    }

    // 7.1 (L R C LFE Ls Rs Lrs Rrs) in AAC: FFmpeg's 7.1 puts the backs before the sides.
    s.audio = impulses(temp.dir.getChildFile("71.wav"), 8, 2.0, 0.1, 0.2);
    s.speakers = Exporter::movieSpeakers(engine::ChannelLayout::surround71());
    s.destination = temp.dir.getChildFile("71.mp4");
    result = video::muxAudioIntoVideo(s);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    CHECK(probeSound(s.destination) == "aac,7.1");
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 8);
    const int seven[] = {0, 1, 2, 3, 6, 7, 4, 5};
    for (const int k : {0, 1, 2, 4, 5, 6, 7})
    {
        INFO("channel " << k);
        CHECK(std::abs(static_cast<double>(loudest(audio, k).second) - static_cast<double>(at(0.1 + seven[k] * 0.2))) <= 24.0);
    }

    // 7.1.4 (twelve channels) in PCM: FFmpeg's 7.1.4.
    s.audio = impulses(temp.dir.getChildFile("714.wav"), 12, 2.0, 0.05, 0.1);
    s.speakers = Exporter::movieSpeakers(engine::ChannelLayout::surround714());
    s.codec = video::MuxAudioCodec::pcm;
    s.destination = temp.dir.getChildFile("714.mov");
    result = video::muxAudioIntoVideo(s);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    CHECK(probeSound(s.destination) == "pcm_s24le,7.1.4");
    REQUIRE(video::decodeAudio(s.destination, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 12);
    const int tops[] = {0, 1, 2, 3, 6, 7, 4, 5, 8, 9, 10, 11};
    for (int k = 0; k < 12; ++k)
    {
        INFO("channel " << k);
        CHECK(loudest(audio, k).second == at(0.05 + tops[k] * 0.1));
    }

    // Twelve channels do not fit in AAC.
    s.codec = video::MuxAudioCodec::aac;
    s.destination = temp.dir.getChildFile("714.mp4");
    CHECK(video::muxAudioIntoVideo(s).getErrorMessage().contains("choose PCM"));
}

TEST_CASE("FFmpeg keeps an HEVC movie's QuickTime tag (hvc1)", "[video][mux][ffmpeg]")
{
    test::TempFolder temp;
    const auto movie = temp.dir.getChildFile("hevc.mov");
    if (!runTool({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=25:duration=1", "-c:v", "libx265", "-x265-params",
                  "log-level=error", "-tag:v", "hvc1", "-pix_fmt", "yuv420p", movie.getFullPathName()})
        || !movie.existsAsFile())
        SKIP("no ffmpeg tool with libx265 to make a test movie");

    video::MuxSettings s;
    s.video = movie;
    s.to = 1.0;
    s.audio = impulses(temp.dir.getChildFile("mix.wav"), 2, 1.0, 0.1, 0.1);
    s.destination = temp.dir.getChildFile("out.mov");
    REQUIRE(video::muxAudioIntoVideo(s).wasOk());
    juce::String tag;
    runTool({"ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=codec_name,codec_tag_string", "-of", "csv=p=0",
             s.destination.getFullPathName()},
            &tag);
    CHECK(tag == "hevc,hvc1");
}

TEST_CASE("Exporting into an MP4 through FFmpeg: from the key frame before the clip's start", "[video][export][ffmpeg]")
{
    test::TempFolder temp;
    const auto movie = temp.dir.getChildFile("movie.mp4");
    if (!runTool({"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=160x120:rate=25:duration=4", "-c:v", "libx264", "-pix_fmt", "yuv420p",
                  "-x264-params", "keyint=25:min-keyint=25:scenecut=0", movie.getFullPathName()})
        || !movie.existsAsFile())
        SKIP("no ffmpeg tool with libx264 to make a test movie");

    App app;
    auto& session = app.project.getSession();
    session.setMasterFormat(session::TrackFormat::surround714);
    auto picture = session.addTrack(session::TrackKind::video, "Picture");
    // The clip shows the movie from 1.6 s, from 2.0 s on the timeline: the copy starts at the key
    // frame at 1.0 s, 0.6 s earlier (1.4 s on the timeline).
    session.addVideoClip(picture, movie, 2.0, 2.0, 1.6);
    auto dialogue = session.addTrack(session::TrackKind::audio, "Dialogue");
    addLoadedClip(app, dialogue, test::writeImpulse(temp.dir.getChildFile("click.wav"), static_cast<int>(at(2.5)), static_cast<int>(at(5.0)), kRate),
                  0.0, 5.0);

    session::ExportSettings settings;
    settings.format = session::ExportSettings::Format::video;
    settings.videoAudio = video::MuxAudioCodec::aac;
    settings.destination = temp.dir.getChildFile("film.mov");
    settings.endSeconds = 10.0;
    const auto result = runExport(app, settings);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());
    const auto film = temp.dir.getChildFile("film.mp4");
    REQUIRE(film.existsAsFile());
    CHECK(probeSound(film) == "aac,stereo"); // (7.1.4 does not fit in AAC: folded to stereo)

    juce::String error;
    auto original = video::openVideo(movie, error);
    auto copy = video::openVideo(film, error);
    REQUIRE(copy != nullptr);
    CHECK_THAT(copy->getInfo().durationSeconds, WithinAbs(2.6, 0.05)); // (1.0 to 3.6 s of the movie)
    CHECK(samePicture(copy->getFrame(0.0), original->getFrame(1.0)));
    juce::AudioBuffer<float> audio;
    double rate = 0.0;
    REQUIRE(video::decodeAudio(film, audio, rate, error));
    // The click at 2.5 s on the timeline: 1.1 s into the copy, under the movie's 2.1 s.
    const auto click = loudest(audio, 0).second;
    INFO("click at " << click);
    CHECK(std::abs(static_cast<double>(click) - static_cast<double>(at(2.5 - 1.4))) <= 24.0);
    CHECK(samePicture(copy->getFrame(1.1), original->getFrame(2.1)));

    // PCM keeps every channel of the 7.1.4 mix, in a MOV.
    settings.videoAudio = video::MuxAudioCodec::pcm;
    const auto pcm = runExport(app, settings);
    INFO(pcm.getErrorMessage());
    REQUIRE(pcm.wasOk());
    const auto mov = temp.dir.getChildFile("film.mov");
    REQUIRE(mov.existsAsFile());
    CHECK(probeSound(mov) == "pcm_s24le,7.1.4");
    REQUIRE(video::decodeAudio(mov, audio, rate, error));
    REQUIRE(audio.getNumChannels() == 12);
    CHECK(loudest(audio, 0).second == at(2.5 - 1.4));
}
#endif

//==================================================================================================
// Media Foundation (Windows): MP4 with AAC

#if JUCE_WINDOWS
TEST_CASE("Media Foundation copies an H.264 MP4's samples from a key frame and encodes the mix as AAC", "[video][mux][mediafoundation]")
{
    test::TempFolder temp;
    const auto movie = temp.dir.getChildFile("movie.mp4");
    std::string error;
    const bool made = test::mf::writeH264Movie(movie.getFullPathName().toWideCharPointer(), 100, 25, 25, 64, 48,
                                               [](int frame, std::uint8_t* bgra)
                                               {
                                                   // The numbered frames' stripes (see TestVideo.h), top row first.
                                                   for (int y = 0; y < 48; ++y)
                                                       for (int x = 0; x < 64; ++x)
                                                       {
                                                           const int bit = x / test::kStripeWidth;
                                                           const auto v = static_cast<std::uint8_t>(bit < 5 ? ((frame >> bit) & 1 ? 255 : 0) : 96);
                                                           auto* p = bgra + (y * 64 + x) * 4;
                                                           p[0] = p[1] = p[2] = v;
                                                           p[3] = 255;
                                                       }
                                               },
                                               error);
    if (!made)
        SKIP("Media Foundation cannot write an H.264 test movie here: " + error);

    const auto support = video::muxSupport(movie);
    REQUIRE(support.backend == "Media Foundation");
    CHECK(support.extension == ".mp4");
    CHECK_FALSE(support.pcm);
    CHECK(support.aacMaxBitrate == 192000);
    CHECK(support.aacHolds(2));
    CHECK(support.aacHolds(6));
    CHECK_FALSE(support.aacHolds(8));
    const double key = video::keyFrameAtOrBefore(movie, 1.5);
    INFO("key frame at " << key);
    CHECK(key <= 1.5 + 1e-6);
    CHECK(key >= 0.0);

    std::vector<std::vector<float>> mixData(2, std::vector<float>(at(3.0), 0.0f));
    mixData[0][at(0.5)] = 0.5f;
    video::MuxSettings s;
    s.video = movie;
    s.from = key;
    s.to = key + 2.0;
    s.audio = test::writeMultichannelWav(temp.dir.getChildFile("mix.wav"), mixData, kRate);
    s.destination = temp.dir.getChildFile("out.mp4");
    s.codec = video::MuxAudioCodec::aac;
    const auto result = video::muxAudioIntoVideo(s);
    INFO(result.getErrorMessage());
    REQUIRE(result.wasOk());

    juce::String readError;
    auto copy = video::openVideo(s.destination, readError);
    REQUIRE(copy != nullptr);
    CHECK_THAT(copy->getInfo().durationSeconds, WithinAbs(2.0, 0.1));
    CHECK(test::frameNumberOf(copy->getFrame(0.0)) == static_cast<int>(std::lround(key * 25.0)) % 32);

    std::vector<float> sound;
    int channels = 0;
    int rate = 0;
    REQUIRE(test::mf::readSound(s.destination.getFullPathName().toWideCharPointer(), sound, channels, rate, error));
    REQUIRE(channels == 2);
    CHECK(rate == 48000);
    std::size_t best = 0;
    for (std::size_t i = 0; i < sound.size(); i += 2)
        if (std::abs(sound[i]) > std::abs(sound[best]))
            best = i;
    const double click = static_cast<double>(best / 2) / kRate;
    INFO("the click is at " << click << " s (0.5 s in the mix)");
    CHECK(std::abs(click - 0.5) < 0.03);
    CHECK(std::abs(static_cast<double>(sound.size() / 2) / kRate - 2.0) < 0.1);
}
#endif

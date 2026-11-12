#include "video/FrameServer.h"
#include "video/MjpegAvi.h"
#include "video/PlatformDecoders.h"
#include "video/VideoSource.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using Catch::Matchers::WithinAbs;

namespace
{
/** Frame k of the test videos: a flat colour with red = 12 k (so its number can be read back). */
juce::Image numberedFrame(int k, int width = 64, int height = 48)
{
    juce::Image image(juce::Image::RGB, width, height, false);
    image.clear(image.getBounds(), juce::Colour(static_cast<juce::uint8>(12 * k), 80, 160));
    return image;
}

/** Which frame an image is (from its red), or -1. */
int frameNumberOf(const juce::Image& image)
{
    if (!image.isValid())
        return -1;
    const auto colour = image.getPixelAt(image.getWidth() / 2, image.getHeight() / 2);
    return juce::roundToInt(colour.getRed() / 12.0);
}

juce::File writeTestVideo(const juce::File& file, int frames = 10, double frameRate = 10.0)
{
    std::vector<juce::Image> images;
    for (int k = 0; k < frames; ++k)
        images.push_back(numberedFrame(k));
    REQUIRE(video::writeMjpegAvi(file, images, frameRate, 0.95f));
    return file;
}

/** Plays a source through its frames: forward, back, far ahead. */
void checkFrames(video::VideoSource& source)
{
    CHECK(frameNumberOf(source.getFrame(0.0)) == 0);
    CHECK(frameNumberOf(source.getFrame(0.05)) == 0);
    CHECK(frameNumberOf(source.getFrame(0.1)) == 1);  // a frame's own start
    CHECK(frameNumberOf(source.getFrame(0.35)) == 3);
    CHECK(frameNumberOf(source.getFrame(0.36)) == 3); // the same frame again
    CHECK(frameNumberOf(source.getFrame(0.42)) == 4); // forward by one
    CHECK(frameNumberOf(source.getFrame(0.15)) == 1); // back: a seek
    CHECK(frameNumberOf(source.getFrame(0.95)) == 9); // far ahead
    CHECK_FALSE(source.getFrame(1.0).isValid());      // past the end
    CHECK_FALSE(source.getFrame(-0.1).isValid());

    // Scaled to fit a box (thumbnails), keeping the aspect ratio.
    const auto small = source.getFrame(0.55, 32, 32);
    CHECK(small.getWidth() == 32);
    CHECK(small.getHeight() == 24);
    CHECK(frameNumberOf(small) == 5);
}
} // namespace

TEST_CASE("Motion-JPEG AVI files are written and read back frame by frame", "[video]")
{
    test::TempFolder temp;
    const auto file = writeTestVideo(temp.dir.getChildFile("count.avi"));

    juce::String error;
    auto source = video::openVideo(file, error);
    REQUIRE(source != nullptr);
    const auto& info = source->getInfo();
    CHECK(info.backend == "Motion-JPEG AVI");
    CHECK(info.codec == "MJPEG");
    CHECK(info.width == 64);
    CHECK(info.height == 48);
    CHECK_THAT(info.frameRate, WithinAbs(10.0, 1e-9));
    CHECK_THAT(info.durationSeconds, WithinAbs(1.0, 1e-9));
    CHECK(info.frameIndexAt(0.1) == 1);
    checkFrames(*source);

    // Other files are refused (with a reason), not misread.
    const auto wav = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(440.0, 48000.0, 4800), 48000.0);
    CHECK(video::openVideo(wav, error) == nullptr);
    CHECK(error.isNotEmpty());
    CHECK(video::openVideo(temp.dir.getChildFile("missing.mp4"), error) == nullptr);
    CHECK(error.contains("Missing"));

    CHECK(video::isVideoFile(juce::File("/x/clip.MP4")));
    CHECK(video::isVideoFile(juce::File("/x/clip.mov")));
    CHECK_FALSE(video::isVideoFile(juce::File("/x/clip.wav")));
    CHECK(video::getDecoderNames().contains("Motion-JPEG AVI"));
}

#if DOJO_HAS_FFMPEG
TEST_CASE("FFmpeg decodes video files frame by frame, forward and after seeks", "[video][ffmpeg]")
{
    test::TempFolder temp;
    const auto file = writeTestVideo(temp.dir.getChildFile("count.avi"));

    juce::String error;
    auto source = video::openWithFfmpeg(file, error);
    REQUIRE(source != nullptr);
    CHECK(source->getInfo().backend.startsWith("FFmpeg"));
    CHECK(source->getInfo().width == 64);
    CHECK_THAT(source->getInfo().frameRate, WithinAbs(10.0, 1e-6));
    CHECK_THAT(source->getInfo().durationSeconds, WithinAbs(1.0, 0.11));
    checkFrames(*source);
}
#endif

#if JUCE_WINDOWS
TEST_CASE("Media Foundation decodes video files frame by frame (where it can read AVI)", "[video][mediafoundation]")
{
    test::TempFolder temp;
    const auto file = writeTestVideo(temp.dir.getChildFile("count.avi"));

    juce::String error;
    auto source = video::openWithMediaFoundation(file, error);
    if (source == nullptr || !source->getFrame(0.0).isValid())
        SKIP("Media Foundation cannot read Motion-JPEG AVI here: " + error.toStdString());
    CHECK(source->getInfo().backend == "Media Foundation");
    CHECK(source->getInfo().width == 64);
    checkFrames(*source);
}
#endif

TEST_CASE("The frame server decodes in the background for playback and thumbnails", "[video]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    const auto file = writeTestVideo(temp.dir.getChildFile("count.avi"), 20, 10.0);

    video::FrameServer server;
    int ready = 0;
    server.onFramesReady = [&] { ++ready; };

    const auto info = server.getInfo(file);
    REQUIRE(info.has_value());
    CHECK_THAT(info->durationSeconds, WithinAbs(2.0, 1e-9));
    juce::String error;
    CHECK_FALSE(server.getInfo(temp.dir.getChildFile("nothing.avi"), &error).has_value());
    CHECK(error.isNotEmpty());

    // Playback: the first answer comes later; then exactly the frame asked for.
    CHECK_FALSE(server.requestFrame(file, 1.25, 0, 0).isValid());
    REQUIRE(server.waitUntilIdle(5000));
    CHECK(frameNumberOf(server.requestFrame(file, 1.25, 0, 0)) == 12);
    // A new time: the previous frame until the new one is decoded.
    CHECK(frameNumberOf(server.requestFrame(file, 0.31, 0, 0)) == 12);
    REQUIRE(server.waitUntilIdle(5000));
    CHECK(frameNumberOf(server.requestFrame(file, 0.31, 0, 0)) == 3);
    CHECK_FALSE(server.requestFrame(file, 2.5, 0, 0).isValid()); // past the end

    // Thumbnails: requested, decoded, cached at the requested height.
    CHECK_FALSE(server.getThumbnail(file, 0.5, 24).isValid());
    CHECK_FALSE(server.getThumbnail(file, 1.5, 24).isValid());
    REQUIRE(server.waitUntilIdle(5000));
    const auto thumbnail = server.getThumbnail(file, 0.5, 24);
    REQUIRE(thumbnail.isValid());
    CHECK(thumbnail.getHeight() == 24);
    CHECK(frameNumberOf(thumbnail) == 5);
    CHECK(frameNumberOf(server.getThumbnail(file, 1.5, 24)) == 15);

    // The message thread hears about new frames.
    test::pumpMessagesUntil([&] { return ready > 0; }, 2000);
    CHECK(ready > 0);
}

#if DOJO_HAS_FFMPEG
TEST_CASE("FFmpeg reads H.264 MP4 files (when the ffmpeg tool is there to make one)", "[video][ffmpeg]")
{
    test::TempFolder temp;
    const auto file = temp.dir.getChildFile("bars.mp4");
    juce::ChildProcess encoder;
    const juce::StringArray command{"ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc=size=320x240:rate=25:duration=2",
                                    "-c:v", "libx264", "-pix_fmt", "yuv420p", "-g", "12", file.getFullPathName()};
    if (!encoder.start(command) || !encoder.waitForProcessToFinish(60'000) || !file.existsAsFile())
        SKIP("no ffmpeg tool with libx264 to make a test file");

    juce::String error;
    auto source = video::openVideo(file, error);
    REQUIRE(source != nullptr);
    const auto& info = source->getInfo();
    CHECK(info.codec == "h264");
    CHECK(info.width == 320);
    CHECK(info.height == 240);
    CHECK_THAT(info.frameRate, WithinAbs(25.0, 1e-6));
    CHECK_THAT(info.durationSeconds, WithinAbs(2.0, 0.05));

    // testsrc changes every frame: different times give different pictures, the same time the same.
    const auto a = source->getFrame(0.5);
    const auto b = source->getFrame(1.52);
    const auto again = source->getFrame(0.5); // (a seek back)
    REQUIRE(a.isValid());
    REQUIRE(b.isValid());
    REQUIRE(again.isValid());
    const auto differs = [](const juce::Image& x, const juce::Image& y)
    {
        int different = 0;
        for (int i = 0; i < 64; ++i)
            different += x.getPixelAt(i * 5, 200) != y.getPixelAt(i * 5, 200) ? 1 : 0;
        return different;
    };
    CHECK(differs(a, b) > 0);
    CHECK(differs(a, again) == 0);
}
#endif

//==================================================================================================
// Video tracks

#include "session/Project.h"
#include "devices/DeviceRegistry.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/VideoWindow.h"

TEST_CASE("Video tracks hold video clips that edit like audio clips", "[video][session]")
{
    test::TempFolder temp;
    const auto file = writeTestVideo(temp.dir.getChildFile("picture.avi"), 20, 10.0);
    session::Session session;
    ui::EditorState editor;
    auto audio = session.addTrack(session::TrackKind::audio, "Dialogue");

    // Import: a new video track on top, the whole file at the requested time.
    juce::String error;
    auto c = ui::actions::importVideo(session, editor, file, 1.0, audio, error);
    REQUIRE(c.isValid());
    const auto t = c.getParent();
    CHECK(session::track::getKind(t) == session::TrackKind::video);
    CHECK(session.indexOf(t) == 0);
    CHECK(session::clip::isVideo(c));
    CHECK_THAT(session::clip::getStart(c), WithinAbs(1.0, 1e-9));
    CHECK_THAT(session::clip::getLength(c), WithinAbs(2.0, 1e-9));
    CHECK(editor.isClipSelected(session::clip::getId(c)));
    CHECK_FALSE(session::isRendered(session::TrackKind::video)); // no audio: never in the render graph

    // A second import goes onto the same video track; an unreadable file is refused with a reason.
    auto second = ui::actions::importVideo(session, editor, file, 5.0, {}, error);
    REQUIRE(second.isValid());
    CHECK(second.getParent() == t);
    CHECK_FALSE(ui::actions::importVideo(session, editor, temp.dir.getChildFile("none.mp4"), 0.0, {}, error).isValid());
    CHECK(error.isNotEmpty());

    // Video clips stay on video tracks.
    session.moveClip(c, audio, 2.0);
    CHECK(c.getParent() == t);
    CHECK_THAT(session::clip::getStart(c), WithinAbs(2.0, 1e-9));

    // Split: the right part shows the file from the split point on.
    auto right = session.splitClip(c, 2.5);
    REQUIRE(right.isValid());
    CHECK(session::clip::isVideo(right));
    CHECK_THAT(session::clip::getOffset(right), WithinAbs(0.5, 1e-9));

    // What the video window shows: the topmost active, unmuted video track's clip at that time.
    auto at = ui::actions::findVideoAt(session, 2.75);
    REQUIRE(at.clip == right);
    CHECK_THAT(at.fileSeconds, WithinAbs(0.75, 1e-9));
    CHECK_FALSE(ui::actions::findVideoAt(session, 4.6).clip.isValid()); // between clips
    session::track::setMuted(t, true, nullptr);
    CHECK_FALSE(ui::actions::findVideoAt(session, 2.75).clip.isValid());
    session::track::setMuted(t, false, nullptr);

    // Copy and paste: onto a video track.
    editor.setSelectedClips({session::clip::getId(right)});
    editor.selectTrack(session::track::getId(audio));
    REQUIRE(ui::actions::copySelectedClips(session, editor));
    REQUIRE(ui::actions::pasteClips(session, editor, 10.0));
    const auto pasted = session.findClip(*editor.getSelectedClips().begin());
    CHECK(session::clip::isVideo(pasted));
    CHECK(session::track::getKind(pasted.getParent()) == session::TrackKind::video);

    // Saved with the session, relative to it.
    const auto sessionFile = temp.dir.getChildFile("film.dojo");
    REQUIRE(session.save(sessionFile).wasOk());
    session::Session loaded;
    REQUIRE(loaded.load(sessionFile).wasOk());
    REQUIRE(loaded.getTrack(0).isValid());
    CHECK(session::track::getKind(loaded.getTrack(0)) == session::TrackKind::video);
    const auto loadedClip = loaded.getTrack(0).getChildWithName(session::ids::videoClip);
    REQUIRE(loadedClip.isValid());
    CHECK(session::clip::getFile(loadedClip) == file);

    // Track > New > Video Track.
    ui::actions::NewTracks row;
    row.type = ui::actions::NewTrackType::video;
    const auto created = ui::actions::addNewTracks(session, editor, {row});
    REQUIRE(created.size() == 1);
    CHECK(session::track::getKind(created[0]) == session::TrackKind::video);
    CHECK(ui::actions::getName(ui::actions::NewTrackType::video) == "Video Track");
}

TEST_CASE("The video window shows the frame at the playhead with its timecode", "[video][ui]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    const auto file = writeTestVideo(temp.dir.getChildFile("picture.avi"), 20, 10.0);
    session::Session session;
    ui::EditorState editor;
    juce::String error;
    REQUIRE(ui::actions::importVideo(session, editor, file, 1.0, {}, error).isValid());

    video::FrameServer frames;
    double playhead = 0.5;
    ui::VideoView view(session, frames, [&] { return playhead; });
    view.setSize(320, 180);

    // Before the clip: nothing to show.
    view.refresh();
    CHECK_FALSE(view.getCurrentFrame().isValid());
    CHECK(view.getClipName().isEmpty());

    // 1.35 s on the timeline = 0.35 s into the file = frame 3.
    playhead = 1.35;
    view.refresh();
    REQUIRE(frames.waitUntilIdle(5000));
    view.refresh();
    CHECK(frameNumberOf(view.getCurrentFrame()) == 3);
    CHECK(view.getTimecode() == "00:00:01:03");
    CHECK(view.getClipName() == "picture");

    // Playing on: the next frames follow.
    playhead = 2.05;
    view.refresh();
    REQUIRE(frames.waitUntilIdle(5000));
    view.refresh();
    CHECK(frameNumberOf(view.getCurrentFrame()) == 10);

    CHECK(ui::formatTimecode(3661.5, 25.0) == "01:01:01:12");
    CHECK(ui::formatTimecode(0.0, 30.0) == "00:00:00:00");

}

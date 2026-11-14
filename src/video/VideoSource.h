#pragma once

#include <juce_graphics/juce_graphics.h>

#include <memory>
#include <vector>

namespace dojo::video
{

/** What a video file holds. */
struct VideoInfo
{
    int width = 0;
    int height = 0;
    double frameRate = 25.0;       ///< frames per second
    double durationSeconds = 0.0;
    juce::String codec;            ///< e.g. "MJPEG", "h264"
    juce::String backend;          ///< the decoder that opened it

    [[nodiscard]] double getFrameDuration() const noexcept { return frameRate > 0.0 ? 1.0 / frameRate : 0.04; }
    /** The index of the frame showing at `seconds`. */
    [[nodiscard]] long long frameIndexAt(double seconds) const noexcept;
};

/**
    A video file opened for decoding. Frames come out as juce::Images (ARGB, opaque), optionally
    scaled down to fit a box (thumbnails). One thread at a time: a source keeps decoder state, so
    playing forward frame by frame is cheap and jumping elsewhere seeks.
*/
class VideoSource
{
public:
    virtual ~VideoSource() = default;

    [[nodiscard]] virtual const VideoInfo& getInfo() const noexcept = 0;

    /**
        The frame showing at `seconds` from the start of the file (the last one that starts at or
        before it), scaled to fit `maxWidth` x `maxHeight` keeping its aspect ratio (0 = full size).
        An invalid image if there is none (past the end, or a decoding error).
    */
    [[nodiscard]] virtual juce::Image getFrame(double seconds, int maxWidth = 0, int maxHeight = 0) = 0;
};

/**
    Opens a video with the first decoder that can read it: the built-in Motion-JPEG AVI reader
    (every platform, no dependencies), then FFmpeg (Linux, when built with it) or Media Foundation
    (Windows), which read MP4/MOV/MKV/WebM/AVI with H.264, HEVC, VP9, ProRes, ... Null and `error`
    set if none can.
*/
[[nodiscard]] std::unique_ptr<VideoSource> openVideo(const juce::File& file, juce::String& error);

/** File extensions The Dojo treats as video ("*.mp4;*.mov;..."). */
[[nodiscard]] juce::String getVideoFileWildcard();
[[nodiscard]] bool isVideoFile(const juce::File& file);

/** The decoders this build has, e.g. {"Motion-JPEG AVI", "FFmpeg 6.1"}. */
[[nodiscard]] juce::StringArray getDecoderNames();

/** `image` scaled to fit `maxWidth` x `maxHeight` (keeping the aspect ratio; 0 = no limit). */
[[nodiscard]] juce::Image fitWithin(const juce::Image& image, int maxWidth, int maxHeight);

/**
    Writes a Motion-JPEG AVI (no sound): every image becomes one frame at `frameRate`. Used for
    tests and the demo project. Returns false on a write error.
*/
bool writeMjpegAvi(const juce::File& file, const std::vector<juce::Image>& frames, double frameRate, float quality = 0.8f);

} // namespace dojo::video

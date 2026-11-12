#pragma once

#include "video/VideoSource.h"

namespace dojo::video
{

/**
    Common logic of decoders that read a stream forward (FFmpeg, Media Foundation): a request near
    the frame on screen decodes forward; going back, or far ahead, seeks first. The frame read one
    past the request is kept for the next call, so playback decodes every frame exactly once.
*/
class SequentialSource : public VideoSource
{
public:
    [[nodiscard]] juce::Image getFrame(double seconds, int maxWidth, int maxHeight) override;

protected:
    /** Positions the stream at (or before) `seconds`. */
    virtual void seekTo(double seconds) = 0;
    /** The next frame and its time (seconds from the start), or an invalid image at the end. */
    virtual juce::Image decodeNext(double& time) = 0;

    VideoInfo info_;

private:
    juce::Image current_;
    double currentTime_ = 0.0;
    juce::Image pending_;
    double pendingTime_ = 0.0;
};

#if DOJO_HAS_FFMPEG
std::unique_ptr<VideoSource> openWithFfmpeg(const juce::File& file, juce::String& error);
juce::String getFfmpegVersion();
#endif

#if JUCE_WINDOWS
std::unique_ptr<VideoSource> openWithMediaFoundation(const juce::File& file, juce::String& error);
#endif

} // namespace dojo::video

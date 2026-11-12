#include "video/VideoSource.h"

#include "video/MjpegAvi.h"
#include "video/PlatformDecoders.h"

#include <cmath>

namespace dojo::video
{

long long VideoInfo::frameIndexAt(double seconds) const noexcept
{
    // (a hair of tolerance: a frame's own start time maps to it, not to the one before)
    return static_cast<long long>(std::floor(seconds * frameRate + 1.0e-6));
}

juce::Image fitWithin(const juce::Image& image, int maxWidth, int maxHeight)
{
    if (!image.isValid() || ((maxWidth <= 0 || image.getWidth() <= maxWidth) && (maxHeight <= 0 || image.getHeight() <= maxHeight)))
        return image;

    double scale = 1.0;
    if (maxWidth > 0)
        scale = std::min(scale, static_cast<double>(maxWidth) / image.getWidth());
    if (maxHeight > 0)
        scale = std::min(scale, static_cast<double>(maxHeight) / image.getHeight());
    const int width = std::max(1, static_cast<int>(std::lround(image.getWidth() * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(image.getHeight() * scale)));
    return image.rescaled(width, height, juce::Graphics::mediumResamplingQuality);
}

juce::Image SequentialSource::getFrame(double seconds, int maxWidth, int maxHeight)
{
    if (seconds < 0.0 || (info_.durationSeconds > 0.0 && seconds >= info_.durationSeconds))
        return {};

    // The frame on screen already? (Playback asks for it many times.)
    const double frameDuration = info_.getFrameDuration();
    if (current_.isValid() && seconds >= currentTime_ - 1.0e-6 && seconds < currentTime_ + frameDuration - 1.0e-6)
        return fitWithin(current_, maxWidth, maxHeight);
    if (current_.isValid() && pending_.isValid() && seconds >= currentTime_ && seconds < pendingTime_ - 1.0e-6)
        return fitWithin(current_, maxWidth, maxHeight); // (a frame that lasts longer)

    // Going back, or far ahead: seek; otherwise decode forward from where we are.
    if (!current_.isValid() || seconds < currentTime_ - 1.0e-6 || seconds > currentTime_ + 2.0)
    {
        seekTo(seconds);
        current_ = {};
        pending_ = {};
    }

    juce::Image best = current_;
    double bestTime = currentTime_;
    if (pending_.isValid())
    {
        // The frame read ahead last time comes next.
        best = pending_;
        bestTime = pendingTime_;
        pending_ = {};
    }

    for (int guard = 0; guard < 100'000; ++guard)
    {
        if (best.isValid() && bestTime > seconds + 1.0e-6)
            break; // (only the first frame after a seek can be late: show it)
        double time = 0.0;
        auto image = decodeNext(time);
        if (!image.isValid())
            break; // end of the file
        if (time > seconds + 1.0e-6 && best.isValid())
        {
            pending_ = image; // the next frame, for the next call
            pendingTime_ = time;
            break;
        }
        best = image;
        bestTime = time;
    }

    if (!best.isValid())
        return {};
    current_ = best;
    currentTime_ = bestTime;
    return fitWithin(current_, maxWidth, maxHeight);
}

std::unique_ptr<VideoSource> openVideo(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile())
    {
        error = "Missing file: " + file.getFullPathName();
        return nullptr;
    }

    // The built-in reader first: if it opens the file and decodes its first frame, it is enough.
    {
        juce::String ignored;
        if (auto source = MjpegAviSource::open(file, ignored); source != nullptr && source->getFrame(0.0, 16, 16).isValid())
            return source;
    }

    juce::String platformError;
#if DOJO_HAS_FFMPEG
    if (auto source = openWithFfmpeg(file, platformError))
        return source;
#endif
#if JUCE_WINDOWS
    if (auto source = openWithMediaFoundation(file, platformError))
        return source;
#endif

    error = platformError.isNotEmpty() ? platformError
                                       : "The Dojo cannot decode " + file.getFileName() + " (this build reads Motion-JPEG AVI files"
#if !DOJO_HAS_FFMPEG && !JUCE_WINDOWS
                                             "; build with FFmpeg for MP4, MOV, MKV and WebM"
#endif
                                             ").";
    return nullptr;
}

juce::String getVideoFileWildcard()
{
    return "*.mp4;*.m4v;*.mov;*.avi;*.mkv;*.webm;*.mpg;*.mpeg;*.mxf;*.wmv";
}

bool isVideoFile(const juce::File& file)
{
    return file.hasFileExtension(getVideoFileWildcard().replace("*.", ""));
}

juce::StringArray getDecoderNames()
{
    juce::StringArray names{"Motion-JPEG AVI"};
#if DOJO_HAS_FFMPEG
    names.add("FFmpeg " + getFfmpegVersion());
#endif
#if JUCE_WINDOWS
    names.add("Media Foundation");
#endif
    return names;
}

} // namespace dojo::video

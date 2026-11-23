#include "video/PlatformDecoders.h"

#if JUCE_WINDOWS

#include "video/MediaFoundationMux.h"
#include "video/MediaFoundationReader.h"
#include "video/MuxBackends.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dojo::video
{
namespace
{
/** A video decoded by Windows Media Foundation (MP4/MOV/AVI/WMV with H.264, HEVC, ...). */
class MediaFoundationSource final : public SequentialSource
{
public:
    bool open(const juce::File& file, juce::String& error)
    {
        std::string message;
        if (!reader_.open(file.getFullPathName().toWideCharPointer(), message))
        {
            error = juce::String(message) + " (" + file.getFileName() + ")";
            return false;
        }
        info_.width = reader_.getWidth();
        info_.height = reader_.getHeight();
        info_.frameRate = reader_.getFrameRate();
        info_.durationSeconds = reader_.getDuration();
        info_.codec = juce::String(reader_.getCodec());
        info_.backend = "Media Foundation";
        return true;
    }

    [[nodiscard]] const VideoInfo& getInfo() const noexcept override { return info_; }

private:
    void seekTo(double seconds) override { reader_.seek(seconds); }

    juce::Image decodeNext(double& time) override
    {
        int width = 0;
        int height = 0;
        if (!reader_.readNext(pixels_, width, height, time) || width <= 0 || height <= 0)
            return {};

        juce::Image image(juce::Image::ARGB, width, height, false);
        juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < height; ++y)
            std::memcpy(bitmap.getLinePointer(y), pixels_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 4,
                        static_cast<std::size_t>(width) * 4);
        return image;
    }

    mf::Reader reader_;
    std::vector<std::uint8_t> pixels_;
};
} // namespace

std::unique_ptr<VideoSource> openWithMediaFoundation(const juce::File& file, juce::String& error)
{
    auto source = std::make_unique<MediaFoundationSource>();
    if (!source->open(file, error))
        return nullptr;
    return source;
}

namespace detail
{
juce::Result muxWithMediaFoundation(const MuxSettings& s, const std::function<bool(double)>& progress)
{
    if (!(s.to > s.from))
        return juce::Result::fail("Nothing to write.");
    MixReader mix(s.audio, s.audioOffset, s.to - s.from, s.speakers); // (WAVE order: L R C LFE Ls Rs for 5.1)
    if (!mix.isOpen())
        return juce::Result::fail("Could not read the mix.");
    const int channels = mix.getNumChannels();
    const int rate = static_cast<int>(std::lround(mix.getSampleRate()));
    if (channels != 1 && channels != 2 && channels != 6)
        return juce::Result::fail("Media Foundation's AAC holds mono, stereo or 5.1; this mix has " + juce::String(channels) + " channels.");
    if (rate != 44100 && rate != 48000)
        return juce::Result::fail("Media Foundation's AAC takes 44.1 or 48 kHz; this mix is " + juce::String(rate) + " Hz.");

    mf::MuxRequest request;
    request.video = s.video.getFullPathName().toWideCharPointer();
    request.destination = s.destination.getFullPathName().toWideCharPointer();
    request.from = s.from;
    request.to = s.to;
    request.sampleRate = rate;
    request.channels = channels;
    const int bytes = std::clamp(s.aacBitrate / 8, 12000, 24000);
    request.aacBytesPerSecond = static_cast<std::uint32_t>(bytes >= 24000 ? 24000 : bytes >= 20000 ? 20000 : bytes >= 16000 ? 16000 : 12000);

    juce::AudioBuffer<float> block(channels, 4096);
    std::string error;
    s.destination.deleteFile();
    const bool ok = mf::mux(
        request,
        [&](std::int16_t* interleaved, int frames)
        {
            const int got = mix.read(block, std::min(frames, block.getNumSamples()));
            for (int i = 0; i < got; ++i)
                for (int c = 0; c < channels; ++c)
                    interleaved[i * channels + c] = static_cast<std::int16_t>(std::lround(std::clamp(block.getSample(c, i), -1.0f, 1.0f) * 32767.0f));
            return got;
        },
        progress, error);
    return ok ? juce::Result::ok() : juce::Result::fail(juce::String(error));
}

double mediaFoundationKeyFrameAtOrBefore(const juce::File& video, double seconds)
{
    return mf::keyFrameAtOrBefore(video.getFullPathName().toWideCharPointer(), seconds);
}
} // namespace detail

} // namespace dojo::video

#endif

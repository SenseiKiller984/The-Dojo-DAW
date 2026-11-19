#include "video/PlatformDecoders.h"

#if JUCE_WINDOWS

#include "video/MediaFoundationReader.h"

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

} // namespace dojo::video

#endif

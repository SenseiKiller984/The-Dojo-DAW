#pragma once

#include "video/VideoSource.h"

namespace dojo::video
{

/**
    The built-in decoder: Motion-JPEG video in AVI files (what many cameras and screen recorders
    write), read straight from disk with JUCE's JPEG decoder. Every platform, no dependencies.
    Other AVI codecs are left to the platform decoders.
*/
class MjpegAviSource final : public VideoSource
{
public:
    /** Null without an error when the file is not a Motion-JPEG AVI (another decoder may read it). */
    static std::unique_ptr<MjpegAviSource> open(const juce::File& file, juce::String& error);

    [[nodiscard]] const VideoInfo& getInfo() const noexcept override { return info_; }
    [[nodiscard]] juce::Image getFrame(double seconds, int maxWidth, int maxHeight) override;

private:
    MjpegAviSource() = default;
    bool parse(juce::String& error);
    juce::Image decode(int index);

    struct Frame
    {
        juce::int64 offset = 0;
        int size = 0;
    };

    std::unique_ptr<juce::InputStream> stream_;
    VideoInfo info_;
    std::vector<Frame> frames_;
    int cachedIndex_ = -1;
    juce::Image cached_;
};

} // namespace dojo::video

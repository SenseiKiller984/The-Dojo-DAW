#pragma once

#include "video/VideoMux.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace dojo::video::detail
{

/**
    The mix, read block by block for a muxer: `offset` seconds of silence first, then the WAV, then
    silence again until `length` seconds have been read. Given the mix's speakers, its channels come
    out in the containers' order (by speakerBit: named speakers first, in bit order).
*/
class MixReader
{
public:
    MixReader(const juce::File& wav, double offsetSeconds, double lengthSeconds,
              const juce::Array<juce::AudioChannelSet::ChannelType>& speakers = {});
    [[nodiscard]] bool isOpen() const noexcept { return reader_ != nullptr; }
    [[nodiscard]] int getNumChannels() const noexcept;
    [[nodiscard]] double getSampleRate() const noexcept;
    [[nodiscard]] juce::int64 getTotalFrames() const noexcept { return total_; }
    [[nodiscard]] juce::int64 getFramesRead() const noexcept { return position_; }
    /** The channel mask of the channels as read (speakerBit bits), 0 unless every one is named. */
    [[nodiscard]] std::uint64_t getChannelMask() const noexcept { return mask_; }
    /** The WAVE part of it (WAVEFORMATEXTENSIBLE's dwChannelMask: the first channels; any after are
        unnamed there). */
    [[nodiscard]] std::uint32_t getWaveChannelMask() const noexcept { return static_cast<std::uint32_t>(mask_ & 0x3ffffu); }
    /** The next `count` frames (fewer at the end) into `destination` (getNumChannels() channels);
        how many. */
    int read(juce::AudioBuffer<float>& destination, int count);

private:
    std::unique_ptr<juce::AudioFormatReader> reader_;
    juce::int64 offset_ = 0;   ///< frames of silence first
    juce::int64 total_ = 0;    ///< frames in all
    juce::int64 position_ = 0;
    std::uint64_t mask_ = 0;
    std::vector<int> order_;   ///< channel k read = the WAV's channel order_[k] (empty: as they are)
    juce::AudioBuffer<float> scratch_;
};

#if DOJO_HAS_FFMPEG
juce::Result muxWithFfmpeg(const MuxSettings& settings, const std::function<bool(double)>& progress);
double ffmpegKeyFrameAtOrBefore(const juce::File& video, double seconds);
bool ffmpegCanMux(const juce::File& video);
bool decodeAudioWithFfmpeg(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate, juce::String& error);
#endif

#if JUCE_WINDOWS
juce::Result muxWithMediaFoundation(const MuxSettings& settings, const std::function<bool(double)>& progress);
double mediaFoundationKeyFrameAtOrBefore(const juce::File& video, double seconds);
#endif

} // namespace dojo::video::detail

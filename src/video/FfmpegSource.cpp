#include "video/PlatformDecoders.h"

#if DOJO_HAS_FFMPEG

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/version.h>
#include <libswscale/swscale.h>
}

#include <cmath>

namespace dojo::video
{
namespace
{
/**
    A video file decoded with FFmpeg (libavformat + libavcodec), converted to BGRA (JUCE's ARGB
    layout in memory) and scaled with libswscale.
*/
class FfmpegSource final : public SequentialSource
{
public:
    ~FfmpegSource() override
    {
        sws_freeContext(scaler_);
        av_packet_free(&packet_);
        av_frame_free(&frame_);
        avcodec_free_context(&decoder_);
        avformat_close_input(&format_);
    }

    bool open(const juce::File& file, juce::String& error)
    {
        const auto path = file.getFullPathName().toStdString();
        if (avformat_open_input(&format_, path.c_str(), nullptr, nullptr) < 0)
        {
            error = "FFmpeg could not open " + file.getFileName();
            return false;
        }
        if (avformat_find_stream_info(format_, nullptr) < 0)
        {
            error = "FFmpeg could not read the streams of " + file.getFileName();
            return false;
        }
        streamIndex_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (streamIndex_ < 0)
        {
            error = file.getFileName() + " has no video.";
            return false;
        }

        auto* stream = format_->streams[streamIndex_];
        const auto* codec = avcodec_find_decoder(stream->codecpar->codec_id);
        if (codec == nullptr)
        {
            error = "FFmpeg has no decoder for the video of " + file.getFileName();
            return false;
        }
        decoder_ = avcodec_alloc_context3(codec);
        if (decoder_ == nullptr || avcodec_parameters_to_context(decoder_, stream->codecpar) < 0)
            return false;
        decoder_->thread_count = 0; // automatic
        if (avcodec_open2(decoder_, codec, nullptr) < 0)
        {
            error = "FFmpeg could not open the " + juce::String(codec->name) + " decoder.";
            return false;
        }

        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        timeBase_ = av_q2d(stream->time_base);
        startTime_ = stream->start_time != AV_NOPTS_VALUE ? static_cast<double>(stream->start_time) * timeBase_ : 0.0;

        info_.width = decoder_->width;
        info_.height = decoder_->height;
        const auto rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
        info_.frameRate = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 25.0;
        info_.durationSeconds = stream->duration != AV_NOPTS_VALUE ? static_cast<double>(stream->duration) * timeBase_
                                : format_->duration != AV_NOPTS_VALUE ? static_cast<double>(format_->duration) / AV_TIME_BASE
                                                                       : 0.0;
        info_.codec = codec->name;
        info_.backend = "FFmpeg " + juce::String(LIBAVFORMAT_IDENT).fromFirstOccurrenceOf("Lavf", false, false);
        return info_.width > 0 && info_.height > 0;
    }

    [[nodiscard]] const VideoInfo& getInfo() const noexcept override { return info_; }

private:
    void seekTo(double seconds) override
    {
        const auto target = static_cast<std::int64_t>((seconds + startTime_) / timeBase_);
        av_seek_frame(format_, streamIndex_, target, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(decoder_);
        flushed_ = false;
    }

    juce::Image decodeNext(double& time) override
    {
        for (;;)
        {
            const int received = avcodec_receive_frame(decoder_, frame_);
            if (received == 0)
            {
                const auto pts = frame_->best_effort_timestamp != AV_NOPTS_VALUE ? frame_->best_effort_timestamp : frame_->pts;
                time = static_cast<double>(pts) * timeBase_ - startTime_;
                auto image = convert();
                av_frame_unref(frame_);
                return image;
            }
            if (received == AVERROR_EOF)
                return {};
            if (received != AVERROR(EAGAIN))
                return {};

            // Feed the decoder.
            if (flushed_)
                return {};
            if (av_read_frame(format_, packet_) < 0)
            {
                avcodec_send_packet(decoder_, nullptr); // drain
                flushed_ = true;
                continue;
            }
            if (packet_->stream_index == streamIndex_)
                avcodec_send_packet(decoder_, packet_);
            av_packet_unref(packet_);
        }
    }

    juce::Image convert()
    {
        const int width = frame_->width;
        const int height = frame_->height;
        if (width <= 0 || height <= 0)
            return {};
        scaler_ = sws_getCachedContext(scaler_, width, height, static_cast<AVPixelFormat>(frame_->format), width, height, AV_PIX_FMT_BGRA,
                                       SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (scaler_ == nullptr)
            return {};

        juce::Image image(juce::Image::ARGB, width, height, false);
        {
            juce::Image::BitmapData bitmap(image, juce::Image::BitmapData::writeOnly);
            std::uint8_t* destination[4] = {bitmap.data, nullptr, nullptr, nullptr};
            int strides[4] = {bitmap.lineStride, 0, 0, 0};
            sws_scale(scaler_, frame_->data, frame_->linesize, 0, height, destination, strides);
        }
        return image;
    }

    AVFormatContext* format_ = nullptr;
    AVCodecContext* decoder_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* packet_ = nullptr;
    SwsContext* scaler_ = nullptr;
    int streamIndex_ = -1;
    double timeBase_ = 1.0;
    double startTime_ = 0.0;
    bool flushed_ = false;
};
} // namespace

std::unique_ptr<VideoSource> openWithFfmpeg(const juce::File& file, juce::String& error)
{
    static const bool quiet = []
    {
        av_log_set_level(AV_LOG_ERROR); // (no chatter about pixel formats on the console)
        return true;
    }();
    (void)quiet;
    auto source = std::make_unique<FfmpegSource>();
    if (!source->open(file, error))
        return nullptr;
    return source;
}

juce::String getFfmpegVersion()
{
    return juce::String(av_version_info());
}

} // namespace dojo::video

#endif

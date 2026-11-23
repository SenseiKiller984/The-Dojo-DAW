#include "video/MuxBackends.h"
#include "video/PlatformDecoders.h"

#if DOJO_HAS_FFMPEG

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
}

#include <cmath>
#include <cstring>
#include <vector>

namespace dojo::video::detail
{
namespace
{
/** FFmpeg's console chatter (the muxer's second pass, the encoder's statistics) off: errors only. */
void quietLogs()
{
    static const bool quiet = []
    {
        av_log_set_level(AV_LOG_ERROR);
        return true;
    }();
    (void)quiet;
}

juce::String describe(int code)
{
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, text, sizeof(text));
    return juce::String(text);
}

/** A movie opened for reading, its best video stream found. */
struct Input
{
    AVFormatContext* format = nullptr;
    int stream = -1;

    Input() = default;
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    ~Input() { avformat_close_input(&format); }

    bool open(const juce::File& file)
    {
        quietLogs();
        const auto path = file.getFullPathName().toStdString();
        if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0)
            return false;
        stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        return stream >= 0;
    }

    [[nodiscard]] AVStream* video() const { return format->streams[stream]; }
    [[nodiscard]] std::int64_t startTime() const { return video()->start_time != AV_NOPTS_VALUE ? video()->start_time : 0; }
    /** A time (seconds from the stream's start) in the stream's time base. */
    [[nodiscard]] std::int64_t toStream(double seconds) const
    {
        return startTime() + av_rescale_q(static_cast<std::int64_t>(std::llround(seconds * AV_TIME_BASE)), AV_TIME_BASE_Q, video()->time_base);
    }
};

juce::String containerExtension(const juce::File& video)
{
    auto extension = video.getFileExtension().toLowerCase();
    if (extension == ".webm" || extension.isEmpty())
        extension = ".mkv";
    return extension;
}

/** Owns everything a mux allocates (FFmpeg's C objects). */
struct Output
{
    AVFormatContext* format = nullptr;
    AVCodecContext* encoder = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* packet = nullptr;
    AVPacket* audioPacket = nullptr;

    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output()
    {
        av_packet_free(&packet);
        av_packet_free(&audioPacket);
        av_frame_free(&frame);
        avcodec_free_context(&encoder);
        if (format != nullptr)
        {
            if ((format->oformat->flags & AVFMT_NOFILE) == 0)
                avio_closep(&format->pb);
            avformat_free_context(format);
        }
    }
};
} // namespace

bool ffmpegCanMux(const juce::File& video)
{
    Input in;
    if (!in.open(video))
        return false;
    const auto* container = av_guess_format(nullptr, ("x" + containerExtension(video)).toRawUTF8(), nullptr);
    return container != nullptr && avformat_query_codec(container, in.video()->codecpar->codec_id, FF_COMPLIANCE_NORMAL) == 1;
}

double ffmpegKeyFrameAtOrBefore(const juce::File& video, double seconds)
{
    Input in;
    if (!in.open(video) || seconds <= 0.0)
        return 0.0;
    double found = -1.0;
    if (av_seek_frame(in.format, in.stream, in.toStream(seconds), AVSEEK_FLAG_BACKWARD) >= 0)
    {
        AVPacket* packet = av_packet_alloc();
        while (av_read_frame(in.format, packet) >= 0)
        {
            const bool key = packet->stream_index == in.stream && (packet->flags & AV_PKT_FLAG_KEY) != 0;
            const auto time = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
            av_packet_unref(packet);
            if (key && time != AV_NOPTS_VALUE)
            {
                found = static_cast<double>(time - in.startTime()) * av_q2d(in.video()->time_base);
                break;
            }
        }
        av_packet_free(&packet);
    }
    // (an index that landed past the time: from the start)
    return found >= 0.0 && found <= seconds + 1.0e-6 ? found : 0.0;
}

juce::Result muxWithFfmpeg(const MuxSettings& s, const std::function<bool(double)>& progress)
{
    Input in;
    if (!in.open(s.video))
        return juce::Result::fail("FFmpeg could not open " + s.video.getFileName());
    auto* source = in.video();
    const double length = s.to > 0.0 ? s.to - s.from
                                     : (in.format->duration > 0 ? static_cast<double>(in.format->duration) / AV_TIME_BASE - s.from : 0.0);
    if (!(length > 0.0))
        return juce::Result::fail("There is no picture there to copy.");

    MixReader mix(s.audio, s.audioOffset, length, s.speakers);
    if (!mix.isOpen())
        return juce::Result::fail("Could not read the mix.");
    const int channels = mix.getNumChannels();
    const int rate = static_cast<int>(std::lround(mix.getSampleRate()));
    const bool aac = s.codec == MuxAudioCodec::aac;
    if (aac && channels > 8)
        return juce::Result::fail("AAC holds up to 8 channels (7.1); this mix has " + juce::String(channels) + ": choose PCM.");

    Output out;
    const auto path = s.destination.getFullPathName().toStdString();
    s.destination.deleteFile();
    if (avformat_alloc_output_context2(&out.format, nullptr, nullptr, path.c_str()) < 0 || out.format == nullptr)
        return juce::Result::fail("FFmpeg cannot write " + s.destination.getFileName());

    // The picture: its packets copied as they are.
    AVStream* picture = avformat_new_stream(out.format, nullptr);
    if (picture == nullptr || avcodec_parameters_copy(picture->codecpar, source->codecpar) < 0)
        return juce::Result::fail("FFmpeg could not set up the picture.");
    // The movie's own tag for the codec where this container knows it (an Apple 'hvc1' stays
    // 'hvc1', which QuickTime needs), else the container's own (as the ffmpeg tool decides).
    unsigned int containerTag = 0;
    const auto* tags = out.format->oformat->codec_tag;
    const bool keepTag = tags == nullptr || av_codec_get_id(tags, source->codecpar->codec_tag) == source->codecpar->codec_id
                         || av_codec_get_tag2(tags, source->codecpar->codec_id, &containerTag) == 0;
    picture->codecpar->codec_tag = keepTag ? source->codecpar->codec_tag : 0;
    picture->time_base = source->time_base;
    picture->avg_frame_rate = source->avg_frame_rate;
    picture->r_frame_rate = source->r_frame_rate;
    picture->sample_aspect_ratio = source->sample_aspect_ratio;

    // The sound: the mix, encoded. PCM is 24-bit, but 32-bit in an MP4: FFmpeg 6.1 writes 24-bit
    // 'ipcm' that it then reads as 32-bit.
    const auto destinationExtension = s.destination.getFileExtension().toLowerCase();
    const bool mp4 = destinationExtension == ".mp4" || destinationExtension == ".m4v";
    const auto* codec = avcodec_find_encoder(aac ? AV_CODEC_ID_AAC : mp4 ? AV_CODEC_ID_PCM_S32LE : AV_CODEC_ID_PCM_S24LE);
    if (codec == nullptr || (out.encoder = avcodec_alloc_context3(codec)) == nullptr)
        return juce::Result::fail(juce::String(aac ? "AAC" : "PCM") + " is not in this FFmpeg.");
    auto* encoder = out.encoder;
    encoder->sample_rate = rate;
    if (const auto mask = mix.getChannelMask(); mask != 0)
        av_channel_layout_from_mask(&encoder->ch_layout, mask); // (the mix's speakers, in FFmpeg's own order)
    else
        av_channel_layout_default(&encoder->ch_layout, channels);
    encoder->sample_fmt = aac ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_S32;
    encoder->bit_rate = aac ? s.aacBitrate : 0;
    encoder->time_base = AVRational{1, rate};
    if ((out.format->oformat->flags & AVFMT_GLOBALHEADER) != 0)
        encoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (const int r = avcodec_open2(encoder, codec, nullptr); r < 0)
    {
        char layout[128] = {};
        av_channel_layout_describe(&encoder->ch_layout, layout, sizeof(layout));
        return juce::Result::fail("The " + juce::String(aac ? "AAC" : "PCM") + " encoder does not take this mix (" + juce::String(layout) + ", "
                                  + juce::String(channels) + " channels at " + juce::String(rate) + " Hz): " + describe(r));
    }
    AVStream* sound = avformat_new_stream(out.format, nullptr);
    if (sound == nullptr || avcodec_parameters_from_context(sound->codecpar, encoder) < 0)
        return juce::Result::fail("FFmpeg could not set up the sound.");
    sound->time_base = encoder->time_base;

    if ((out.format->oformat->flags & AVFMT_NOFILE) == 0 && avio_open(&out.format->pb, path.c_str(), AVIO_FLAG_WRITE) < 0)
        return juce::Result::fail("Could not write " + s.destination.getFullPathName());
    AVDictionary* options = nullptr;
    if (mp4 || destinationExtension == ".mov")
        av_dict_set(&options, "movflags", "+faststart", 0); // (the index first: it plays while it loads)
    const int header = avformat_write_header(out.format, &options);
    av_dict_free(&options);
    if (header < 0)
        return juce::Result::fail("FFmpeg could not start " + s.destination.getFileName() + ": " + describe(header));

    out.frame = av_frame_alloc();
    out.packet = av_packet_alloc();
    out.audioPacket = av_packet_alloc();
    const int frameSize = encoder->frame_size > 0 ? encoder->frame_size : 1024;
    out.frame->nb_samples = frameSize;
    out.frame->format = encoder->sample_fmt;
    out.frame->sample_rate = rate;
    av_channel_layout_copy(&out.frame->ch_layout, &encoder->ch_layout);
    if (av_frame_get_buffer(out.frame, 0) < 0)
        return juce::Result::fail("Out of memory.");

    const auto drain = [&]() -> bool
    {
        for (;;)
        {
            const int r = avcodec_receive_packet(encoder, out.audioPacket);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                return true;
            if (r < 0)
                return false;
            av_packet_rescale_ts(out.audioPacket, encoder->time_base, sound->time_base);
            out.audioPacket->stream_index = sound->index;
            if (av_interleaved_write_frame(out.format, out.audioPacket) < 0)
                return false;
        }
    };
    juce::AudioBuffer<float> block(channels, frameSize);
    std::int64_t written = 0;
    const auto soundUntil = [&](double seconds) -> bool
    {
        while (mix.getFramesRead() < mix.getTotalFrames() && static_cast<double>(written) / rate < seconds)
        {
            const int got = mix.read(block, frameSize);
            if (got <= 0 || av_frame_make_writable(out.frame) < 0)
                return got <= 0;
            out.frame->nb_samples = got;
            if (aac)
            {
                for (int c = 0; c < channels; ++c)
                    std::memcpy(out.frame->data[c], block.getReadPointer(c), static_cast<std::size_t>(got) * sizeof(float));
            }
            else
            {
                auto* interleaved = reinterpret_cast<std::int32_t*>(out.frame->data[0]);
                for (int i = 0; i < got; ++i)
                    for (int c = 0; c < channels; ++c)
                        interleaved[i * channels + c] = static_cast<std::int32_t>(
                            std::llround(static_cast<double>(std::clamp(block.getSample(c, i), -1.0f, 1.0f)) * 2147483647.0));
            }
            out.frame->pts = written;
            written += got;
            if (avcodec_send_frame(encoder, out.frame) < 0 || !drain())
                return false;
        }
        return true;
    };

    // From the key frame at `from` on: every picture shown before `to`, and the ones they need, with
    // the sound kept a little ahead. With B-frames a picture shown after `to` can come first in
    // decoding order (the ones before it are predicted from it): it is held back, and written only
    // if a picture shown before `to` follows it. Once a packet is decoded at or after `to`, every
    // later one is shown after it too.
    const auto from = in.toStream(s.from);
    const auto end = s.to > 0.0 ? in.toStream(s.to) : INT64_MAX;
    av_seek_frame(in.format, in.stream, from, AVSEEK_FLAG_BACKWARD);
    bool ok = true;
    bool cancelled = false;
    std::vector<AVPacket*> held;
    const auto writePicture = [&](AVPacket* p) -> bool
    {
        const auto when = p->dts != AV_NOPTS_VALUE ? p->dts : p->pts;
        const double at = when != AV_NOPTS_VALUE ? static_cast<double>(when - from) * av_q2d(source->time_base) : 0.0;
        if (!soundUntil(at + 0.25))
            return false;
        if (p->pts != AV_NOPTS_VALUE)
            p->pts -= from;
        if (p->dts != AV_NOPTS_VALUE)
            p->dts -= from;
        av_packet_rescale_ts(p, source->time_base, picture->time_base);
        p->stream_index = picture->index;
        p->pos = -1;
        return av_interleaved_write_frame(out.format, p) >= 0; // (takes the packet's data)
    };
    auto* packet = out.packet;
    while (ok && av_read_frame(in.format, packet) >= 0)
    {
        if (packet->stream_index != in.stream)
        {
            av_packet_unref(packet);
            continue;
        }
        const auto decoded = packet->dts != AV_NOPTS_VALUE ? packet->dts : packet->pts;
        const auto shown = packet->pts != AV_NOPTS_VALUE ? packet->pts : packet->dts;
        if (decoded != AV_NOPTS_VALUE && decoded >= end)
        {
            av_packet_unref(packet);
            break;
        }
        if (shown != AV_NOPTS_VALUE && shown >= end)
        {
            held.push_back(av_packet_clone(packet));
            av_packet_unref(packet);
            continue;
        }
        for (auto*& h : held)
        {
            ok = ok && h != nullptr && writePicture(h);
            av_packet_free(&h);
        }
        held.clear();
        const double at = static_cast<double>(shown != AV_NOPTS_VALUE ? shown - from : 0) * av_q2d(source->time_base);
        ok = ok && writePicture(packet);
        av_packet_unref(packet);
        if (progress && !progress(std::clamp(at / length, 0.0, 1.0)))
        {
            cancelled = true;
            break;
        }
    }
    for (auto*& h : held)
        av_packet_free(&h);
    if (ok && !cancelled)
        ok = soundUntil(1.0e12) && avcodec_send_frame(encoder, nullptr) >= 0 && drain(); // (the rest of the mix, then the encoder's tail)
    const bool finished = av_write_trailer(out.format) >= 0;
    if (cancelled)
        return juce::Result::fail("Export cancelled.");
    if (!ok || !finished)
        return juce::Result::fail("Writing " + s.destination.getFileName() + " failed.");
    return juce::Result::ok();
}

bool decodeAudioWithFfmpeg(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate, juce::String& error)
{
    quietLogs();
    AVFormatContext* format = nullptr;
    const auto path = file.getFullPathName().toStdString();
    if (avformat_open_input(&format, path.c_str(), nullptr, nullptr) < 0 || avformat_find_stream_info(format, nullptr) < 0)
    {
        avformat_close_input(&format);
        error = "FFmpeg could not open " + file.getFileName();
        return false;
    }
    const AVCodec* codec = nullptr;
    const int stream = av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    AVCodecContext* decoder = stream >= 0 && codec != nullptr ? avcodec_alloc_context3(codec) : nullptr;
    if (decoder != nullptr)
        decoder->pkt_timebase = format->streams[stream]->time_base; // (so a priming skip moves the frames' times too)
    if (decoder == nullptr || avcodec_parameters_to_context(decoder, format->streams[stream]->codecpar) < 0 || avcodec_open2(decoder, codec, nullptr) < 0)
    {
        avcodec_free_context(&decoder);
        avformat_close_input(&format);
        error = file.getFileName() + " has no sound FFmpeg can decode.";
        return false;
    }

    const int channels = decoder->ch_layout.nb_channels;
    std::vector<std::vector<float>> samples(static_cast<std::size_t>(channels));
    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto timeBase = format->streams[stream]->time_base;
    std::int64_t lead = 0; // (samples before the file's time zero: negative; silence first: positive)
    bool first = true;
    const auto take = [&]
    {
        while (avcodec_receive_frame(decoder, frame) >= 0)
        {
            if (first)
            {
                // Where the sound starts against the picture: an encoder's priming before zero
                // (Matroska keeps it, with a negative time), or a late start.
                first = false;
                const auto when = frame->best_effort_timestamp;
                if (when != AV_NOPTS_VALUE)
                    lead = av_rescale_q(when, timeBase, AVRational{1, frame->sample_rate > 0 ? frame->sample_rate : decoder->sample_rate});
            }
            const auto fmt = static_cast<AVSampleFormat>(frame->format);
            const bool planar = av_sample_fmt_is_planar(fmt) != 0;
            for (int i = 0; i < frame->nb_samples; ++i)
                for (int c = 0; c < channels; ++c)
                {
                    const auto* plane = frame->extended_data[planar ? c : 0];
                    const int at = planar ? i : i * channels + c;
                    float v = 0.0f;
                    switch (av_get_packed_sample_fmt(fmt))
                    {
                    case AV_SAMPLE_FMT_FLT: v = reinterpret_cast<const float*>(plane)[at]; break;
                    case AV_SAMPLE_FMT_DBL: v = static_cast<float>(reinterpret_cast<const double*>(plane)[at]); break;
                    case AV_SAMPLE_FMT_S16: v = static_cast<float>(reinterpret_cast<const std::int16_t*>(plane)[at]) / 32768.0f; break;
                    case AV_SAMPLE_FMT_S32: v = static_cast<float>(static_cast<double>(reinterpret_cast<const std::int32_t*>(plane)[at]) / 2147483648.0); break;
                    case AV_SAMPLE_FMT_U8: v = (static_cast<float>(plane[at]) - 128.0f) / 128.0f; break;
                    case AV_SAMPLE_FMT_NONE:
                    case AV_SAMPLE_FMT_S64:
                    case AV_SAMPLE_FMT_U8P:
                    case AV_SAMPLE_FMT_S16P:
                    case AV_SAMPLE_FMT_S32P:
                    case AV_SAMPLE_FMT_FLTP:
                    case AV_SAMPLE_FMT_DBLP:
                    case AV_SAMPLE_FMT_S64P:
                    case AV_SAMPLE_FMT_NB: break;
                    }
                    samples[static_cast<std::size_t>(c)].push_back(v);
                }
            av_frame_unref(frame);
        }
    };
    while (av_read_frame(format, packet) >= 0)
    {
        if (packet->stream_index == stream && avcodec_send_packet(decoder, packet) >= 0)
            take();
        av_packet_unref(packet);
    }
    avcodec_send_packet(decoder, nullptr);
    take();
    sampleRate = decoder->sample_rate;
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&format);

    const auto decodedLength = static_cast<std::int64_t>(samples.empty() ? 0 : samples.front().size());
    const auto skip = std::min(decodedLength, std::max<std::int64_t>(0, -lead));
    const auto silence = std::max<std::int64_t>(0, lead);
    audio.setSize(channels, static_cast<int>(silence + decodedLength - skip));
    audio.clear();
    for (int c = 0; c < channels; ++c)
    {
        const auto& from = samples[static_cast<std::size_t>(c)];
        std::copy(from.begin() + static_cast<std::ptrdiff_t>(skip), from.end(), audio.getWritePointer(c) + silence);
    }
    return true;
}

} // namespace dojo::video::detail

#endif // DOJO_HAS_FFMPEG

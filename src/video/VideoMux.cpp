#include "video/VideoMux.h"

#include "video/MuxBackends.h"
#include "video/PlatformDecoders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace dojo::video
{
namespace
{
constexpr std::uint32_t fourcc(const char (&id)[5]) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(id[0])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(id[1])) << 8)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(id[2])) << 16) | (static_cast<std::uint32_t>(static_cast<unsigned char>(id[3])) << 24);
}

std::uint32_t readU32(juce::InputStream& in) { return static_cast<std::uint32_t>(in.readInt()); }

bool isMotionJpeg(std::uint32_t handler)
{
    for (const auto id : {fourcc("MJPG"), fourcc("mjpg"), fourcc("AVRn"), fourcc("dmb1"), fourcc("JPEG"), fourcc("jpeg")})
        if (handler == id)
            return true;
    return false;
}

/** An AVI file's streams and where each one's chunks are. */
struct Avi
{
    struct Stream
    {
        std::uint32_t type = 0;    ///< "vids", "auds"
        std::uint32_t handler = 0; ///< the codec ("MJPG"; 0 for PCM sound)
        std::uint32_t scale = 1, rate = 1;
        juce::MemoryBlock format; ///< its strf (BITMAPINFOHEADER, WAVEFORMATEX)
        std::vector<std::pair<juce::int64, std::uint32_t>> chunks; ///< (offset of the data, size)
    };
    int width = 0, height = 0;
    std::vector<Stream> streams;

    [[nodiscard]] const Stream* find(std::uint32_t type) const
    {
        for (const auto& s : streams)
            if (s.type == type)
                return &s;
        return nullptr;
    }
};

bool parseAvi(juce::InputStream& in, Avi& avi)
{
    const auto total = in.getTotalLength();
    if (total < 12 || !in.setPosition(0) || readU32(in) != fourcc("RIFF"))
        return false;
    readU32(in);
    if (readU32(in) != fourcc("AVI "))
        return false;

    const std::function<void(juce::int64, juce::int64, std::uint32_t, int)> walk = [&](juce::int64 begin, juce::int64 end, std::uint32_t list, int depth)
    {
        juce::int64 at = begin;
        while (at + 8 <= end && depth < 8)
        {
            in.setPosition(at);
            const auto id = readU32(in);
            auto size = static_cast<juce::int64>(readU32(in));
            const auto data = at + 8;
            size = std::min(size, total - data);
            if (id == fourcc("LIST") || id == fourcc("RIFF"))
                walk(data + 4, data + size, readU32(in), depth + 1);
            else if (id == fourcc("avih") && size >= 40)
            {
                in.setPosition(data + 32);
                avi.width = static_cast<int>(readU32(in));
                avi.height = static_cast<int>(readU32(in));
            }
            else if (id == fourcc("strh") && list == fourcc("strl") && size >= 28)
            {
                Avi::Stream s;
                s.type = readU32(in);
                s.handler = readU32(in);
                in.setPosition(data + 20);
                s.scale = std::max(1u, readU32(in));
                s.rate = std::max(1u, readU32(in));
                avi.streams.push_back(std::move(s));
            }
            else if (id == fourcc("strf") && list == fourcc("strl") && !avi.streams.empty() && size > 0 && size < (1 << 16))
            {
                avi.streams.back().format.setSize(static_cast<std::size_t>(size));
                in.read(avi.streams.back().format.getData(), static_cast<int>(size));
            }
            else if (list == fourcc("movi") || list == fourcc("rec "))
            {
                // "NNxx": the stream's number in two digits, then the kind of chunk.
                const auto c0 = static_cast<char>(id & 0xff);
                const auto c1 = static_cast<char>((id >> 8) & 0xff);
                if (c0 >= '0' && c0 <= '9' && c1 >= '0' && c1 <= '9')
                {
                    const auto stream = static_cast<std::size_t>((c0 - '0') * 10 + (c1 - '0'));
                    if (stream < avi.streams.size())
                        avi.streams[stream].chunks.emplace_back(data, static_cast<std::uint32_t>(size));
                }
            }
            at = data + size + (size & 1);
        }
    };
    walk(12, total, fourcc("AVI "), 0);
    return !avi.streams.empty();
}

/** A Motion-JPEG AVI (every frame a key frame): the built-in muxer copies it. */
bool isBuiltInAvi(const juce::File& video, Avi* parsed = nullptr)
{
    juce::FileInputStream in(video);
    Avi avi;
    if (!in.openedOk() || !parseAvi(in, avi))
        return false;
    const auto* picture = avi.find(fourcc("vids"));
    std::uint32_t compression = 0;
    if (picture != nullptr && picture->format.getSize() >= 20)
        compression = static_cast<const std::uint32_t*>(picture->format.getData())[4]; // (biCompression)
    if (picture == nullptr || picture->chunks.empty() || !(isMotionJpeg(picture->handler) || isMotionJpeg(compression)))
        return false;
    if (parsed != nullptr)
        *parsed = std::move(avi);
    return true;
}

juce::Result muxBuiltInAvi(const MuxSettings& s, const std::function<bool(double)>& progress)
{
    Avi avi;
    if (!isBuiltInAvi(s.video, &avi))
        return juce::Result::fail(s.video.getFileName() + " is not a Motion-JPEG AVI.");
    juce::FileInputStream source(s.video);
    const auto& picture = *avi.find(fourcc("vids"));
    const double fps = static_cast<double>(picture.rate) / static_cast<double>(picture.scale);
    const auto frames = static_cast<int>(picture.chunks.size());
    const int first = std::clamp(static_cast<int>(std::floor(s.from * fps + 1.0e-6)), 0, frames);
    const int last = s.to > 0.0 ? std::clamp(static_cast<int>(std::ceil(s.to * fps - 1.0e-6)), first, frames) : frames;
    if (last <= first)
        return juce::Result::fail("There is no picture there to copy.");

    const double seconds = (last - first) / fps;
    detail::MixReader mix(s.audio, s.audioOffset, seconds, s.speakers);
    if (!mix.isOpen())
        return juce::Result::fail("Could not read the mix.");
    const int channels = mix.getNumChannels();
    const auto rate = static_cast<std::uint32_t>(std::lround(mix.getSampleRate()));
    const std::uint32_t blockAlign = static_cast<std::uint32_t>(channels) * 3;
    juce::int64 bytes = 0;
    for (int k = first; k < last; ++k)
        bytes += picture.chunks[static_cast<std::size_t>(k)].second + 8;
    bytes += mix.getTotalFrames() * blockAlign + (last - first) * 8;
    if (bytes > 0x7f000000)
        return juce::Result::fail("The AVI would be over 2 GB: export a shorter stretch.");

    s.destination.deleteFile();
    juce::FileOutputStream out(s.destination, 1 << 18);
    if (out.failedToOpen())
        return juce::Result::fail("Could not write " + s.destination.getFullPathName());
    const auto u32 = [&](std::uint32_t v) { out.writeInt(static_cast<int>(v)); };
    const auto u16 = [&](std::uint16_t v) { out.writeShort(static_cast<short>(v)); };
    const auto id = [&](const char (&text)[5]) { u32(fourcc(text)); };
    std::vector<std::pair<juce::int64, juce::int64>> patches; // (where, from): the size of what follows `where + 4` up to now
    const auto open = [&](const char (&text)[5])
    {
        id(text);
        const auto at = out.getPosition();
        u32(0);
        return at;
    };
    const auto close = [&](juce::int64 at) { patches.emplace_back(at, out.getPosition() - at - 4); };

    const auto riff = open("RIFF");
    id("AVI ");
    const auto hdrl = open("LIST");
    id("hdrl");
    id("avih");
    u32(56);
    u32(static_cast<std::uint32_t>(std::lround(1.0e6 / fps)));
    u32(static_cast<std::uint32_t>(std::min<juce::int64>(0x7fffffff, static_cast<juce::int64>(static_cast<double>(bytes) / std::max(0.001, seconds)))));
    u32(0);
    u32(0x10 | 0x100); // AVIF_HASINDEX, AVIF_ISINTERLEAVED
    u32(static_cast<std::uint32_t>(last - first));
    u32(0);
    u32(2); // streams
    u32(1 << 20);
    u32(static_cast<std::uint32_t>(avi.width));
    u32(static_cast<std::uint32_t>(avi.height));
    for (int i = 0; i < 4; ++i)
        u32(0);

    // The picture, as it was.
    const auto videoList = open("LIST");
    id("strl");
    id("strh");
    u32(56);
    id("vids");
    u32(picture.handler);
    u32(0);
    u16(0);
    u16(0);
    u32(0);
    u32(picture.scale);
    u32(picture.rate);
    u32(0);
    u32(static_cast<std::uint32_t>(last - first));
    u32(1 << 20);
    u32(0xffffffffu);
    u32(0);
    u16(0);
    u16(0);
    u16(static_cast<std::uint16_t>(avi.width));
    u16(static_cast<std::uint16_t>(avi.height));
    id("strf");
    u32(static_cast<std::uint32_t>(picture.format.getSize()));
    out.write(picture.format.getData(), picture.format.getSize());
    if ((picture.format.getSize() & 1) != 0)
        out.writeByte(0);
    close(videoList);

    // The sound: PCM, 24-bit.
    const auto audioList = open("LIST");
    id("strl");
    id("strh");
    u32(56);
    id("auds");
    u32(0);
    u32(0);
    u16(0);
    u16(0);
    u32(0);
    u32(blockAlign);
    u32(rate * blockAlign);
    u32(0);
    u32(static_cast<std::uint32_t>(mix.getTotalFrames()));
    u32(rate * blockAlign);
    u32(0xffffffffu);
    u32(blockAlign);
    for (int i = 0; i < 4; ++i)
        u16(0);
    id("strf");
    const bool extensible = channels > 2;
    u32(extensible ? 40 : 18);
    u16(extensible ? 0xfffe : 1);
    u16(static_cast<std::uint16_t>(channels));
    u32(rate);
    u32(rate * blockAlign);
    u16(static_cast<std::uint16_t>(blockAlign));
    u16(24);
    if (extensible)
    {
        u16(22);
        u16(24);  // valid bits
        u32(mix.getWaveChannelMask()); // (0: in order, unnamed)
        u32(1);   // KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00aa00389b71}
        u16(0);
        u16(0x10);
        for (const int b : {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
            out.writeByte(static_cast<char>(b));
    }
    else
        u16(0);
    close(audioList);
    close(hdrl);

    // The frames with the sound for each, interleaved.
    const auto movi = open("LIST");
    const auto moviStart = out.getPosition();
    id("movi");
    struct Entry
    {
        std::uint32_t id, offset, size;
    };
    std::vector<Entry> index;
    juce::AudioBuffer<float> block(channels, 1);
    juce::HeapBlock<char> data;
    std::vector<char> pcm;
    for (int k = first; k < last; ++k)
    {
        const auto [offset, size] = picture.chunks[static_cast<std::size_t>(k)];
        data.realloc(std::max<std::size_t>(1, size));
        source.setPosition(offset);
        if (source.read(data.get(), static_cast<int>(size)) != static_cast<int>(size))
            return juce::Result::fail("Could not read the movie's frame " + juce::String(k + 1) + ".");
        index.push_back({fourcc("00dc"), static_cast<std::uint32_t>(out.getPosition() - moviStart), size});
        id("00dc");
        u32(size);
        out.write(data.get(), size);
        if ((size & 1) != 0)
            out.writeByte(0);

        // The sound under this frame.
        const auto upTo = std::min(mix.getTotalFrames(), static_cast<juce::int64>(std::llround((k - first + 1) * static_cast<double>(rate) / fps)));
        const auto count = static_cast<int>(std::max<juce::int64>(0, upTo - mix.getFramesRead()));
        if (count > 0)
        {
            block.setSize(channels, count, false, false, true);
            const int got = mix.read(block, count);
            pcm.resize(static_cast<std::size_t>(got) * blockAlign);
            auto* p = pcm.data();
            for (int i = 0; i < got; ++i)
                for (int c = 0; c < channels; ++c)
                {
                    const auto v = static_cast<std::int32_t>(std::lround(static_cast<double>(std::clamp(block.getSample(c, i), -1.0f, 1.0f)) * 8388607.0));
                    const auto u = static_cast<std::uint32_t>(v);
                    *p++ = static_cast<char>(u & 0xff);
                    *p++ = static_cast<char>((u >> 8) & 0xff);
                    *p++ = static_cast<char>((u >> 16) & 0xff);
                }
            const auto chunk = static_cast<std::uint32_t>(pcm.size());
            index.push_back({fourcc("01wb"), static_cast<std::uint32_t>(out.getPosition() - moviStart), chunk});
            id("01wb");
            u32(chunk);
            out.write(pcm.data(), pcm.size());
            if ((chunk & 1) != 0)
                out.writeByte(0);
        }
        if (progress && !progress(static_cast<double>(k - first + 1) / (last - first)))
        {
            out.flush();
            return juce::Result::fail("Export cancelled.");
        }
    }
    close(movi);
    id("idx1");
    u32(static_cast<std::uint32_t>(index.size() * 16));
    for (const auto& e : index)
    {
        u32(e.id);
        u32(0x10); // key frame (every Motion-JPEG frame; sound chunks too)
        u32(e.offset);
        u32(e.size);
    }
    close(riff);

    for (const auto& [at, value] : patches)
    {
        out.setPosition(at);
        u32(static_cast<std::uint32_t>(value));
    }
    out.flush();
    return out.getStatus().wasOk() ? juce::Result::ok() : juce::Result::fail("Writing " + s.destination.getFileName() + " failed (is the disk full?).");
}

/** The sound of an AVI (PCM 16, 24 or 32-bit, or float). */
bool decodeAviAudio(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate, juce::String& error)
{
    juce::FileInputStream in(file);
    Avi avi;
    if (!in.openedOk() || !parseAvi(in, avi))
    {
        error = "Not an AVI file.";
        return false;
    }
    const auto* sound = avi.find(fourcc("auds"));
    if (sound == nullptr || sound->format.getSize() < 16)
    {
        error = "The AVI has no sound.";
        return false;
    }
    const auto* f = static_cast<const std::uint8_t*>(sound->format.getData());
    const auto u16 = [f](int at) { return static_cast<int>(f[at] | (f[at + 1] << 8)); };
    const auto u32 = [f](int at)
    {
        return static_cast<std::uint32_t>(f[at]) | (static_cast<std::uint32_t>(f[at + 1]) << 8) | (static_cast<std::uint32_t>(f[at + 2]) << 16)
               | (static_cast<std::uint32_t>(f[at + 3]) << 24);
    };
    const int tag = u16(0);
    const int channels = u16(2);
    sampleRate = static_cast<double>(u32(4));
    const int bits = u16(14);
    const bool isFloat = tag == 3 || (tag == 0xfffe && sound->format.getSize() >= 40 && f[24] == 3);
    if (!(tag == 1 || tag == 3 || tag == 0xfffe) || channels <= 0 || (bits != 16 && bits != 24 && bits != 32))
    {
        error = "The AVI's sound is not PCM.";
        return false;
    }
    const int bytes = bits / 8;
    juce::int64 total = 0;
    for (const auto& chunk : sound->chunks)
        total += chunk.second / static_cast<std::uint32_t>(bytes * channels);
    audio.setSize(channels, static_cast<int>(total));
    int at = 0;
    std::vector<std::uint8_t> data;
    for (const auto& [offset, size] : sound->chunks)
    {
        data.resize(size);
        in.setPosition(offset);
        in.read(data.data(), static_cast<int>(size));
        const auto frames = static_cast<int>(size / static_cast<std::uint32_t>(bytes * channels));
        const auto* p = data.data();
        for (int i = 0; i < frames; ++i, ++at)
            for (int c = 0; c < channels; ++c, p += bytes)
            {
                float v = 0.0f;
                if (bytes == 2)
                    v = static_cast<float>(static_cast<std::int16_t>(p[0] | (p[1] << 8))) / 32768.0f;
                else if (bytes == 3)
                    v = static_cast<float>(static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0] << 8 | p[1] << 16 | p[2] << 24)) >> 8) / 8388608.0f;
                else
                {
                    const auto u = static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16)
                                   | (static_cast<std::uint32_t>(p[3]) << 24);
                    if (isFloat)
                        std::memcpy(&v, &u, sizeof(float));
                    else
                        v = static_cast<float>(static_cast<double>(static_cast<std::int32_t>(u)) / 2147483648.0);
                }
                audio.setSample(c, at, v);
            }
    }
    return true;
}
} // namespace

int speakerBit(juce::AudioChannelSet::ChannelType type) noexcept
{
    using T = juce::AudioChannelSet;
    // (a table rather than a switch: the enum has a hundred ambisonic and discrete values)
    static constexpr std::pair<T::ChannelType, int> bits[] = {
        {T::left, 0}, // front left
        {T::right, 1},
        {T::centre, 2},
        {T::LFE, 3},
        {T::leftSurround, 4}, // (5.1's surrounds: the backs, as in WAVE's and AAC's 5.1)
        {T::rightSurround, 5},
        {T::leftSurroundRear, 4},
        {T::rightSurroundRear, 5},
        {T::leftCentre, 6},
        {T::rightCentre, 7},
        {T::centreSurround, 8},
        {T::leftSurroundSide, 9},
        {T::rightSurroundSide, 10},
        {T::topMiddle, 11},
        {T::topFrontLeft, 12},
        {T::topFrontCentre, 13},
        {T::topFrontRight, 14},
        {T::topRearLeft, 15},
        {T::topRearCentre, 16},
        {T::topRearRight, 17},
        {T::wideLeft, 31}, // (from here on FFmpeg's only: WAVE has no names for them)
        {T::wideRight, 32},
        {T::LFE2, 35},
        {T::topSideLeft, 36},
        {T::topSideRight, 37},
    };
    for (const auto& [speaker, bit] : bits)
        if (speaker == type)
            return bit;
    return -1;
}

namespace detail
{
MixReader::MixReader(const juce::File& wav, double offsetSeconds, double lengthSeconds,
                     const juce::Array<juce::AudioChannelSet::ChannelType>& speakers)
{
    juce::WavAudioFormat format;
    auto stream = std::make_unique<juce::FileInputStream>(wav);
    if (!stream->openedOk())
        return;
    reader_.reset(format.createReaderFor(stream.release(), true));
    if (reader_ == nullptr)
        return;
    const double rate = reader_->sampleRate > 0.0 ? reader_->sampleRate : 48000.0;
    offset_ = std::llround(std::max(0.0, offsetSeconds) * rate);
    total_ = lengthSeconds > 0.0 ? std::llround(lengthSeconds * rate) : offset_ + reader_->lengthInSamples;

    // The containers' order: named speakers by their bit, any others after them as they were.
    const int channels = static_cast<int>(reader_->numChannels);
    if (speakers.size() != channels)
        return;
    std::vector<std::pair<int, int>> keyed; // (sort key, channel)
    std::uint64_t mask = 0;
    bool named = true;
    for (int c = 0; c < channels; ++c)
    {
        int bit = speakerBit(speakers.getUnchecked(c));
        if (bit >= 0 && (mask & (std::uint64_t{1} << bit)) != 0)
            bit = -1; // (a speaker twice: the second is unnamed)
        if (bit >= 0)
            mask |= std::uint64_t{1} << bit;
        else
            named = false;
        keyed.emplace_back(bit >= 0 ? bit : 64 + c, c);
    }
    std::stable_sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    mask_ = named ? mask : 0;
    bool identity = true;
    for (int k = 0; k < channels; ++k)
        identity = identity && keyed[static_cast<std::size_t>(k)].second == k;
    if (!identity)
        for (const auto& entry : keyed)
            order_.push_back(entry.second);
}

int MixReader::getNumChannels() const noexcept { return reader_ != nullptr ? static_cast<int>(reader_->numChannels) : 0; }
double MixReader::getSampleRate() const noexcept { return reader_ != nullptr ? reader_->sampleRate : 48000.0; }

int MixReader::read(juce::AudioBuffer<float>& destination, int count)
{
    count = static_cast<int>(std::min<juce::int64>(count, total_ - position_));
    if (count <= 0 || reader_ == nullptr)
        return 0;
    const int channels = getNumChannels();
    if (!order_.empty())
        scratch_.setSize(channels, count, false, false, true);
    auto& target = order_.empty() ? destination : scratch_;
    target.clear(0, count);
    int done = position_ < offset_ ? static_cast<int>(std::min<juce::int64>(count, offset_ - position_)) : 0; // (silence first)
    if (done < count)
    {
        const auto fileStart = position_ + done - offset_;
        const auto available = static_cast<int>(std::clamp<juce::int64>(reader_->lengthInSamples - fileStart, 0, count - done));
        if (available > 0)
            reader_->read(&target, done, available, fileStart, true, true);
        done = count; // (after the mix: silence)
    }
    if (!order_.empty())
        for (int k = 0; k < channels; ++k)
            destination.copyFrom(k, 0, scratch_, order_[static_cast<std::size_t>(k)], 0, count);
    position_ += count;
    return count;
}
} // namespace detail

bool MuxSupport::aacHolds(int channels) const noexcept
{
    if (aacChannels <= 0 || channels <= 0)
        return false;
    if (aacSurround)
        return channels == 1 || channels == 2 || channels == 6;
    return channels <= aacChannels;
}

MuxSupport muxSupport(const juce::File& video)
{
    MuxSupport support;
    if (isBuiltInAvi(video))
    {
        support.backend = "built-in (AVI)";
        support.extension = ".avi";
        support.pcmExtension = ".avi";
        support.pcm = true;
        return support;
    }
#if DOJO_HAS_FFMPEG
    if (detail::ffmpegCanMux(video))
    {
        support.backend = "FFmpeg " + getFfmpegVersion();
        auto extension = video.getFileExtension().toLowerCase();
        if (extension == ".webm" || extension.isEmpty())
            extension = ".mkv"; // (WebM holds no AAC or PCM)
        support.extension = extension;
        support.pcmExtension = extension == ".mp4" || extension == ".m4v" ? ".mov" : extension;
        support.pcm = true;
        support.aacChannels = 8;
        return support;
    }
#endif
#if JUCE_WINDOWS
    juce::String error;
    if (openWithMediaFoundation(video, error) != nullptr)
    {
        support.backend = "Media Foundation";
        support.extension = ".mp4";
        support.aacChannels = 6;
        support.aacSurround = true;
        support.aacMaxBitrate = 192000;
        return support;
    }
#endif
    return support;
}

double keyFrameAtOrBefore(const juce::File& video, double seconds)
{
    seconds = std::max(0.0, seconds);
    Avi avi;
    if (isBuiltInAvi(video, &avi))
    {
        const auto& picture = *avi.find(fourcc("vids"));
        const double fps = static_cast<double>(picture.rate) / static_cast<double>(picture.scale);
        return std::floor(seconds * fps + 1.0e-6) / fps;
    }
#if DOJO_HAS_FFMPEG
    if (detail::ffmpegCanMux(video))
        return detail::ffmpegKeyFrameAtOrBefore(video, seconds);
#endif
#if JUCE_WINDOWS
    return detail::mediaFoundationKeyFrameAtOrBefore(video, seconds);
#else
    return seconds;
#endif
}

juce::Result muxAudioIntoVideo(const MuxSettings& settings, const std::function<bool(double)>& progress)
{
    if (!settings.video.existsAsFile())
        return juce::Result::fail("The movie " + settings.video.getFullPathName() + " is not there.");
    if (!settings.audio.existsAsFile())
        return juce::Result::fail("The mix is not there.");
    const auto support = muxSupport(settings.video);
    if (!support.canMux())
        return juce::Result::fail("Nothing here can write " + settings.video.getFileName()
                                  + " with a new sound (FFmpeg on Linux, Media Foundation on Windows; Motion-JPEG AVI everywhere).");
    if (settings.codec == MuxAudioCodec::pcm && !support.pcm)
        return juce::Result::fail(support.backend + " cannot put PCM sound in the movie: choose AAC.");
    if (settings.codec == MuxAudioCodec::aac && support.aacChannels == 0)
        return juce::Result::fail(support.backend + " cannot put AAC sound in the movie: choose PCM.");
    if (settings.destination == settings.video)
        return juce::Result::fail("That would write over the movie itself: choose another file.");
    if (!settings.destination.getParentDirectory().createDirectory())
        return juce::Result::fail("Could not create " + settings.destination.getParentDirectory().getFullPathName());

    auto result = juce::Result::fail("No muxer for " + settings.video.getFileName() + ".");
    if (support.backend.startsWith("built-in"))
        result = muxBuiltInAvi(settings, progress);
#if DOJO_HAS_FFMPEG
    else if (support.backend.startsWith("FFmpeg"))
        result = detail::muxWithFfmpeg(settings, progress);
#endif
#if JUCE_WINDOWS
    else if (support.backend == "Media Foundation")
        result = detail::muxWithMediaFoundation(settings, progress);
#endif
    if (result.failed())
        settings.destination.deleteFile(); // (a cancelled or broken file is no use)
    return result;
}

bool decodeAudio(const juce::File& file, juce::AudioBuffer<float>& audio, double& sampleRate, juce::String& error)
{
    if (file.hasFileExtension("avi") && decodeAviAudio(file, audio, sampleRate, error))
        return true;
#if DOJO_HAS_FFMPEG
    return detail::decodeAudioWithFfmpeg(file, audio, sampleRate, error);
#else
    if (error.isEmpty())
        error = "Decoding the sound of " + file.getFileName() + " needs FFmpeg.";
    return false;
#endif
}

} // namespace dojo::video

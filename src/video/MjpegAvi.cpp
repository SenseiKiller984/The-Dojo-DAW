#include "video/MjpegAvi.h"

#include <cmath>
#include <functional>

namespace dojo::video
{
namespace
{
constexpr std::uint32_t fourcc(const char (&id)[5]) noexcept
{
    return static_cast<std::uint32_t>(static_cast<unsigned char>(id[0])) | (static_cast<std::uint32_t>(static_cast<unsigned char>(id[1])) << 8)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(id[2])) << 16)
           | (static_cast<std::uint32_t>(static_cast<unsigned char>(id[3])) << 24);
}

std::uint32_t readU32(juce::InputStream& in) { return static_cast<std::uint32_t>(in.readInt()); }

/** Motion-JPEG compressions found in AVI files. */
bool isMotionJpeg(std::uint32_t compression)
{
    for (const auto id : {fourcc("MJPG"), fourcc("mjpg"), fourcc("AVRn"), fourcc("dmb1"), fourcc("JPEG"), fourcc("jpeg")})
        if (compression == id)
            return true;
    return false;
}
} // namespace

std::unique_ptr<MjpegAviSource> MjpegAviSource::open(const juce::File& file, juce::String& error)
{
    auto stream = file.createInputStream();
    if (stream == nullptr)
    {
        error = "Could not open " + file.getFullPathName();
        return nullptr;
    }
    if (stream->getTotalLength() < 12 || readU32(*stream) != fourcc("RIFF"))
        return nullptr;
    readU32(*stream);
    if (readU32(*stream) != fourcc("AVI "))
        return nullptr;

    auto source = std::unique_ptr<MjpegAviSource>(new MjpegAviSource());
    source->stream_ = std::move(stream);
    if (!source->parse(error))
        return nullptr;
    return source;
}

bool MjpegAviSource::parse(juce::String& error)
{
    auto& in = *stream_;
    const auto total = in.getTotalLength();
    std::uint32_t compression = 0;
    std::uint32_t scale = 0;
    std::uint32_t rate = 0;
    double microsecondsPerFrame = 0.0;
    int streamIndex = -1;  // the strl being read
    int videoStream = -1;  // the first video stream's index

    // Walks the chunks of [begin, end). Lists recurse; frames are recorded, not read.
    const std::function<void(juce::int64, juce::int64, std::uint32_t)> walk = [&](juce::int64 begin, juce::int64 end, std::uint32_t list)
    {
        juce::int64 position = begin;
        while (position + 8 <= end && position + 8 <= total)
        {
            in.setPosition(position);
            const auto id = readU32(in);
            const auto size = static_cast<juce::int64>(readU32(in));
            const auto data = position + 8;

            if (id == fourcc("LIST") || id == fourcc("RIFF"))
            {
                const auto type = readU32(in);
                if (type == fourcc("strl"))
                    ++streamIndex;
                walk(data + 4, std::min(end, data + size), type);
            }
            else if (id == fourcc("avih") && size >= 40)
            {
                microsecondsPerFrame = static_cast<double>(readU32(in));
                in.skipNextBytes(28);
                info_.width = static_cast<int>(readU32(in));
                info_.height = static_cast<int>(readU32(in));
            }
            else if (id == fourcc("strh") && size >= 32 && list == fourcc("strl"))
            {
                const auto type = readU32(in);
                in.skipNextBytes(16);
                const auto s = readU32(in);
                const auto r = readU32(in);
                if (type == fourcc("vids") && videoStream < 0)
                {
                    videoStream = streamIndex;
                    scale = s;
                    rate = r;
                }
            }
            else if (id == fourcc("strf") && size >= 20 && list == fourcc("strl") && streamIndex == videoStream && compression == 0)
            {
                in.skipNextBytes(4);
                const auto width = static_cast<int>(readU32(in));
                const auto height = static_cast<int>(in.readInt());
                in.skipNextBytes(4);
                compression = readU32(in);
                if (width > 0)
                    info_.width = width;
                if (height != 0)
                    info_.height = std::abs(height);
            }
            else if (list == fourcc("movi") || list == fourcc("rec "))
            {
                // "NNdc" / "NNdb": a frame of stream NN.
                const auto digits = static_cast<char>(id & 0xff);
                const auto digits2 = static_cast<char>((id >> 8) & 0xff);
                const auto kind = (id >> 16) & 0xffff;
                if (juce::CharacterFunctions::isDigit(digits) && juce::CharacterFunctions::isDigit(digits2)
                    && (kind == (fourcc("00dc") >> 16) || kind == (fourcc("00db") >> 16))
                    && (digits - '0') * 10 + (digits2 - '0') == std::max(0, videoStream))
                    frames_.push_back({data, static_cast<int>(size)});
            }

            position = data + size + (size & 1); // chunks are word-aligned
        }
    };
    walk(12, total, fourcc("AVI "));

    if (videoStream < 0 || !isMotionJpeg(compression))
        return false; // not ours: another decoder may read it
    if (frames_.empty())
    {
        error = "The AVI file has no video frames.";
        return false;
    }

    info_.frameRate = scale > 0 && rate > 0 ? static_cast<double>(rate) / scale
                      : microsecondsPerFrame > 0.0 ? 1.0e6 / microsecondsPerFrame
                                                   : 25.0;
    info_.durationSeconds = static_cast<double>(frames_.size()) / info_.frameRate;
    info_.codec = "MJPEG";
    info_.backend = "Motion-JPEG AVI";
    return true;
}

juce::Image MjpegAviSource::decode(int index)
{
    // Empty chunks repeat the previous frame (dropped frames).
    while (index >= 0 && frames_[static_cast<std::size_t>(index)].size <= 0)
        --index;
    if (index < 0)
        return {};
    if (index == cachedIndex_)
        return cached_;

    const auto& frame = frames_[static_cast<std::size_t>(index)];
    juce::MemoryBlock data(static_cast<std::size_t>(frame.size));
    stream_->setPosition(frame.offset);
    if (stream_->read(data.getData(), frame.size) != frame.size)
        return {};

    juce::JPEGImageFormat jpeg;
    juce::MemoryInputStream jpegStream(data, false);
    auto image = jpeg.decodeImage(jpegStream);
    if (image.isValid() && image.getFormat() != juce::Image::ARGB)
        image = image.convertedToFormat(juce::Image::ARGB);
    cachedIndex_ = index;
    cached_ = image;
    return image;
}

juce::Image MjpegAviSource::getFrame(double seconds, int maxWidth, int maxHeight)
{
    if (seconds < 0.0)
        return {};
    const auto index = info_.frameIndexAt(seconds);
    if (index >= static_cast<long long>(frames_.size()))
        return {};
    return fitWithin(decode(static_cast<int>(index)), maxWidth, maxHeight);
}

//==================================================================================================

bool writeMjpegAvi(const juce::File& file, const std::vector<juce::Image>& frames, double frameRate, float quality)
{
    if (frames.empty() || frameRate <= 0.0 || !frames.front().isValid())
        return false;

    const int width = frames.front().getWidth();
    const int height = frames.front().getHeight();
    const auto rate = static_cast<std::uint32_t>(std::lround(frameRate * 1000.0));

    // Frames first (their sizes go into the headers).
    juce::JPEGImageFormat jpeg;
    jpeg.setQuality(quality);
    std::vector<juce::MemoryBlock> encoded;
    std::uint32_t largest = 0;
    for (const auto& frame : frames)
    {
        juce::MemoryOutputStream out;
        if (!jpeg.writeImageToStream(frame.rescaled(width, height), out))
            return false;
        encoded.push_back(out.getMemoryBlock());
        largest = std::max(largest, static_cast<std::uint32_t>(encoded.back().getSize()));
    }

    juce::MemoryBlock block;
    juce::MemoryOutputStream avi(block, false);
    const auto u32 = [&](std::uint32_t v) { avi.writeInt(static_cast<int>(v)); };
    const auto u16 = [&](std::uint16_t v) { avi.writeShort(static_cast<short>(v)); };
    const auto id = [&](const char (&text)[5]) { u32(fourcc(text)); };
    // Chunk sizes are known only once their contents are written: patched in at the end.
    std::vector<std::pair<juce::int64, std::uint32_t>> patches;
    const auto patch = [&](juce::int64 at, juce::int64 value) { patches.emplace_back(at, static_cast<std::uint32_t>(value)); };
    const auto count = static_cast<std::uint32_t>(frames.size());

    id("RIFF");
    const auto riffSize = avi.getPosition();
    u32(0);
    id("AVI ");

    id("LIST");
    const auto hdrlSize = avi.getPosition();
    u32(0);
    id("hdrl");
    id("avih");
    u32(56);
    u32(static_cast<std::uint32_t>(std::lround(1.0e6 / frameRate))); // microseconds per frame
    u32(static_cast<std::uint32_t>(largest * frameRate));            // max bytes per second
    u32(0);                                                          // padding granularity
    u32(0x10);                                                       // AVIF_HASINDEX
    u32(count);
    u32(0);
    u32(1); // streams
    u32(largest);
    u32(static_cast<std::uint32_t>(width));
    u32(static_cast<std::uint32_t>(height));
    for (int i = 0; i < 4; ++i)
        u32(0);

    id("LIST");
    const auto strlSize = avi.getPosition();
    u32(0);
    id("strl");
    id("strh");
    u32(56);
    id("vids");
    id("MJPG");
    u32(0);   // flags
    u16(0);   // priority
    u16(0);   // language
    u32(0);   // initial frames
    u32(1000);
    u32(rate); // rate / scale = frames per second
    u32(0);
    u32(count);
    u32(largest);
    u32(0xffffffffu); // quality: default
    u32(0);
    u16(0);
    u16(0);
    u16(static_cast<std::uint16_t>(width));
    u16(static_cast<std::uint16_t>(height));
    id("strf");
    u32(40);
    u32(40);
    u32(static_cast<std::uint32_t>(width));
    u32(static_cast<std::uint32_t>(height));
    u16(1);
    u16(24);
    id("MJPG");
    u32(static_cast<std::uint32_t>(width * height * 3));
    u32(0);
    u32(0);
    u32(0);
    u32(0);
    patch(strlSize, avi.getPosition() - strlSize - 4);
    patch(hdrlSize, avi.getPosition() - hdrlSize - 4);

    id("LIST");
    const auto moviSize = avi.getPosition();
    u32(0);
    const auto moviStart = avi.getPosition();
    id("movi");
    std::vector<std::pair<std::uint32_t, std::uint32_t>> index; // offset (from "movi"), size
    for (const auto& frame : encoded)
    {
        index.emplace_back(static_cast<std::uint32_t>(avi.getPosition() - moviStart), static_cast<std::uint32_t>(frame.getSize()));
        id("00dc");
        u32(static_cast<std::uint32_t>(frame.getSize()));
        avi.write(frame.getData(), frame.getSize());
        if ((frame.getSize() & 1) != 0)
            avi.writeByte(0);
    }
    patch(moviSize, avi.getPosition() - moviSize - 4);

    id("idx1");
    u32(count * 16);
    for (const auto& [offset, size] : index)
    {
        id("00dc");
        u32(0x10); // key frame
        u32(offset);
        u32(size);
    }
    patch(riffSize, avi.getPosition() - 8);
    avi.flush();

    auto* bytes = static_cast<std::uint8_t*>(block.getData());
    for (const auto& [at, value] : patches)
        for (int b = 0; b < 4; ++b)
            bytes[at + b] = static_cast<std::uint8_t>((value >> (8 * b)) & 0xff);

    file.deleteFile();
    return file.replaceWithData(block.getData(), block.getSize());
}

} // namespace dojo::video

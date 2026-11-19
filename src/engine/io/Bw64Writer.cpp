#include "engine/io/Bw64Writer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace dojo::engine::adm
{
namespace
{
constexpr std::uint32_t kDs64Size = 28; ///< bw64Size, dataSize, dummySize (8 bytes each) and tableLength (4)
constexpr std::uint64_t kMax32 = 0xFFFFFFFFull;

void writeId(juce::OutputStream& out, const char* id) { out.write(id, 4); }

void writeFixed(juce::MemoryOutputStream& out, const juce::String& text, std::size_t length)
{
    // An ASCII field of a fixed length, padded with zeros.
    const auto utf8 = text.toRawUTF8();
    const auto size = std::min(std::strlen(utf8), length);
    out.write(utf8, size);
    for (std::size_t i = size; i < length; ++i)
        out.writeByte(0);
}
} // namespace

Bw64Writer::Bw64Writer(const juce::File& file, int numChannels, double sampleRate, Format format, const std::vector<TrackEntry>& tracks)
    : numChannels_(std::max(1, numChannels))
    , format_(format)
{
    file.deleteFile();
    auto stream = std::make_unique<juce::FileOutputStream>(file, 1 << 18);
    if (stream->failedToOpen())
    {
        error_ = "Could not write " + file.getFullPathName();
        return;
    }
    stream_ = std::move(stream);
    auto& out = *stream_;

    const int bytes = getBytesPerSample();
    const int blockAlign = bytes * numChannels_;
    const auto rate = static_cast<std::uint32_t>(std::lround(sampleRate));

    writeId(out, "RIFF");
    out.writeInt(0); // (patched by finish)
    writeId(out, "WAVE");

    writeId(out, "JUNK"); // (becomes ds64 beyond 4 GB)
    out.writeInt(static_cast<int>(kDs64Size));
    for (std::uint32_t i = 0; i < kDs64Size; ++i)
        out.writeByte(0);

    const bool isFloat = format_ == Format::float32;
    writeId(out, "fmt ");
    out.writeInt(isFloat ? 18 : 16);
    out.writeShort(static_cast<short>(isFloat ? 3 : 1)); // WAVE_FORMAT_IEEE_FLOAT / WAVE_FORMAT_PCM
    out.writeShort(static_cast<short>(numChannels_));
    out.writeInt(static_cast<int>(rate));
    out.writeInt(static_cast<int>(rate * static_cast<std::uint32_t>(blockAlign)));
    out.writeShort(static_cast<short>(blockAlign));
    out.writeShort(static_cast<short>(bytes * 8));
    if (isFloat)
        out.writeShort(0); // cbSize

    const auto chunk = chna(tracks);
    writeId(out, "chna");
    out.writeInt(static_cast<int>(chunk.getSize()));
    out.write(chunk.getData(), chunk.getSize());

    writeId(out, "data");
    dataSizePosition_ = out.getPosition();
    out.writeInt(0);

    if (out.getStatus().failed())
        fail("Could not write " + file.getFullPathName());
}

Bw64Writer::~Bw64Writer()
{
    if (stream_ != nullptr && !finished_)
        finish({});
}

int Bw64Writer::getBytesPerSample() const noexcept
{
    switch (format_)
    {
    case Format::pcm16:   return 2;
    case Format::pcm24:   return 3;
    case Format::float32: return 4;
    }
    return 3;
}

juce::MemoryBlock Bw64Writer::chna(const std::vector<TrackEntry>& tracks)
{
    // numTracks and numUIDs, then 40 bytes per UID: trackIndex (2), UID (12), trackRef (14),
    // packRef (11) and a pad byte.
    juce::MemoryOutputStream out;
    std::vector<int> distinct;
    for (const auto& t : tracks)
        if (std::find(distinct.begin(), distinct.end(), t.trackIndex) == distinct.end())
            distinct.push_back(t.trackIndex);
    out.writeShort(static_cast<short>(distinct.size()));
    out.writeShort(static_cast<short>(tracks.size()));
    for (const auto& t : tracks)
    {
        out.writeShort(static_cast<short>(t.trackIndex));
        writeFixed(out, t.uid, 12);
        writeFixed(out, t.trackRef, 14);
        writeFixed(out, t.packRef, 11);
        out.writeByte(0);
    }
    return out.getMemoryBlock();
}

bool Bw64Writer::fail(const juce::String& message)
{
    if (error_.isEmpty())
        error_ = message;
    return false;
}

bool Bw64Writer::write(const float* const* channels, int numFrames)
{
    if (stream_ == nullptr || finished_)
        return fail("The file is not open.");
    if (numFrames <= 0)
        return true;

    const int bytes = getBytesPerSample();
    const auto frameBytes = static_cast<std::size_t>(bytes * numChannels_);
    scratch_.resize(frameBytes * static_cast<std::size_t>(numFrames));
    char* p = scratch_.data();
    for (int i = 0; i < numFrames; ++i)
        for (int c = 0; c < numChannels_; ++c)
        {
            const float v = channels[c] != nullptr ? channels[c][i] : 0.0f;
            switch (format_)
            {
            case Format::pcm16:
            {
                const auto s = static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
                const auto u = static_cast<std::uint16_t>(s);
                *p++ = static_cast<char>(u & 0xff);
                *p++ = static_cast<char>((u >> 8) & 0xff);
                break;
            }
            case Format::pcm24:
            {
                const auto s = static_cast<std::int32_t>(std::lround(static_cast<double>(std::clamp(v, -1.0f, 1.0f)) * 8388607.0));
                const auto u = static_cast<std::uint32_t>(s);
                *p++ = static_cast<char>(u & 0xff);
                *p++ = static_cast<char>((u >> 8) & 0xff);
                *p++ = static_cast<char>((u >> 16) & 0xff);
                break;
            }
            case Format::float32:
            {
                std::uint32_t u = 0;
                std::memcpy(&u, &v, sizeof(u));
                *p++ = static_cast<char>(u & 0xff);
                *p++ = static_cast<char>((u >> 8) & 0xff);
                *p++ = static_cast<char>((u >> 16) & 0xff);
                *p++ = static_cast<char>((u >> 24) & 0xff);
                break;
            }
            }
        }
    if (!stream_->write(scratch_.data(), scratch_.size()))
        return fail("Writing the audio file failed (is the disk full?).");
    frames_ += static_cast<std::uint64_t>(numFrames);
    return true;
}

bool Bw64Writer::finish(const juce::String& axml)
{
    if (stream_ == nullptr || finished_)
        return fail("The file is not open.");
    finished_ = true;
    auto& out = *stream_;

    const std::uint64_t dataSize = frames_ * static_cast<std::uint64_t>(getBytesPerSample() * numChannels_);
    if ((dataSize & 1u) != 0)
        out.writeByte(0); // (chunks start on even bytes)

    if (axml.isNotEmpty())
    {
        const auto text = axml.toUTF8();
        const auto size = std::strlen(text.getAddress());
        writeId(out, "axml");
        out.writeInt(static_cast<int>(static_cast<std::uint32_t>(size)));
        out.write(text.getAddress(), size);
        if ((size & 1u) != 0)
            out.writeByte(0);
    }

    out.flush();
    const auto fileSize = static_cast<std::uint64_t>(out.getPosition());
    const std::uint64_t riffSize = fileSize - 8;
    const bool bw64 = alwaysBw64_ || riffSize > kMax32 || dataSize > kMax32;

    bool ok = out.setPosition(0);
    writeId(out, bw64 ? "BW64" : "RIFF");
    out.writeInt(static_cast<int>(static_cast<std::uint32_t>(bw64 ? kMax32 : riffSize)));
    if (bw64)
    {
        ok = ok && out.setPosition(12);
        writeId(out, "ds64");
        out.writeInt(static_cast<int>(kDs64Size));
        out.writeInt64(static_cast<juce::int64>(riffSize));
        out.writeInt64(static_cast<juce::int64>(dataSize));
        out.writeInt64(0); // dummySize
        out.writeInt(0);   // tableLength
    }
    ok = ok && out.setPosition(dataSizePosition_);
    out.writeInt(static_cast<int>(static_cast<std::uint32_t>(bw64 ? kMax32 : dataSize)));
    out.flush();
    ok = ok && out.getStatus().wasOk();
    stream_.reset();
    return ok ? true : fail("Finishing the audio file failed (is the disk full?).");
}

} // namespace dojo::engine::adm

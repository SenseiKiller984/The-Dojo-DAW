#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace dojo::engine::adm
{

/** One channel of a BW64 file and the ADM audio track it carries: an entry of the chna chunk. */
struct TrackEntry
{
    int trackIndex = 1;    ///< the file's channel, counted from 1
    juce::String uid;      ///< its audioTrackUID, "ATU_00000001"
    juce::String trackRef; ///< the audioTrackFormat it plays, "AT_00010001_01"
    juce::String packRef;  ///< the audioPackFormat it belongs to, "AP_00010003"
};

/**
    Writes a BW64 file (ITU-R BS.2088-1): the RIFF/WAVE layout broadcast WAV files use, able to grow
    beyond 4 GB, with the chunks that carry ADM metadata (ITU-R BS.2076).

    The chunks, in order:
      - "RIFF" "WAVE" ("BW64" once the file is over 4 GB);
      - "JUNK": the 28 bytes a "ds64" chunk needs, kept first so that the file can become BW64
        without anything moving;
      - "fmt ": PCM (16 or 24 bit) or IEEE float (32 bit), interleaved;
      - "chna": which ADM track each channel carries;
      - "data": the audio;
      - "axml": the ADM XML, after the audio (an object's blocks are only known once it has played).
    finish() patches the sizes. Over 4 GB the header says BW64, the JUNK becomes "ds64" holding the
    64-bit RIFF and data sizes, and the 32-bit size fields say 0xFFFFFFFF.

    One thread writes (an export's writer thread).
*/
class Bw64Writer
{
public:
    enum class Format
    {
        pcm16,
        pcm24,
        float32,
    };

    Bw64Writer(const juce::File& file, int numChannels, double sampleRate, Format format, const std::vector<TrackEntry>& tracks);
    ~Bw64Writer();
    Bw64Writer(const Bw64Writer&) = delete;
    Bw64Writer& operator=(const Bw64Writer&) = delete;

    [[nodiscard]] bool isOpen() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] const juce::String& getError() const noexcept { return error_; }
    [[nodiscard]] std::uint64_t getFramesWritten() const noexcept { return frames_; }
    [[nodiscard]] int getNumChannels() const noexcept { return numChannels_; }
    [[nodiscard]] int getBytesPerSample() const noexcept;

    /** Appends frames: `channels[c]` is the file's channel c (null: silence). */
    bool write(const float* const* channels, int numFrames);
    /** Appends the axml chunk and patches the sizes: the file is complete (and closed). */
    bool finish(const juce::String& axml);

    /** BW64 with a ds64 chunk whatever the size (tests: a file over 4 GB takes long to write). */
    void setAlwaysBw64(bool always) noexcept { alwaysBw64_ = always; }

    /** The chna chunk's payload for these tracks. */
    [[nodiscard]] static juce::MemoryBlock chna(const std::vector<TrackEntry>& tracks);

private:
    bool fail(const juce::String& message);

    std::unique_ptr<juce::FileOutputStream> stream_;
    juce::String error_;
    int numChannels_ = 0;
    Format format_ = Format::pcm24;
    std::uint64_t frames_ = 0;
    juce::int64 dataSizePosition_ = 0;
    bool alwaysBw64_ = false;
    bool finished_ = false;
    std::vector<char> scratch_;
};

} // namespace dojo::engine::adm

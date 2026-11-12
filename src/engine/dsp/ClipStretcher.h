#pragma once

#include "core/Types.h"
#include "engine/RenderGraph.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace dojo::engine
{

/**
    Real-time time-stretch and pitch-shift state for one audio clip (Signalsmith Stretch).

    Created and configured on the message thread (allocates), then owned across graph swaps by the
    session so its internal state survives edits; only the audio thread calls render(). It detects
    discontinuities (locate, loop wrap, clip edits) and re-seeks, priming the stretcher with enough
    source material that the very next output sample is already correct.
*/
class ClipStretcher final
{
public:
    /** `numChannels`: the source channels the clip plays (1..8; one chosen channel = 1). */
    ClipStretcher(int numChannels, double sampleRate, int maximumBlockSize);
    ~ClipStretcher();

    ClipStretcher(const ClipStretcher&) = delete;
    ClipStretcher& operator=(const ClipStretcher&) = delete;

    /** Audio thread: adds output samples [first, last) of the current block into `outputs`; output o
        plays source channel sourceOf[o] (-1 = none). */
    bool render(const RenderClip& clip, double blockStartSeconds, double secondsPerSample, int first, int last,
                Sample* const* outputs, const int* sourceOf, int numOutputs) noexcept;

    [[nodiscard]] int getNumChannels() const noexcept;

    [[nodiscard]] double getConfiguredSampleRate() const noexcept { return sampleRate_; }
    [[nodiscard]] int getMaximumBlockSize() const noexcept { return maxBlock_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    double sampleRate_;
    int maxBlock_;
};

/**
    A long audio file played straight from disk instead of being decoded into memory.

    The file is divided into fixed-size chunks. A background service thread keeps the chunks that are
    being played — and the ones right after them (read-ahead) — loaded and evicts chunks nobody has
    read for a while, so several clips can play different parts of one file. The audio thread only
    ever loads chunk pointers atomically and reads from them; a missing chunk reads as silence and is
    requested.
    Evicted chunks are freed only after the engine has completed two further processing cycles, so
    the audio thread can never read freed memory.
*/
class StreamedAudio final
{
public:
    static constexpr int kChunkFrames = 1 << 16;

    /** Takes ownership of a reader (only the service thread reads from it afterwards). */
    explicit StreamedAudio(std::unique_ptr<juce::AudioFormatReader> reader);
    ~StreamedAudio();

    [[nodiscard]] int getNumChannels() const noexcept { return numChannels_; }
    [[nodiscard]] SampleCount getLength() const noexcept { return length_; }
    [[nodiscard]] double getSampleRate() const noexcept { return sampleRate_; }

    /** Audio thread: one sample (0 if its chunk is not resident yet). */
    [[nodiscard]] float sample(int channel, SampleCount index) const noexcept;

    /** Audio thread: pointer to the chunk holding `index` for `channel`, or null if not resident. */
    [[nodiscard]] const float* chunkFor(int channel, SampleCount index) const noexcept;

    /** Any thread: ask for the region around `frame` to be resident soon. */
    void prefetch(SampleCount frame) const noexcept;

    /** Service thread: load wanted chunks, evict unwanted ones. `epoch` is the engine's process epoch. */
    void service(std::uint64_t epoch);

    [[nodiscard]] int getNumResidentChunks() const noexcept;

private:
    struct Chunk;
    void load(int chunkIndex);

    std::unique_ptr<juce::AudioFormatReader> reader_;
    int numChannels_ = 0;
    SampleCount length_ = 0;
    double sampleRate_ = 48000.0;
    int numChunks_ = 0;
    std::unique_ptr<std::atomic<Chunk*>[]> chunks_;
    mutable std::unique_ptr<std::atomic<bool>[]> requested_;
    mutable std::unique_ptr<std::atomic<std::uint32_t>[]> lastUsed_; ///< service tick of the last read
    mutable std::atomic<SampleCount> hint_{-1};
    std::atomic<std::uint32_t> tick_{1000};

    struct Retired
    {
        Chunk* chunk;
        std::uint64_t epoch;
    };
    std::vector<Retired> retired_;
};

} // namespace dojo::engine

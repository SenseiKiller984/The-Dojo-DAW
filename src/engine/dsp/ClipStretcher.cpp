#include "engine/dsp/ClipStretcher.h"

#include "engine/dsp/ClipRenderer.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>

namespace dojo::engine
{

//==================================================================================================
// ClipStretcher

struct ClipStretcher::Impl
{
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    std::array<std::vector<float>, kMaxChannels> input;
    std::array<std::vector<float>, kMaxChannels> output;
    std::array<std::vector<float>, kMaxChannels> seekBuffer;
    double expectedTimeInClip = -1.0;
    double feedPosition = 0.0; ///< next source frame to feed (fractional)
    int numChannels = 2;
};

ClipStretcher::ClipStretcher(int numChannels, double sampleRate, int maximumBlockSize)
    : impl_(std::make_unique<Impl>())
    , sampleRate_(sampleRate)
    , maxBlock_(std::max(64, maximumBlockSize))
{
    // One stretcher channel per source channel the clip plays (stereo at least, so a mono source
    // simply runs on both).
    impl_->numChannels = std::clamp(numChannels, 2, kMaxChannels);
    impl_->stretch.presetDefault(impl_->numChannels, static_cast<float>(sampleRate));

    // Worst case input per block: 4x varispeed, 0.25x stretch -> 16x the block, plus latency slack.
    const auto inputCapacity = static_cast<std::size_t>(maxBlock_) * 16 + 16;
    const auto seekCapacity = static_cast<std::size_t>(impl_->stretch.seekLength() + impl_->stretch.inputLatency() + 16);

    for (int ch = 0; ch < impl_->numChannels; ++ch)
    {
        const auto index = static_cast<std::size_t>(ch);
        impl_->input[index].assign(inputCapacity, 0.0f);
        impl_->output[index].assign(static_cast<std::size_t>(maxBlock_), 0.0f);
        impl_->seekBuffer[index].assign(seekCapacity, 0.0f);
    }
}

int ClipStretcher::getNumChannels() const noexcept
{
    return impl_->numChannels;
}

ClipStretcher::~ClipStretcher() = default;

namespace
{
float sourceSample(const RenderClip& clip, int channel, SampleCount index) noexcept
{
    if (index < 0 || index >= clip.sourceLength)
        return 0.0f;

    const int ch = std::min(clip.sourceChannel >= 0 ? clip.sourceChannel : channel, clip.numChannels - 1);
    if (clip.channels != nullptr)
        return clip.channels[ch][index];
    if (clip.streamed != nullptr)
        return clip.streamed->sample(ch, index);
    return 0.0f;
}

void fillFromSource(const RenderClip& clip, SampleCount start, int count, int numChannels,
                    std::array<std::vector<float>, kMaxChannels>& dest) noexcept
{
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < count; ++i)
            dest[static_cast<std::size_t>(ch)][static_cast<std::size_t>(i)] = sourceSample(clip, ch, start + i);
}
} // namespace

bool ClipStretcher::render(const RenderClip& clip, double blockStartSeconds, double secondsPerSample, int first,
                           int last, Sample* const* outputs, const int* sourceOf, int numOutputs) noexcept
{
    auto& s = *impl_;
    const int numOut = std::min(last - first, maxBlock_);
    if (numOut <= 0)
        return false;

    const double stretch = clip.stretch > 0.0 ? clip.stretch : 1.0;
    const double sourceRate = clip.sourceSampleRate;
    const double inputPerOutput = secondsPerSample / stretch * sourceRate;
    const int inputLatency = s.stretch.inputLatency();
    const int outputLatency = s.stretch.outputLatency();

    // Pitch: keep the source's pitch at the device rate, then apply the clip's transposition.
    s.stretch.setTransposeFactor(static_cast<float>(sourceRate * secondsPerSample * std::pow(2.0, clip.pitchSemitones / 12.0)));

    const double timeInClip = blockStartSeconds + first * secondsPerSample - clip.startSeconds;
    const auto sourceFrameAt = [&](double clipTime) { return (clip.sourceOffsetSeconds + clipTime / stretch) * sourceRate; };

    // Processing time runs outputLatency output samples ahead of what we hear now.
    if (std::abs(timeInClip - s.expectedTimeInClip) > secondsPerSample * 0.5)
    {
        const double processingFrame = sourceFrameAt(timeInClip + outputLatency * secondsPerSample);
        const int seekLength = std::min(static_cast<int>(s.seekBuffer[0].size()), s.stretch.seekLength() + inputLatency);
        const auto seekStart = static_cast<SampleCount>(std::floor(processingFrame)) + inputLatency - seekLength;

        fillFromSource(clip, seekStart, seekLength, s.numChannels, s.seekBuffer);
        std::array<float*, kMaxChannels> seekChannels{};
        for (int ch = 0; ch < s.numChannels; ++ch)
            seekChannels[static_cast<std::size_t>(ch)] = s.seekBuffer[static_cast<std::size_t>(ch)].data();
        s.stretch.seek(seekChannels.data(), seekLength, inputPerOutput);
        s.feedPosition = std::floor(processingFrame) + inputLatency;
    }

    const double feedEnd = sourceFrameAt(timeInClip + (numOut + outputLatency) * secondsPerSample) + inputLatency;
    const auto feedStartFrame = static_cast<SampleCount>(std::floor(s.feedPosition));
    const int numIn = std::clamp(static_cast<int>(std::floor(feedEnd) - static_cast<double>(feedStartFrame)), 0,
                                 static_cast<int>(s.input[0].size()));

    fillFromSource(clip, feedStartFrame, numIn, s.numChannels, s.input);
    s.feedPosition = static_cast<double>(feedStartFrame + numIn);

    std::array<float*, kMaxChannels> inputs{};
    std::array<float*, kMaxChannels> stretched{};
    for (int ch = 0; ch < s.numChannels; ++ch)
    {
        inputs[static_cast<std::size_t>(ch)] = s.input[static_cast<std::size_t>(ch)].data();
        stretched[static_cast<std::size_t>(ch)] = s.output[static_cast<std::size_t>(ch)].data();
    }
    s.stretch.process(inputs.data(), numIn, stretched.data(), numOut);

    for (int o = 0; o < numOutputs; ++o)
    {
        // Stretcher channel c holds source channel c (or the chosen one, in channel 0).
        const int source = sourceOf[o];
        if (source < 0 || outputs[o] == nullptr)
            continue;
        const auto& channel = s.output[static_cast<std::size_t>(std::min(clip.sourceChannel >= 0 ? 0 : source, s.numChannels - 1))];
        Sample* out = outputs[o] + first;
        for (int i = 0; i < numOut; ++i)
        {
            const double clipTime = timeInClip + i * secondsPerSample;
            out[i] += channel[static_cast<std::size_t>(i)] * clip.gain * dsp::clipEdgeGain(clip, clipTime);
        }
    }

    s.expectedTimeInClip = timeInClip + numOut * secondsPerSample;
    return true;
}

//==================================================================================================
// StreamedAudio

struct StreamedAudio::Chunk
{
    std::array<std::vector<float>, kMaxChannels> data;
};

StreamedAudio::StreamedAudio(std::unique_ptr<juce::AudioFormatReader> reader)
    : reader_(std::move(reader))
{
    numChannels_ = reader_ != nullptr ? std::min(kMaxChannels, static_cast<int>(reader_->numChannels)) : 0;
    length_ = reader_ != nullptr ? reader_->lengthInSamples : 0;
    sampleRate_ = reader_ != nullptr ? reader_->sampleRate : 48000.0;
    numChunks_ = static_cast<int>((length_ + kChunkFrames - 1) / kChunkFrames);

    chunks_ = std::make_unique<std::atomic<Chunk*>[]>(static_cast<std::size_t>(std::max(1, numChunks_)));
    requested_ = std::make_unique<std::atomic<bool>[]>(static_cast<std::size_t>(std::max(1, numChunks_)));
    lastUsed_ = std::make_unique<std::atomic<std::uint32_t>[]>(static_cast<std::size_t>(std::max(1, numChunks_)));
    for (int i = 0; i < numChunks_; ++i)
    {
        chunks_[static_cast<std::size_t>(i)].store(nullptr);
        requested_[static_cast<std::size_t>(i)].store(false);
        lastUsed_[static_cast<std::size_t>(i)].store(0);
    }
}

StreamedAudio::~StreamedAudio()
{
    for (int i = 0; i < numChunks_; ++i)
        delete chunks_[static_cast<std::size_t>(i)].load();
    for (auto& r : retired_)
        delete r.chunk;
}

const float* StreamedAudio::chunkFor(int channel, SampleCount index) const noexcept
{
    if (index < 0 || index >= length_ || numChannels_ == 0)
        return nullptr;

    const auto chunk = static_cast<std::size_t>(index / kChunkFrames);
    const Chunk* data = chunks_[chunk].load(std::memory_order_acquire);
    lastUsed_[chunk].store(tick_.load(std::memory_order_relaxed), std::memory_order_relaxed);

    if (data == nullptr)
    {
        requested_[chunk].store(true, std::memory_order_relaxed);
        return nullptr;
    }

    return data->data[static_cast<std::size_t>(std::clamp(channel, 0, numChannels_ - 1))].data();
}

float StreamedAudio::sample(int channel, SampleCount index) const noexcept
{
    const float* chunk = chunkFor(channel, index);
    return chunk != nullptr ? chunk[index % kChunkFrames] : 0.0f;
}

void StreamedAudio::prefetch(SampleCount frame) const noexcept
{
    hint_.store(std::clamp<SampleCount>(frame, 0, std::max<SampleCount>(0, length_ - 1)), std::memory_order_relaxed);
}

void StreamedAudio::load(int chunkIndex)
{
    auto chunk = std::make_unique<Chunk>();
    const SampleCount start = static_cast<SampleCount>(chunkIndex) * kChunkFrames;
    const int frames = static_cast<int>(std::min<SampleCount>(kChunkFrames, length_ - start));

    juce::AudioBuffer<float> buffer(std::max(1, numChannels_), frames);
    reader_->read(&buffer, 0, frames, start, true, numChannels_ > 1);

    for (int ch = 0; ch < std::max(1, numChannels_); ++ch)
    {
        auto& data = chunk->data[static_cast<std::size_t>(ch)];
        data.assign(static_cast<std::size_t>(kChunkFrames), 0.0f);
        const float* source = buffer.getReadPointer(std::min(ch, buffer.getNumChannels() - 1));
        std::copy_n(source, frames, data.begin());
    }

    chunks_[static_cast<std::size_t>(chunkIndex)].store(chunk.release(), std::memory_order_release);
}

void StreamedAudio::service(std::uint64_t epoch)
{
    if (reader_ == nullptr || numChunks_ == 0)
        return;

    constexpr std::uint32_t kRecentTicks = 400; // ~2 s of service calls
    constexpr int kReadAhead = 4;               // chunks (~5 s at 48 kHz)
    const std::uint32_t tick = tick_.fetch_add(1, std::memory_order_relaxed) + 1;

    // Free chunks evicted at least two processing cycles ago.
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                  [epoch](const Retired& r)
                                  {
                                      if (epoch < r.epoch + 2)
                                          return false;
                                      delete r.chunk;
                                      return true;
                                  }),
                   retired_.end());

    // Wanted: around the explicit hint, everything requested, and read-ahead after recently read chunks.
    std::vector<bool> wanted(static_cast<std::size_t>(numChunks_), false);
    const auto want = [&](int from, int to)
    {
        for (int i = std::max(0, from); i <= std::min(numChunks_ - 1, to); ++i)
            wanted[static_cast<std::size_t>(i)] = true;
    };

    if (const auto hint = hint_.load(std::memory_order_relaxed); hint >= 0)
    {
        const auto centre = static_cast<int>(hint / kChunkFrames);
        want(centre, centre + kReadAhead);
    }

    for (int i = 0; i < numChunks_; ++i)
    {
        const auto index = static_cast<std::size_t>(i);
        if (requested_[index].exchange(false, std::memory_order_relaxed))
            want(i, i + kReadAhead);
        else if (chunks_[index].load(std::memory_order_relaxed) != nullptr &&
                 tick - lastUsed_[index].load(std::memory_order_relaxed) < kRecentTicks)
            want(i, i + kReadAhead);
    }

    for (int i = 0; i < numChunks_; ++i)
    {
        const auto index = static_cast<std::size_t>(i);
        auto& slot = chunks_[index];

        if (wanted[index])
        {
            if (slot.load(std::memory_order_acquire) == nullptr)
                load(i); // not marked as used: read-ahead must not cascade through the whole file
        }
        else if (Chunk* evicted = slot.exchange(nullptr, std::memory_order_acq_rel))
        {
            retired_.push_back({evicted, epoch});
        }
    }
}

int StreamedAudio::getNumResidentChunks() const noexcept
{
    int count = 0;
    for (int i = 0; i < numChunks_; ++i)
        count += chunks_[static_cast<std::size_t>(i)].load() != nullptr ? 1 : 0;
    return count;
}

} // namespace dojo::engine

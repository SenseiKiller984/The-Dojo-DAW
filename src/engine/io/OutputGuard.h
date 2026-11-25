#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace dojo::engine
{

/**
    The license's say over what the sound card plays (licensing/Features.h), applied by the device
    callback after the engine, to every output channel the device plays: the mix, direct outputs,
    HW Insert sends and renderer outputs alike, so a loopback or a virtual cable gets the same.

        mute       silence (no license; the free trial while recording software runs)
        watermark  the free trial: a soft-edged noise burst (0.75 s, about -20 dBFS) on every
                   channel after every 30 seconds of output that is not silent (the first after 15)
        clean      as the engine made it

    Starts muted. setPolicy() from any thread; prepare() before the device starts; process() on
    the audio thread (real-time safe).
*/
class OutputGuard
{
public:
    enum class Policy
    {
        mute,
        watermark,
        clean,
    };

    static constexpr double kPeriodSeconds = 30.0;
    static constexpr double kFirstSeconds = 15.0;
    static constexpr double kBurstSeconds = 0.75;
    static constexpr float kLevel = 0.17f;

    void setPolicy(Policy policy) noexcept { policy_.store(static_cast<int>(policy), std::memory_order_relaxed); }
    [[nodiscard]] Policy getPolicy() const noexcept { return static_cast<Policy>(policy_.load(std::memory_order_relaxed)); }
    /** Watermark bursts started so far. */
    [[nodiscard]] std::uint64_t getWatermarkCount() const noexcept { return count_.load(std::memory_order_relaxed); }

    void prepare(double sampleRate) noexcept
    {
        rate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        untilNext_ = static_cast<std::int64_t>(kFirstSeconds * rate_);
        burstLength_ = static_cast<int>(kBurstSeconds * rate_);
        position_ = -1;
    }

    void process(float* const* out, int numChannels, int numSamples) noexcept
    {
        switch (getPolicy())
        {
        case Policy::clean:
            return;
        case Policy::watermark:
            watermark(out, numChannels, numSamples);
            return;
        case Policy::mute:
            break;
        }
        for (int c = 0; c < numChannels; ++c)
            if (out[c] != nullptr)
                juce::FloatVectorOperations::clear(out[c], numSamples);
    }

private:
    void watermark(float* const* out, int numChannels, int numSamples) noexcept
    {
        if (position_ < 0)
        {
            // Only sound counts: silence (stopped, nothing playing) never gets a burst.
            float peak = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                if (out[c] != nullptr)
                {
                    const auto range = juce::FloatVectorOperations::findMinAndMax(out[c], numSamples);
                    peak = std::max({peak, range.getEnd(), -range.getStart()});
                }
            if (peak > 1.0e-3f)
                untilNext_ -= numSamples;
            if (untilNext_ > 0)
                return;
            position_ = 0;
            untilNext_ = static_cast<std::int64_t>(kPeriodSeconds * rate_);
            count_.fetch_add(1, std::memory_order_relaxed);
        }
        for (int i = 0; i < numSamples; ++i, ++position_)
        {
            if (position_ >= burstLength_)
            {
                position_ = -1;
                return;
            }
            // Raised-cosine envelope over lightly low-passed white noise.
            const double phase = static_cast<double>(position_) / static_cast<double>(burstLength_);
            const auto envelope = static_cast<float>(0.5 - 0.5 * std::cos(juce::MathConstants<double>::twoPi * phase));
            seed_ = seed_ * 1664525u + 1013904223u;
            const float white = static_cast<float>(static_cast<std::int32_t>(seed_)) / 2147483648.0f;
            smoothed_ += 0.45f * (white - smoothed_);
            const float value = smoothed_ * envelope * kLevel * 2.0f;
            for (int c = 0; c < numChannels; ++c)
                if (out[c] != nullptr)
                    out[c][i] += value;
        }
    }

    std::atomic<int> policy_{static_cast<int>(Policy::mute)};
    std::atomic<std::uint64_t> count_{0};
    double rate_ = 48000.0;
    std::int64_t untilNext_ = static_cast<std::int64_t>(kFirstSeconds * 48000.0);
    int burstLength_ = static_cast<int>(kBurstSeconds * 48000.0);
    int position_ = -1;
    std::uint32_t seed_ = 0x2545f491u;
    float smoothed_ = 0.0f;
};

} // namespace dojo::engine

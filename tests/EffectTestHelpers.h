#pragma once

// Signal helpers shared by the device tests: test tones, measurement (Goertzel amplitude, RMS,
// peak) and a play head the tests move along block by block.

#include "DeviceTestSupport.h"

#include <cmath>
#include <vector>

namespace dojo::test::devices
{
inline constexpr double kTestPi = 3.14159265358979323846;

/** A sine on both channels (the right channel can differ). */
inline Stereo tone(double frequency, std::size_t n, double amplitude = 0.5, double rightFrequency = 0.0, double rightAmplitude = -1.0)
{
    Stereo s(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        s.left[i] = amplitude * std::sin(2.0 * kTestPi * frequency * static_cast<double>(i) / kRate);
        s.right[i] = (rightAmplitude < 0.0 ? amplitude : rightAmplitude) *
                     std::sin(2.0 * kTestPi * (rightFrequency > 0.0 ? rightFrequency : frequency) * static_cast<double>(i) / kRate);
    }
    return s;
}

/** Signals end to end. */
inline Stereo concat(std::initializer_list<Stereo> parts)
{
    Stereo out;
    for (const auto& p : parts)
    {
        out.left.insert(out.left.end(), p.left.begin(), p.left.end());
        out.right.insert(out.right.end(), p.right.begin(), p.right.end());
    }
    return out;
}

inline Stereo sum(const Stereo& a, const Stereo& b)
{
    Stereo out(a.left.size());
    for (std::size_t i = 0; i < a.left.size(); ++i)
    {
        out.left[i] = a.left[i] + (i < b.left.size() ? b.left[i] : 0.0);
        out.right[i] = a.right[i] + (i < b.right.size() ? b.right[i] : 0.0);
    }
    return out;
}

/** Amplitude of `frequency` in `x[begin, end)` (Goertzel). */
inline double amplitudeAt(const std::vector<double>& x, double frequency, std::size_t begin, std::size_t end)
{
    const double w = 2.0 * kTestPi * frequency / kRate;
    const double coefficient = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    end = std::min(end, x.size());
    for (std::size_t i = begin; i < end; ++i)
    {
        const double s0 = x[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / static_cast<double>(std::max<std::size_t>(1, end - begin));
}

inline double rmsOf(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    double total = 0.0;
    end = std::min(end, x.size());
    for (std::size_t i = begin; i < end; ++i)
        total += x[i] * x[i];
    return std::sqrt(total / static_cast<double>(std::max<std::size_t>(1, end - begin)));
}

inline double peakOf(const std::vector<double>& x, std::size_t begin = 0, std::size_t end = static_cast<std::size_t>(-1))
{
    double result = 0.0;
    for (std::size_t i = begin; i < std::min(end, x.size()); ++i)
        result = std::max(result, std::abs(x[i]));
    return result;
}

inline bool allFinite(const Stereo& s)
{
    for (std::size_t i = 0; i < s.left.size(); ++i)
        if (!std::isfinite(s.left[i]) || !std::isfinite(s.right[i]))
            return false;
    return true;
}

inline double toDb(double gain)
{
    return 20.0 * std::log10(std::max(gain, 1.0e-12));
}

/** Seconds to samples. */
inline std::size_t at(double seconds)
{
    return static_cast<std::size_t>(seconds * kRate);
}

/** A playing transport at a tempo, which the run below moves along block by block. */
struct TestPlayHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }
    PositionInfo info;
};

/** run(), with a transport playing from the start at `bpm` (or stopped). `perBlock` is called after
    each block with the samples done so far (to sample editor read-outs). */
template <typename PerBlock>
Stereo runPlaying(ProcessorNode& node, juce::AudioProcessor& processor, const Stereo& input, double bpm, bool playing,
                  PerBlock&& perBlock, const Stereo* sidechain = nullptr)
{
    TestPlayHead head;
    processor.setPlayHead(&head);
    const auto total = input.left.size();
    Stereo out(total);
    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::AudioBuffer<double> side(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t offset = 0; offset < total; offset += kBlock)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, total - offset));
        const double seconds = static_cast<double>(offset) / kRate;
        head.info.setIsPlaying(playing);
        head.info.setBpm(bpm);
        head.info.setTimeInSeconds(seconds);
        head.info.setPpqPosition(seconds * bpm / 60.0);
        head.info.setTimeSignature(juce::AudioPlayHead::TimeSignature{4, 4});
        for (int i = 0; i < n; ++i)
        {
            buffer.setSample(0, i, input.left[offset + static_cast<std::size_t>(i)]);
            buffer.setSample(1, i, input.right[offset + static_cast<std::size_t>(i)]);
            if (sidechain != nullptr)
            {
                side.setSample(0, i, sidechain->left[offset + static_cast<std::size_t>(i)]);
                side.setSample(1, i, sidechain->right[offset + static_cast<std::size_t>(i)]);
            }
        }
        node.process(buffer, n, midi, sidechain != nullptr ? &side : nullptr);
        for (int i = 0; i < n; ++i)
        {
            out.left[offset + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
            out.right[offset + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
        }
        perBlock(offset + static_cast<std::size_t>(n));
    }
    processor.setPlayHead(nullptr);
    return out;
}
} // namespace dojo::test::devices

#pragma once

#include <juce_core/juce_core.h>

#include <cmath>

/** How delays (latencies, in samples at the engine's sample rate) are written for people. */
namespace dojo::ui::latency
{

/** Milliseconds a delay of `samples` lasts at `sampleRate`. */
[[nodiscard]] inline double toMilliseconds(int samples, double sampleRate) noexcept
{
    return samples > 0 && sampleRate > 0.0 ? static_cast<double>(samples) * 1000.0 / sampleRate : 0.0;
}

/** "0 ms", "0.02 ms", "2.27 ms", "12.5 ms", "120 ms": two decimals below 10 ms, one below 100. */
[[nodiscard]] inline juce::String formatMilliseconds(int samples, double sampleRate)
{
    const double ms = toMilliseconds(samples, sampleRate);
    if (ms <= 0.0)
        return "0 ms";
    if (ms < 10.0)
        return juce::String(ms, 2) + " ms";
    if (ms < 100.0)
        return juce::String(ms, 1) + " ms";
    return juce::String(static_cast<int>(std::lround(ms))) + " ms";
}

/** "1 sample", "349 samples". */
[[nodiscard]] inline juce::String formatSamples(int samples)
{
    return juce::String(samples) + (samples == 1 ? " sample" : " samples");
}

/** "48 kHz", "44.1 kHz". */
[[nodiscard]] inline juce::String formatRate(double sampleRate)
{
    const double khz = sampleRate / 1000.0;
    return (std::abs(khz - std::round(khz)) < 0.05 ? juce::String(static_cast<int>(std::lround(khz))) : juce::String(khz, 1)) + " kHz";
}

} // namespace dojo::ui::latency

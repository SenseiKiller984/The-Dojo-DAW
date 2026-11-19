#include "ui/devices/SpectrumAnalysis.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui::spectrum
{

std::vector<double> logFrequencies(double low, double high, int count)
{
    std::vector<double> frequencies(static_cast<std::size_t>(std::max(2, count)));
    const double ratio = std::log(high / low);
    for (std::size_t i = 0; i < frequencies.size(); ++i)
        frequencies[i] = low * std::exp(ratio * static_cast<double>(i) / static_cast<double>(frequencies.size() - 1));
    return frequencies;
}

Analyser::Analyser(int order)
    : size_(1 << order)
    , fft_(order)
    , buffer_(static_cast<std::size_t>(size_) * 2, 0.0f)
{
}

void Analyser::analyse(const float* samples, double sampleRate, const std::vector<double>& frequencies, std::vector<float>& levels,
                       float rise, float fall)
{
    std::fill(buffer_.begin(), buffer_.end(), 0.0f);
    for (int i = 0; i < size_; ++i)
    {
        const float window = 0.5f - 0.5f * std::cos(2.0f * juce::MathConstants<float>::pi * static_cast<float>(i) / static_cast<float>(size_ - 1));
        buffer_[static_cast<std::size_t>(i)] = samples[i] * window;
    }
    fft_.performFrequencyOnlyForwardTransform(buffer_.data());

    // A full-scale sine at a bin's centre reads size / 4 there (the Hann window halves it).
    const float reference = static_cast<float>(size_) * 0.25f;
    const int bins = size_ / 2;
    const double binWidth = sampleRate / static_cast<double>(size_);
    const auto levelOf = [&](float magnitude) { return juce::Decibels::gainToDecibels(magnitude / reference, kFloorDb); };
    const bool fresh = levels.size() != frequencies.size();
    if (fresh)
        levels.assign(frequencies.size(), kFloorDb);

    for (std::size_t i = 0; i < frequencies.size(); ++i)
    {
        // The stretch this point stands for: halfway to its neighbours.
        const double low = i > 0 ? std::sqrt(frequencies[i - 1] * frequencies[i]) : frequencies[i];
        const double high = i + 1 < frequencies.size() ? std::sqrt(frequencies[i] * frequencies[i + 1]) : frequencies[i];
        const int first = std::clamp(static_cast<int>(std::ceil(low / binWidth)), 1, bins - 1);
        const int last = std::clamp(static_cast<int>(std::floor(high / binWidth)), 1, bins - 1);
        float magnitude = 0.0f;
        if (last >= first)
            for (int b = first; b <= last; ++b)
                magnitude = std::max(magnitude, buffer_[static_cast<std::size_t>(b)]);
        else
        {
            const double position = std::clamp(frequencies[i] / binWidth, 1.0, static_cast<double>(bins - 2));
            const int below = static_cast<int>(position);
            const auto t = static_cast<float>(position - static_cast<double>(below));
            magnitude = buffer_[static_cast<std::size_t>(below)] * (1.0f - t) + buffer_[static_cast<std::size_t>(below + 1)] * t;
        }
        const float level = levelOf(magnitude);
        float& shown = levels[i];
        shown = fresh ? level : shown + (level - shown) * (level > shown ? rise : fall);
    }
}

std::vector<float> masking(const std::vector<float>& own, const std::vector<float>& other, float floorDb)
{
    std::vector<float> amount(std::min(own.size(), other.size()), 0.0f);
    for (std::size_t i = 0; i < amount.size(); ++i)
        if (own[i] > floorDb && other[i] > floorDb)
            amount[i] = std::clamp((other[i] - own[i] + 6.0f) / 12.0f, 0.0f, 1.0f);
    return amount;
}

std::vector<Range> maskedRanges(const std::vector<double>& frequencies, const std::vector<float>& amount, float threshold)
{
    std::vector<Range> ranges;
    const std::size_t count = std::min(frequencies.size(), amount.size());
    for (std::size_t i = 0; i < count;)
    {
        if (amount[i] < threshold)
        {
            ++i;
            continue;
        }
        Range range;
        range.low = frequencies[i];
        while (i < count && amount[i] >= threshold)
        {
            range.amount += amount[i];
            range.high = frequencies[i];
            ++i;
        }
        ranges.push_back(range);
    }
    std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.amount > b.amount; });
    return ranges;
}

} // namespace dojo::ui::spectrum

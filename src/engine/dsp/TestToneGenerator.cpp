#include "engine/dsp/TestToneGenerator.h"

#include <cmath>

namespace dojo::engine::dsp
{
namespace
{
constexpr double kTwoPi = 6.283185307179586476925286766559;
constexpr double kRampSeconds = 0.020;
} // namespace

void TestToneGenerator::prepare(double sampleRate) noexcept
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    phaseIncrement_ = kTwoPi * frequencyHz_ / sampleRate_;
    gain_.reset(sampleRate_, kRampSeconds);
    gain_.setCurrentAndTargetValue(enabled_ ? targetGain_ : 0.0);
}

void TestToneGenerator::setParameters(bool enabled, double frequencyHz, double linearGain) noexcept
{
    enabled_ = enabled;
    frequencyHz_ = juce::jlimit(1.0, sampleRate_ * 0.45, frequencyHz);
    targetGain_ = juce::jlimit(0.0, 1.0, linearGain);
    phaseIncrement_ = kTwoPi * frequencyHz_ / sampleRate_;
    gain_.setTargetValue(enabled_ ? targetGain_ : 0.0);
}

bool TestToneGenerator::isActive() const noexcept
{
    return enabled_ || gain_.isSmoothing();
}

void TestToneGenerator::addTo(juce::AudioBuffer<Sample>& buffer, int numSamples, int left, int right) noexcept
{
    if (!isActive())
        return;

    auto* const* channels = buffer.getArrayOfWritePointers();
    const int numChannels = buffer.getNumChannels();
    Sample* const first = left >= 0 && left < numChannels ? channels[left] : nullptr;
    Sample* const second = right >= 0 && right < numChannels && right != left ? channels[right] : nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const double value = std::sin(phase_) * gain_.getNextValue();

        if (first != nullptr)
            first[i] += value;
        if (second != nullptr)
            second[i] += value;

        phase_ += phaseIncrement_;
        if (phase_ >= kTwoPi)
            phase_ -= kTwoPi;
    }

    if (!enabled_ && !gain_.isSmoothing())
        phase_ = 0.0; // fully faded out: restart cleanly next time
}

} // namespace dojo::engine::dsp

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

void TestToneGenerator::addTo(juce::AudioBuffer<Sample>& buffer, int numSamples) noexcept
{
    if (!isActive())
        return;

    const int numChannels = buffer.getNumChannels();
    auto* const* channels = buffer.getArrayOfWritePointers();

    for (int i = 0; i < numSamples; ++i)
    {
        const double value = std::sin(phase_) * gain_.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][i] += value;

        phase_ += phaseIncrement_;
        if (phase_ >= kTwoPi)
            phase_ -= kTwoPi;
    }

    if (!enabled_ && !gain_.isSmoothing())
        phase_ = 0.0; // fully faded out: restart cleanly next time
}

} // namespace dojo::engine::dsp

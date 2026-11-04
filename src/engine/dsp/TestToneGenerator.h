#pragma once

#include "core/Types.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace dojo::engine::dsp
{

/**
    A click-free sine generator used to verify the device path end to end.

    Computed in double precision; enabling/disabling ramps the amplitude over 20 ms so that toggling
    it never produces a discontinuity. Real-time safe: no allocation after prepare().
*/
class TestToneGenerator final
{
public:
    void prepare(double sampleRate) noexcept;

    /** Audio thread. */
    void setParameters(bool enabled, double frequencyHz, double linearGain) noexcept;

    /** True while the generator is audible or fading out. */
    [[nodiscard]] bool isActive() const noexcept;

    /** Audio thread: adds `numSamples` of the tone to every channel of `buffer`, from sample 0. */
    void addTo(juce::AudioBuffer<Sample>& buffer, int numSamples) noexcept;

private:
    double sampleRate_ = 48000.0;
    double phase_ = 0.0;
    double phaseIncrement_ = 0.0;
    double frequencyHz_ = 440.0;
    double targetGain_ = 0.0;
    bool enabled_ = false;
    juce::SmoothedValue<double, juce::ValueSmoothingTypes::Linear> gain_;
};

} // namespace dojo::engine::dsp

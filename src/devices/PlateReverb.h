#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"

#include <array>

namespace dojo::devices
{

/**
    Plate Reverb: the dense, bright, smooth reverb of a studio plate (a large steel sheet set into
    vibration), after Jon Dattorro's classic plate algorithm.

    - The input (mono sum) passes a pre-delay, a Bandwidth low-pass and four diffusers, then a
      "figure-eight" tank: two cross-coupled branches of a modulated all-pass, a delay, Damping
      (a low-pass), a second all-pass and another delay. Fourteen taps across the tank make the
      stereo output.
    - Decay: the reverb time (to -60 dB). Size scales the plate (denser and shorter echoes when
      smaller). Diffusion: how quickly the echoes blur into a wash. Modulation: a slow wobble of
      the tank that keeps long tails from ringing metallically.
    - Low Cut thins the reverb's low end; Width; Dry/Wet.
    - Short decays with little pre-delay give the classic short vocal plate that glues a sound
      without pushing it back.
*/
class PlateReverb final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:plate";
    static constexpr double kReferenceRate = 29761.0; ///< the rate the algorithm's delays are given at
    static constexpr double kMaxSize = 1.5;
    static constexpr double kMaxPreDelayMs = 200.0;

    PlateReverb();
    double getTailLengthSeconds() const override;

    /** The tank's feedback gain (per pass of a decay stage) for the current Decay and Size. */
    [[nodiscard]] double getDecayGain() const noexcept;

    juce::AudioParameterFloat* preDelay_ = nullptr;
    juce::AudioParameterFloat* decay_ = nullptr;
    juce::AudioParameterFloat* size_ = nullptr;
    juce::AudioParameterFloat* damping_ = nullptr;
    juce::AudioParameterFloat* bandwidth_ = nullptr;
    juce::AudioParameterFloat* lowCut_ = nullptr;
    juce::AudioParameterFloat* diffusion_ = nullptr;
    juce::AudioParameterFloat* modulation_ = nullptr;
    juce::AudioParameterFloat* width_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    /** A delay line read at a (fractional) number of samples behind its input. */
    using Line = dsp::DelayLine;

    /** A Schroeder all-pass built on a delay line. */
    static double allPass(Line& line, double delay, double coefficient, double x) noexcept
    {
        const double delayed = line.read(delay - 1.0);
        const double v = x - coefficient * delayed;
        line.push(v);
        return delayed + coefficient * v;
    }

    double scale_ = 1.0; ///< samples per reference sample
    Line preDelayLine_;
    std::array<Line, 4> diffusers_;
    std::array<Line, 2> modulated_, firstDelay_, secondAllPass_, secondDelay_; ///< the tank's two branches
    std::array<double, 2> dampingState_{}, tankOut_{};
    double bandwidthState_ = 0.0;
    double lfoPhase_ = 0.0;
    std::array<dsp::Biquad, 2> lowCutFilter_;
};

} // namespace dojo::devices

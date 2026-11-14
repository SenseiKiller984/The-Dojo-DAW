#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"

#include <array>

namespace dojo::devices
{

/**
    Utility: the mixing engineer's multi-tool. In signal-flow order:

    - DC filter     removes sub-audible DC offset (a 5 Hz high-pass) that eats headroom
    - Channel mode  Stereo, Left (left on both sides), Right, or Swap (flip left and right)
    - Phase L / R   flips the polarity of either channel (phase problems between layered tracks
                    or microphones)
    - Width         0 % (mono) .. 100 % (unchanged) .. 200 % (extra wide); Mono sums to mono
                    regardless of Width, for checking how a mix translates to mono systems
    - Bass Mono     everything below the crossover (50..500 Hz) is made mono, the highs keep their
                    width (Linkwitz-Riley crossover: the mid channel passes unchanged in level)
    - Gain          -35 .. +35 dB, smoothed: gain staging and volume automation that leaves the
                    track fader free
    - Balance       stereo placement: the opposite side is attenuated
    - Mute          silences the output (with a short fade, no click)
*/
class Utility final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:utility";

    Utility();

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void updateCrossover() noexcept;

    juce::AudioParameterFloat* gain_ = nullptr;
    juce::AudioParameterFloat* width_ = nullptr;
    juce::AudioParameterBool* mono_ = nullptr;
    juce::AudioParameterBool* bassMono_ = nullptr;
    juce::AudioParameterFloat* bassFrequency_ = nullptr;
    juce::AudioParameterBool* phaseLeft_ = nullptr;
    juce::AudioParameterBool* phaseRight_ = nullptr;
    juce::AudioParameterChoice* channel_ = nullptr;
    juce::AudioParameterFloat* balance_ = nullptr;
    juce::AudioParameterBool* mute_ = nullptr;
    juce::AudioParameterBool* dc_ = nullptr;

    // Smoothed (one-pole) gains, per sample.
    double gainLeft_ = 1.0, gainRight_ = 1.0, widthValue_ = 1.0, muteGain_ = 1.0;
    double smoothing_ = 0.001;
    double muteStep_ = 0.002;
    std::array<double, 2> dcState_{};
    std::array<double, 2> dcInput_{};
    double dcCoefficient_ = 0.999;

    // Linkwitz-Riley (2 x Butterworth) crossover for Bass Mono: low and high part of mid and side.
    dsp::BiquadCoefficients lowPass_, highPass_;
    std::array<dsp::Biquad, 2> midLow_, midHigh_, sideHigh_;
    float crossoverFor_ = -1.0f;
};

} // namespace dojo::devices

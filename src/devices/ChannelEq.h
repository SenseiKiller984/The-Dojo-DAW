#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"

#include <array>

namespace dojo::devices
{

/**
    Channel EQ: a simple, musical equaliser in the style of a mixing console's channel strip.

    - Low Cut: a 12 dB/oct high-pass at 80 Hz (rumble, handling noise).
    - Low: a shelf at 100 Hz, +-15 dB.
    - Mid: a bell sweepable from 120 Hz to 7.5 kHz, +-12 dB, with the proportional Q of console EQs:
      broad and gentle for small moves, narrower as the boost or cut grows.
    - High: a shelf at 8 kHz, +-15 dB.
    - Output gain.
*/
class ChannelEq final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:channeleq";
    static constexpr double kLowCutHz = 80.0;
    static constexpr double kLowShelfHz = 100.0;
    static constexpr double kHighShelfHz = 8000.0;

    ChannelEq();

    /** The response (dB) at `frequency` (for the editor's curve). */
    [[nodiscard]] double getResponseDb(double frequency) const noexcept;
    /** The mid band's Q for a gain (dB). */
    [[nodiscard]] static double midQ(double gainDb) noexcept;

    juce::AudioParameterBool* lowCut_ = nullptr;
    juce::AudioParameterFloat* low_ = nullptr;
    juce::AudioParameterFloat* midFrequency_ = nullptr;
    juce::AudioParameterFloat* mid_ = nullptr;
    juce::AudioParameterFloat* high_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Coefficients
    {
        dsp::BiquadCoefficients lowCut, low, mid, high;
    };
    [[nodiscard]] Coefficients makeCoefficients(double sampleRate) const noexcept;

    std::array<std::array<dsp::Biquad, 4>, 2> filters_; ///< [channel][band]
};

} // namespace dojo::devices

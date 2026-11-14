#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"

#include <array>
#include <memory>
#include <vector>

namespace juce::dsp
{
template <typename SampleType>
class Oversampling;
}

namespace dojo::devices
{

/**
    Amp & Cabinet: guitar and bass amplifier simulation with speaker cabinets.

    Amp models (Clean, Boost, Blues, Rock, Lead, Heavy, Bass) differ in input tightness, the number
    and asymmetry of their gain stages, their gain range and where the mid control sits. The
    preamp runs 4x oversampled (no aliasing fizz at high gain), followed by a passive-style tone
    stack (Bass, Middle, Treble), Presence and a softly saturating power stage.

    Cabinets (1x12, 2x12, 4x12, 4x10 Bass, 1x15 Bass, or Off) model the speakers' low-end
    resonance, cone break-up peaks and steep high roll-off; the microphone can be close on-axis
    (bright), close off-axis (darker) or far (looser, with an early room reflection).

    Reports its (small) oversampling latency for delay compensation; the dry signal is delayed to
    match, so Dry/Wet mixes without comb filtering.
*/
class AmpCabinet final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:ampcab";
    static constexpr int kMaxCabinetFilters = 8;

    AmpCabinet();
    ~AmpCabinet() override;

    static const juce::StringArray& ampNames();
    static const juce::StringArray& cabinetNames();

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void updateFilters() noexcept;

    juce::AudioParameterChoice* amp_ = nullptr;
    juce::AudioParameterFloat* gain_ = nullptr;
    juce::AudioParameterFloat* bass_ = nullptr;
    juce::AudioParameterFloat* middle_ = nullptr;
    juce::AudioParameterFloat* treble_ = nullptr;
    juce::AudioParameterFloat* presence_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;
    juce::AudioParameterChoice* cabinet_ = nullptr;
    juce::AudioParameterChoice* microphone_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

    std::unique_ptr<juce::dsp::Oversampling<double>> oversampling_;
    int latency_ = 0;

    // Base-rate filters per channel.
    dsp::BiquadCoefficients inputHighPass_;
    std::array<dsp::Biquad, 2> inputFilter_;
    std::array<dsp::BiquadCoefficients, 4> tone_; ///< bass, middle, treble, presence
    std::array<std::array<dsp::Biquad, 4>, 2> toneFilters_;
    std::array<dsp::BiquadCoefficients, kMaxCabinetFilters> cabinetCoefficients_;
    int numCabinetFilters_ = 0;
    std::array<std::array<dsp::Biquad, kMaxCabinetFilters>, 2> cabinetFilters_;
    std::array<double, 2> interstage_{}; ///< one-pole low-pass state between gain stages (oversampled)
    std::array<double, 2> interstage2_{};
    double interstageCoefficient_ = 0.5;
    std::array<double, 2> dcIn_{}, dcOut_{};
    double dcCoefficient_ = 0.999;

    // Early reflection for the far microphone.
    std::array<std::vector<double>, 2> room_;
    int roomWrite_ = 0;
    int roomDelay_ = 1;

    // Dry path, delayed by the oversampling latency.
    std::array<std::vector<double>, 2> dryDelay_;
    int dryWrite_ = 0;

    double driveGain_ = 1.0;
    double stageGain_ = 1.0;
    double bias_ = 0.0;
    int stages_ = 2;
    double outputTrim_ = 1.0;
    int shownAmp_ = -1, shownCabinet_ = -1, shownMicrophone_ = -1;
    std::array<float, 5> shownTone_{-1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
};

} // namespace dojo::devices

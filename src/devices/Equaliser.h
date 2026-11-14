#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Four-band parametric EQ with a spectrum analyser.

    Each band is a biquad whose shape is selectable (low cut, low shelf, peak, high shelf, high cut)
    with frequency, gain and Q. Parameter changes are smoothed and coefficients recomputed every
    32 samples, so sweeping a band never zippers. The post-EQ signal (mono sum) is published to a
    lock-free ring for the UI's analyser.
*/
class Equaliser final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:eq";
    static constexpr int kNumBands = 4;
    static constexpr int kAnalyserSize = 4096;

    Equaliser();

    struct BandParameters
    {
        juce::AudioParameterBool* enabled = nullptr;
        juce::AudioParameterChoice* shape = nullptr;
        juce::AudioParameterFloat* frequency = nullptr;
        juce::AudioParameterFloat* gain = nullptr;
        juce::AudioParameterFloat* q = nullptr;
    };

    [[nodiscard]] const BandParameters& getBand(int band) const { return bands_[static_cast<std::size_t>(band)]; }
    [[nodiscard]] juce::AudioParameterFloat& getOutputGain() const { return *output_; }

    /** Message thread: the combined magnitude response (linear) at `frequency`, from the current
        parameter values. */
    [[nodiscard]] double getMagnitudeAt(double frequency) const;
    /** Message thread: one band's magnitude response (linear) at `frequency`. */
    [[nodiscard]] double getBandMagnitudeAt(int band, double frequency) const;

    /** Message thread: copies the newest `count` analyser samples (oldest first). */
    void copyAnalyserSamples(float* destination, int count) const noexcept;

    double getTailLengthSeconds() const override { return 0.05; }

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct BandState
    {
        double frequency = 1000.0;
        double gain = 0.0;
        double q = 0.7;
        int shape = 2;
        bool enabled = true;
        bool initialised = false;
        dsp::BiquadCoefficients coefficients;
        std::array<dsp::Biquad, 2> filters;
    };

    static dsp::BiquadCoefficients::Shape shapeOf(int index) noexcept;
    void updateCoefficients(bool snap) noexcept;

    std::array<BandParameters, kNumBands> bands_;
    juce::AudioParameterFloat* output_ = nullptr;
    std::array<BandState, kNumBands> state_;
    double outputGain_ = 1.0;
    double smoothing_ = 0.5;

    std::array<std::atomic<float>, kAnalyserSize> analyser_{};
    std::atomic<std::uint32_t> analyserWrite_{0};
};

} // namespace dojo::devices

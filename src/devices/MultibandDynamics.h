#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Modulation.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Multiband Dynamics: splits the signal into low, mid and high bands (fourth-order Linkwitz-Riley
    crossovers, so the bands add back up flat) and shapes each band's dynamics on its own.

    Every band has two thresholds:
    - Above: levels over it are scaled by the ratio. A ratio over 1:1 compresses downward (louder
      parts brought down); under 1:1 expands upward (peaks made bigger).
    - Below: levels under it are scaled by the ratio. Over 1:1 expands downward (quiet parts pushed
      further down, gate-like); under 1:1 compresses upward (quiet details brought up).
    Then the band's gain, solo and on/off. Attack, release, Peak/RMS detection, a soft knee, Amount
    (how much of the processing is applied), output gain and dry/wet act on all bands.
*/
class MultibandDynamics final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:multiband";
    static constexpr int kBands = 3;
    static constexpr double kMaxUpwardDb = 24.0;
    static constexpr double kMaxDownwardDb = -80.0;

    MultibandDynamics();

    struct Band
    {
        juce::AudioParameterBool* on = nullptr;
        juce::AudioParameterFloat* aboveThreshold = nullptr;
        juce::AudioParameterFloat* aboveRatio = nullptr;
        juce::AudioParameterFloat* belowThreshold = nullptr;
        juce::AudioParameterFloat* belowRatio = nullptr;
        juce::AudioParameterFloat* gain = nullptr;
        juce::AudioParameterBool* solo = nullptr;
    };

    /** Gain change (dB) for a level (dB), from the two thresholds and ratios. */
    [[nodiscard]] static double gainComputer(double levelDb, double aboveThreshold, double aboveRatio, double belowThreshold,
                                             double belowRatio, bool softKnee, double amount) noexcept;
    /** Band `band`'s static curve: output level for an input level (dB), with its gain. */
    [[nodiscard]] double getBandOutputDb(int band, double inputDb) const noexcept;

    /** Editor: each band's detected level and current gain change (dB). */
    [[nodiscard]] float getBandLevelDb(int band) const noexcept { return levels_[static_cast<std::size_t>(band)].load(std::memory_order_relaxed); }
    [[nodiscard]] float getBandGainDb(int band) const noexcept { return gains_[static_cast<std::size_t>(band)].load(std::memory_order_relaxed); }

    std::array<Band, kBands> bands_;
    juce::AudioParameterFloat* crossLow_ = nullptr;
    juce::AudioParameterFloat* crossHigh_ = nullptr;
    juce::AudioParameterFloat* attack_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterChoice* detection_ = nullptr; ///< Peak, RMS
    juce::AudioParameterBool* softKnee_ = nullptr;
    juce::AudioParameterFloat* amount_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<dsp::LinkwitzRiley, 2> lowSplit_, highSplit_, lowAlign_; ///< per channel
    std::array<double, kBands> envelope_{};
    std::array<std::atomic<float>, kBands> levels_{};
    std::array<std::atomic<float>, kBands> gains_{};
};

} // namespace dojo::devices

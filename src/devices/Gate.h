#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Gate: mutes audio that falls below a threshold (background noise between vocal phrases, spill
    between drum hits).

    - Threshold opens the gate; Return is the hysteresis: it closes only once the level falls
      Return dB below the threshold, so a signal hovering at the threshold does not chatter.
    - Attack (how fast it opens), Hold (how long it stays open after the signal drops), Release
      (how fast it closes), Floor (how far it closes: -80 dB is silence, higher only attenuates).
    - Flip reverses it: only what is below the threshold passes (ducks the loud parts).
    - Lookahead delays the audio (reported as latency) so the gate is already open when a transient
      arrives: drum attacks keep their snap.
    - The detector can listen to the sidechain input instead (gate one track by another), through a
      high-pass and low-pass key filter (e.g. open only on the kick); Listen plays what it hears.
*/
class Gate final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:gate";
    static constexpr double kMaxLookaheadMs = 10.0;
    static constexpr double kDetectorDecaySeconds = 0.03;

    Gate();
    ~Gate() override;

    /** Editor: the loudest detector level (dB) since the last call. */
    [[nodiscard]] float takeDetectorPeakDb() noexcept { return detectorPeak_.exchange(-100.0f, std::memory_order_acq_rel); }
    /** How open the gate is now (0 closed .. 1 open; flipped gates report their gain the same way). */
    [[nodiscard]] float getOpenness() const noexcept { return openness_.load(std::memory_order_relaxed); }
    [[nodiscard]] static double lookaheadMs(int choice) noexcept;

    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterFloat* return_ = nullptr;
    juce::AudioParameterFloat* attack_ = nullptr;
    juce::AudioParameterFloat* hold_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterFloat* floor_ = nullptr;
    juce::AudioParameterBool* flip_ = nullptr;
    juce::AudioParameterChoice* lookahead_ = nullptr;
    juce::AudioParameterBool* external_ = nullptr;
    juce::AudioParameterFloat* keyLow_ = nullptr;
    juce::AudioParameterFloat* keyHigh_ = nullptr;
    juce::AudioParameterBool* listen_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int lookaheadSamples() const noexcept;

    std::array<dsp::DelayLine, 2> delay_;
    std::array<dsp::Biquad, 2> keyHighPass_, keyLowPass_;
    bool open_ = false;
    int holdCounter_ = 0;
    double envelope_ = 0.0;
    double detector_ = 0.0;
    std::atomic<float> detectorPeak_{-100.0f};
    std::atomic<float> openness_{0.0f};
};

} // namespace dojo::devices

#pragma once

#include "devices/BuiltinProcessor.h"

#include <atomic>
#include <vector>

namespace dojo::devices
{

/**
    Feed-forward compressor with soft knee, makeup gain, dry/wet mix and an external sidechain.

    The detector follows the louder channel (stereo-linked). With "External Sidechain" on, it listens
    to the sidechain bus instead of the input — route any track to it for ducking.
*/
class Compressor final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:compressor";

    Compressor();

    /** Message thread: the largest gain reduction (dB, >= 0) since the last call. */
    [[nodiscard]] float takeGainReductionDb() noexcept { return gainReduction_.exchange(0.0f, std::memory_order_acq_rel); }

    /** Static curve: output level (dB) for an input level (dB), for drawing. */
    [[nodiscard]] double getOutputLevelDb(double inputDb) const;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    static double gainComputer(double levelDb, double threshold, double ratio, double knee) noexcept;

    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterFloat* ratio_ = nullptr;
    juce::AudioParameterFloat* attack_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterFloat* knee_ = nullptr;
    juce::AudioParameterFloat* makeup_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;
    juce::AudioParameterBool* external_ = nullptr;

    double envelopeDb_ = 0.0;
    std::atomic<float> gainReduction_{0.0f};
};

/**
    Brick-wall lookahead limiter.

    The gain needed to keep each sample under the ceiling is held over the lookahead window and then
    averaged over it, so the gain is already down when a peak arrives — no overshoot and no click —
    and the audio is delayed by the lookahead (reported as latency, so the engine compensates).
    Release is a one-pole recovery.
*/
class Limiter final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:limiter";
    static constexpr double kMaxLookaheadMs = 10.0;

    Limiter();
    ~Limiter() override;

    [[nodiscard]] float takeGainReductionDb() noexcept { return gainReduction_.exchange(0.0f, std::memory_order_acq_rel); }

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int lookaheadSamples() const noexcept;
    void resetState(int lookahead) noexcept;

    juce::AudioParameterFloat* input_ = nullptr;
    juce::AudioParameterFloat* ceiling_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterFloat* lookahead_ = nullptr;

    int capacity_ = 1;
    int lookahead_samples_ = 0;
    std::vector<double> delay_[2];
    int delayIndex_ = 0;

    // Sliding minimum of the required gain over lookahead + 1 samples (monotonic deque in a ring).
    std::vector<double> required_;
    std::vector<int> dequeIndex_;
    int dequeHead_ = 0;
    int dequeTail_ = 0;
    long long sampleCounter_ = 0;

    // Moving average over the lookahead.
    std::vector<double> averageRing_;
    int averageIndex_ = 0;
    double averageSum_ = 0.0;

    double gain_ = 1.0;
    std::atomic<float> gainReduction_{0.0f};
};

} // namespace dojo::devices

#pragma once

#include "devices/BuiltinProcessor.h"

#include <atomic>
#include <array>
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
    Lookahead brickwall limiter.

    - Input Gain drives the signal into the limiter; with Maximize on, Threshold replaces it: material
      at the threshold is raised to the ceiling and everything above is limited, so lowering the
      threshold reduces the dynamic range and makes the track louder.
    - Ceiling: the output never exceeds it (sample peaks; with True Peak also the peaks between samples).
    - Release, or Auto release: a fast release for short peaks and a slow one that only sustained
      limiting engages, so transients recover at once while dense passages do not pump.
    - Lookahead (reported as latency): the gain ramps down before a peak arrives, so nothing is clipped.
    - Stereo applies the same gain reduction to both channels; L/R limits each on its own peaks, Link
      blending the two (100 % = Stereo).
    - Modes: Standard; Soft Clip (peaks up to 6 dB above the ceiling are rounded off by a saturation
      curve instead of gain-reduced: warmer, crunchier); True Peak (the detector looks at an 8x
      oversampled signal, so the peaks between samples stay below the ceiling too; 8 samples more
      latency).
*/
class Limiter final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:limiter";
    static constexpr double kMaxLookaheadMs = 10.0;
    static constexpr int kTruePeakOversampling = 8;           ///< points per sample the true-peak detector checks
    static constexpr int kTruePeakTaps = 16;                  ///< taps per point of its interpolator
    static constexpr int kTruePeakDelay = kTruePeakTaps / 2;  ///< samples the true-peak detector lags
    static constexpr double kTruePeakMarginDb = 0.05;         ///< True Peak aims this far under the ceiling
    static constexpr double kAutoFastReleaseMs = 20.0;        ///< Auto release: the fast path
    static constexpr double kAutoSlowAttackMs = 200.0;        ///< how long limiting must last to engage the slow path
    static constexpr double kAutoSlowReleaseMs = 900.0;       ///< the slow path's release
    static constexpr double kSoftClipHeadroomDb = 6.0;        ///< how far above the ceiling Soft Clip lets peaks go
    static constexpr double kSoftClipKnee = 0.5;              ///< the curve is linear below half the ceiling

    enum class Mode
    {
        standard,
        softClip,
        truePeak,
    };

    Limiter();
    ~Limiter() override;

    [[nodiscard]] float takeGainReductionDb() noexcept { return gainReduction_.exchange(0.0f, std::memory_order_acq_rel); }

    /** The Soft Clip curve: linear below kSoftClipKnee x ceiling, then rounding off towards (never
        reaching) the ceiling. */
    [[nodiscard]] static double softClip(double x, double ceiling) noexcept;

    // Parameters (for the editor).
    juce::AudioParameterBool* maximize_ = nullptr;
    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterFloat* input_ = nullptr;
    juce::AudioParameterFloat* ceiling_ = nullptr;
    juce::AudioParameterChoice* mode_ = nullptr;
    juce::AudioParameterBool* autoRelease_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterFloat* lookahead_ = nullptr;
    juce::AudioParameterChoice* channels_ = nullptr; ///< 0 = Stereo, 1 = L/R
    juce::AudioParameterFloat* link_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    /** One channel's gain: the sliding minimum of the required gain over the lookahead, ramped in
        by a moving average (it reaches the required gain as the peak arrives), then released. */
    struct GainComputer
    {
        std::vector<double> required;
        std::vector<long long> index;
        int head = 0, tail = 0;
        std::vector<double> averageRing;
        int averageIndex = 0;
        double averageSum = 0.0;
        double gain = 1.0, fast = 1.0, slow = 1.0;

        void prepare(int capacity);
        void reset(int lookahead) noexcept;
        double process(double needed, long long n, int lookahead, double release, bool autoRelease, double fastRelease,
                       double slowAttack, double slowRelease) noexcept;
    };

    void parameterValueChanged(int parameterIndex, float newValue) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int lookaheadSamples() const noexcept;
    [[nodiscard]] int detectorDelay() const noexcept { return mode_->getIndex() == static_cast<int>(Mode::truePeak) ? kTruePeakDelay : 0; }
    void resetState(int lookahead, int delay) noexcept;
    /** True Peak: the largest interpolated value between the samples kTruePeakDelay and
        kTruePeakDelay - 1 back. */
    [[nodiscard]] double interSamplePeak(int channel) const noexcept;

    int capacity_ = 1;
    int lookaheadSamples_ = 0;
    int detectorDelay_ = 0;
    long long sampleCounter_ = 0;
    std::array<std::vector<double>, 2> delay_;
    int delayIndex_ = 0;
    std::array<GainComputer, 2> computers_;

    // True Peak: the last kTruePeakTaps input samples per channel, and the interpolator's
    // coefficients for the three positions between samples.
    std::array<std::array<double, kTruePeakTaps>, 2> history_{};
    int historyIndex_ = 0;
    std::array<double, 2> previousInterSamplePeak_{};
    std::array<std::array<double, kTruePeakTaps>, kTruePeakOversampling - 1> interpolator_{};

    std::atomic<float> gainReduction_{0.0f};
};

} // namespace dojo::devices

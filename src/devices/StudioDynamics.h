#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"
#include "devices/dsp/Oversampler.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/** Stores the larger of `value` and what is there (for meters read on the message thread). */
inline void storeMax(std::atomic<float>& target, float value) noexcept
{
    float previous = target.load(std::memory_order_relaxed);
    while (value > previous && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed))
    {
    }
}

/**
    De-Esser: tames sibilance ("s", "sh", "t" sounds) and other harsh, spiky highs.

    - The detector listens only above Frequency (High-Pass) or around it (Band-Pass). When what it
      hears goes over the Threshold, the gain comes down (4:1, with a 3 dB soft knee), at most by
      Range dB. It reacts in half a millisecond and lets go over Release.
    - Split: only the highs above Frequency are turned down (the body of the voice is untouched).
      Wide: the whole signal is turned down while the sibilance lasts (more natural on some voices).
    - Listen plays what the detector hears, to find the sibilance's frequency.
    - Quiet highs (the normal air of a voice) stay below the threshold and are left alone.
*/
class DeEsser final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:deesser";
    static constexpr double kRatio = 4.0;
    static constexpr double kKneeDb = 3.0;
    static constexpr double kAttackSeconds = 0.0005;

    DeEsser();

    /** Message thread: the largest gain reduction (dB, >= 0) and detector level (dB) since the last call. */
    [[nodiscard]] float takeReductionDb() noexcept { return reduction_.exchange(0.0f, std::memory_order_acq_rel); }
    [[nodiscard]] float takeDetectorDb() noexcept { return detector_.exchange(-100.0f, std::memory_order_acq_rel); }
    /** The reduction (dB, >= 0) for a detector level (dB), for drawing and tests. */
    [[nodiscard]] double reductionFor(double levelDb) const noexcept;

    juce::AudioParameterFloat* frequency_ = nullptr;
    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterFloat* range_ = nullptr;
    juce::AudioParameterChoice* mode_ = nullptr;      ///< Split, Wide
    juce::AudioParameterChoice* detection_ = nullptr; ///< High-Pass, Band-Pass
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterBool* listen_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<std::array<dsp::Biquad, 2>, 2> detectFilter_; ///< per channel, two stages
    std::array<dsp::LinkwitzRiley, 2> split_;
    double envelope_ = 0.0;
    double smoothedFrequency_ = 0.0;
    std::atomic<float> reduction_{0.0f};
    std::atomic<float> detector_{-100.0f};
};

/**
    Glue Compressor: a bus compressor in the style of the classic console centre-section compressors,
    for gluing drums, groups and whole mixes together.

    - Fixed ratios (2:1, 4:1, 10:1), stepped attack (0.01 to 30 ms) and release (0.1 to 1.2 s, or
      Auto: program-dependent, fast after short peaks and slow once compression is sustained).
    - A soft knee, stereo-linked detection and an optional sidechain high-pass (SC High-Pass) so the
      low end does not pump the whole mix.
    - Range limits how far it compresses; Makeup brings the level back; Dry/Wet for parallel
      compression; Soft Clip rounds off peaks above about -0.5 dBFS after the makeup.
*/
class GlueCompressor final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:gluecomp";
    static constexpr double kKneeDb = 4.0;
    static constexpr double kSoftClipCeiling = 0.944; ///< -0.5 dBFS

    GlueCompressor();

    [[nodiscard]] float takeReductionDb() noexcept { return reduction_.exchange(0.0f, std::memory_order_acq_rel); }
    /** The static gain reduction (dB, >= 0) for an input level (dB), for drawing and tests. */
    [[nodiscard]] double reductionFor(double levelDb) const noexcept;
    [[nodiscard]] static double ratioOf(int choice) noexcept;
    [[nodiscard]] static double attackMsOf(int choice) noexcept;
    /** Seconds; negative for Auto. */
    [[nodiscard]] static double releaseSecondsOf(int choice) noexcept;
    [[nodiscard]] static double softClip(double x) noexcept;

    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterChoice* ratio_ = nullptr;
    juce::AudioParameterChoice* attack_ = nullptr;
    juce::AudioParameterChoice* release_ = nullptr;
    juce::AudioParameterFloat* makeup_ = nullptr;
    juce::AudioParameterFloat* range_ = nullptr;
    juce::AudioParameterFloat* scHighPass_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;
    juce::AudioParameterBool* softClip_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<dsp::Biquad, 2> scFilter_;
    double fastDb_ = 0.0; ///< gain reduction (dB) following the signal
    double slowDb_ = 0.0; ///< ... and a slow copy of it (Auto release)
    std::atomic<float> reduction_{0.0f};
};

/**
    Expander: the opposite of a compressor. Below the Threshold, quiet sound is made quieter still
    (Ratio 2:1: 1 dB under the threshold becomes 2 dB under), at most by Range dB. A gentler, more
    natural alternative to a gate for lowering room noise, spill and breaths.

    - Knee: a soft transition around the threshold.
    - Attack: how fast it opens when the sound comes back; Release: how fast it closes.
    - The detector can listen to a sidechain track instead (External Sidechain), through a key
      high-pass filter (Key High-Pass) that ignores rumble.
*/
class Expander final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:expander";

    Expander();

    [[nodiscard]] float takeReductionDb() noexcept { return reduction_.exchange(0.0f, std::memory_order_acq_rel); }
    [[nodiscard]] float takeDetectorDb() noexcept { return detector_.exchange(-100.0f, std::memory_order_acq_rel); }
    /** The static attenuation (dB, >= 0) for a detector level (dB), for drawing and tests. */
    [[nodiscard]] double attenuationFor(double levelDb) const noexcept;

    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterFloat* ratio_ = nullptr;
    juce::AudioParameterFloat* range_ = nullptr;
    juce::AudioParameterFloat* knee_ = nullptr;
    juce::AudioParameterFloat* attack_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterBool* external_ = nullptr;
    juce::AudioParameterFloat* keyLow_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<dsp::Biquad, 2> keyFilter_;
    double level_ = 0.0;         ///< detector (linear)
    double attenuationDb_ = 0.0; ///< smoothed
    std::atomic<float> reduction_{0.0f};
    std::atomic<float> detector_{-100.0f};
};

/**
    Soft Clipper: rounds off peaks so they never pass the Ceiling, for loud drums and mixes without a
    limiter's pumping.

    - Drive pushes the signal into the clipper.
    - Shape: Soft (tanh), Smooth (arctangent: gentler, the peaks approach the ceiling more slowly) or
      Hard (a straight cut). Softness sets where the rounding starts: 0 % only at the ceiling (hard),
      100 % from silence up.
    - Oversampling (2x or 4x) keeps the added harmonics from folding back as aliasing; it adds a
      little latency, which the delay compensation takes care of. The ceiling holds after it too.
    - Output trims the level after the clipper; Dry/Wet blends with the (latency-aligned) dry signal.
*/
class SoftClipper final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:softclip";

    SoftClipper();
    ~SoftClipper() override;

    /** How much the peaks were reduced (dB, >= 0) since the last call. */
    [[nodiscard]] float takeClipDb() noexcept { return clip_.exchange(0.0f, std::memory_order_acq_rel); }
    /** The curve: output for an input (both relative to full scale), with the current settings. */
    [[nodiscard]] double shape(double x) const noexcept;
    /** The curve for a shape (0 Soft, 1 Smooth, 2 Hard) and softness (0..1), at a ceiling of 1. */
    [[nodiscard]] static double unitShape(double x, int shape, double softness) noexcept;

    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* ceiling_ = nullptr;
    juce::AudioParameterChoice* shape_ = nullptr;
    juce::AudioParameterFloat* softness_ = nullptr;
    juce::AudioParameterChoice* oversampling_ = nullptr; ///< Off, 2x, 4x
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void parameterValueChanged(int, float) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int currentLatency() const noexcept;

    std::array<dsp::Oversampler, 2> oversamplers_; ///< 2x, 4x
    std::atomic<float> clip_{0.0f};
};

} // namespace dojo::devices

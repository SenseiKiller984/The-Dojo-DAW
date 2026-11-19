#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/PhaseOversampler.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Soft Clipper: rounds off peaks so they never pass the Ceiling, for loud drums and mixes without a
    limiter's pumping.

    - Input pushes the signal into the clipper; Auto Gain turns the output down by as much, so the
      clipping can be judged at the same loudness.
    - Shape: Soft (tanh), Smooth (arctangent: gentler, the peaks approach the ceiling more slowly),
      Hard (a straight cut) or Tube (a warm, rounded curve that saturates gradually). Softness sets
      where the rounding starts: 0 % only at the ceiling, 100 % from silence up.
    - Asymmetry clips the negative half earlier than the positive (up to 6 dB), adding the even
      harmonics of a single-ended tube stage (the offset it leaves is filtered out).
    - Stereo Link clips both sides by the same amount (from the louder one), so the stereo image
      stays put; off, each side clips on its own.
    - Listen: Delta plays only what the clipper removes.
    - Oversampling: separate settings for real-time use and offline renders (Off, 2x, 4x, 8x), with
      minimum, intermediate or linear phase filters; it keeps the harmonics from folding back as
      aliasing. Each setting has an integer latency, which the delay compensation takes care of.
    - Ceiling Guard holds the ceiling after the downsampling filter too (its ringing can poke a
      little over it). Off, heavy clipping with oversampling is at its cleanest (no aliasing at
      all), at the cost of peaks up to about 1 dB over the ceiling.
    - Output trims the level after the clipper; Dry/Wet blends with the (latency-aligned) dry signal.
*/
class SoftClipper final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:softclip";
    static constexpr double kMaxAsymmetryDb = 6.0;

    SoftClipper();
    ~SoftClipper() override;

    void setNonRealtime(bool isNonRealtime) noexcept override;

    /** How much the peaks were reduced (dB, >= 0) since the last call. */
    [[nodiscard]] float takeClipDb() noexcept { return clip_.exchange(0.0f, std::memory_order_acq_rel); }
    /** The curve: output for an input (both relative to full scale, before Input), with the current
        settings. */
    [[nodiscard]] double shape(double x) const noexcept;
    /** The curve for a shape (0 Soft, 1 Smooth, 2 Hard, 3 Tube) and softness (0..1), at a ceiling of 1. */
    [[nodiscard]] static double unitShape(double x, int shape, double softness) noexcept;
    /** Latency (samples) of the oversampling used now (real-time or offline). */
    [[nodiscard]] int currentLatency() const noexcept;
    /** Latency (samples) of the real-time or the offline oversampling setting. */
    [[nodiscard]] int latencyFor(bool offline) const noexcept
    {
        return oversampler_.latencyOf((offline ? osOffline_ : osRealtime_)->getIndex(), osPhase_->getIndex());
    }

    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* ceiling_ = nullptr;
    juce::AudioParameterChoice* shape_ = nullptr;
    juce::AudioParameterFloat* softness_ = nullptr;
    juce::AudioParameterFloat* asymmetry_ = nullptr;
    juce::AudioParameterBool* link_ = nullptr;
    juce::AudioParameterBool* autoGain_ = nullptr;
    juce::AudioParameterChoice* listen_ = nullptr;     ///< Normal, Delta
    juce::AudioParameterBool* guard_ = nullptr;
    juce::AudioParameterChoice* osRealtime_ = nullptr; ///< Off, 2x, 4x, 8x
    juce::AudioParameterChoice* osOffline_ = nullptr;
    juce::AudioParameterChoice* osPhase_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void parameterValueChanged(int, float) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int factorChoice() const noexcept;
    /** One sample through the curve (with asymmetry), at a ceiling of 1. */
    [[nodiscard]] static double curve(double u, int shape, double softness, double negativeCeiling) noexcept;

    dsp::PhaseOversampler oversampler_;
    std::array<double, 2> dcIn_{}, dcOut_{}; ///< DC blocker (asymmetry)
    std::atomic<float> clip_{0.0f};
};

} // namespace dojo::devices

#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/PhaseOversampler.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    The Glue: an analog-modelled bus compressor after the big British consoles of the 80's, for
    gluing drums, groups and whole mixes together with smooth, transparent compression.

    The circuit, modelled sample by sample (at the oversampled rate):
    - Feedback topology: the gain cell (VCA) turns down the main signal and the detector's input
      too, so the detector hears the compressed signal, and its level sets the gain. Each sample
      the loop is solved implicitly (Newton iterations), as a circuit simulator would, so even the
      fastest attack at 10:1 neither overshoots nor chatters.
    - The detector is a diode-based envelope follower: a capacitor charged through a diode (soft
      turn-on, no hard switch between attack and release) and discharged through the release
      resistor. Auto release adds a second, slower capacitor that holds the gain reduction once
      compression is sustained (fast after short peaks, slow on dense material).
    - Range saturates the sidechain: the top of the detected signal is clipped away, so there is a
      limit to how much compression is possible (at most Range dB). Full keeps the cleanest circuit;
      lower settings behave like older units, or creatively limit the pumping.
    - Sidechain High Pass: a soft -6 dB/oct high-pass on the detected signal (bass hits pump the
      mix less). External Sidechain: another track drives the detector (through the same loop).
    - Mix crossfades the dry and the compressed signal inside the oversampled domain, so the blend
      is phase accurate; its value reads in decibels (the level of the compressed signal).
    - Peak Clip (PEAKCLIP IN): a soft-clipping waveshaper at the output, rounding peaks just under
      0 dBFS. Oversampling keeps its harmonics from aliasing.
    - Oversampling: separate settings for real-time use and offline renders (2x and 8x by default),
      each Off (no latency, the lowest CPU), 2x, 4x or 8x, with minimum phase (the lowest latency),
      intermediate phase or linear phase filters (the highest latency, the best phase alignment of
      the upper frequencies). Every setting has an integer latency at DC, reported to the engine's
      delay compensation.
*/
class TheGlue final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:gluecomp";
    static constexpr double kKneeDb = 6.0;
    static constexpr double kRangeFull = 70.0;     ///< Range at its maximum: no sidechain saturation
    static constexpr double kPeakClipCeiling = 0.977; ///< -0.2 dBFS
    static constexpr double kPeakClipKnee = 0.7;      ///< where the rounding starts (-3.1 dBFS)

    TheGlue();
    ~TheGlue() override;

    void setNonRealtime(bool isNonRealtime) noexcept override;

    /** Message thread: the compression on the VU meter, RMS (VU ballistics) and peak, in dB (>= 0).
        The peak is the largest since the last call. */
    [[nodiscard]] float getRmsReductionDb() const noexcept { return rmsReduction_.load(std::memory_order_relaxed); }
    [[nodiscard]] float takePeakReductionDb() noexcept { return peakReduction_.exchange(0.0f, std::memory_order_acq_rel); }

    /** The gain computer: gain reduction (dB, >= 0) for a detector level (dB), with the current
        threshold and ratio (feedback: the detector hears the compressed signal). */
    [[nodiscard]] double reductionForDetector(double detectorDb) const noexcept;
    /** The static curve: output level (dB) for a steady input level (dB) (Range and the detector's
        ballistics aside), for drawing. */
    [[nodiscard]] double staticOutputDb(double inputDb) const noexcept;
    /** Whether the VU meter shows its Peak needle (a view setting, saved with the device). */
    [[nodiscard]] bool isPeakNeedleShown() const noexcept { return peakNeedle_.load(std::memory_order_relaxed); }
    void setPeakNeedleShown(bool shown) noexcept { peakNeedle_.store(shown, std::memory_order_relaxed); }
    /** Latency (samples) of the oversampling used now (real-time or offline). */
    [[nodiscard]] int currentLatency() const noexcept;
    /** Latency (samples) of the real-time or the offline oversampling setting. */
    [[nodiscard]] int latencyFor(bool offline) const noexcept
    {
        return oversampler_.latencyOf((offline ? osOffline_ : osRealtime_)->getIndex(), osPhase_->getIndex());
    }

    [[nodiscard]] static double ratioOf(int choice) noexcept;
    [[nodiscard]] static double attackMsOf(int choice) noexcept;
    /** Seconds; negative for Auto. */
    [[nodiscard]] static double releaseSecondsOf(int choice) noexcept;
    /** Mix (0..100 %) as shown: the compressed signal's level in dB. */
    [[nodiscard]] static juce::String mixText(float percent);
    [[nodiscard]] static float mixFromText(const juce::String& text);
    [[nodiscard]] static double peakClip(double x) noexcept;

    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterChoice* ratio_ = nullptr;
    juce::AudioParameterChoice* attack_ = nullptr;
    juce::AudioParameterChoice* release_ = nullptr;
    juce::AudioParameterFloat* makeup_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;
    juce::AudioParameterFloat* range_ = nullptr;
    juce::AudioParameterFloat* scHighPass_ = nullptr;
    juce::AudioParameterBool* external_ = nullptr;
    juce::AudioParameterBool* peakClip_ = nullptr;
    juce::AudioParameterChoice* osRealtime_ = nullptr; ///< Off, 2x, 4x, 8x
    juce::AudioParameterChoice* osOffline_ = nullptr;
    juce::AudioParameterChoice* osPhase_ = nullptr;    ///< Minimum Phase, Intermediate, Linear Phase

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;
    void writeExtraState(juce::XmlElement& extra) const override;
    void readExtraState(const juce::XmlElement& extra) override;

private:
    void parameterValueChanged(int, float) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int factorChoice() const noexcept;

    dsp::PhaseOversampler oversampler_;
    std::array<double, 2> hpState_{}; ///< sidechain high-pass (x[n-1], per channel)
    std::array<double, 2> hpOut_{};   ///< (y[n-1])
    std::array<double, 2> sidePrevious_{}; ///< last external sidechain sample (interpolation)
    double envelope_ = 0.0;     ///< the detector capacitor (linear level)
    double slowEnvelope_ = 0.0; ///< the Auto release's second capacitor
    double conducting_ = 0.0;   ///< seconds since the diode last conducted, counting down
    double reductionSquare_ = 0.0; ///< VU ballistics (mean square of the reduction in dB)
    std::atomic<float> rmsReduction_{0.0f};
    std::atomic<float> peakReduction_{0.0f};
    std::atomic<bool> peakNeedle_{false};
};

} // namespace dojo::devices

#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Plate Reverb: the dense, bright, smooth reverb of a studio plate (a large metal sheet set into
    vibration by a driver and picked up by contact microphones), after Jon Dattorro's classic plate
    algorithm.

    - Plate: the sheet's material. Steel is the classic studio plate; Gold Foil a smaller, denser,
      brighter foil with tight lows; Aluminium a larger, more open plate that shimmers.
    - The input (mono sum) passes Drive (the driver saturating when pushed), a pre-delay (in ms, or
      synced to the tempo), a Bandwidth low-pass and four diffusers, then a "figure-eight" tank: two
      cross-coupled branches of a modulated all-pass, a delay, Damping (a low-pass), a second
      all-pass and another delay. Fourteen taps across the tank make the stereo output.
    - Decay: the reverb time (to -60 dB). Low Decay lengthens (or shortens) it below Low Freq, for
      a warmer or a tighter tail. Size scales the plate. Diffusion: how quickly the echoes blur
      into a wash. Mod Rate and Mod Depth: a slow wobble of the tank that keeps long tails from
      ringing metallically.
    - Ducking turns the reverb down while the dry signal plays and lets it bloom in the gaps.
      Freeze holds the tail forever (no new input, no decay).
    - Low Cut and High Cut shape the reverb's tone; Width; Dry/Wet.
*/
class PlateReverb final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:plate";
    static constexpr double kReferenceRate = 29761.0; ///< the rate the algorithm's delays are given at
    static constexpr double kMaxSize = 1.5;
    static constexpr double kMaxPlateScale = 1.2;          ///< the largest plate's size multiple
    static constexpr double kMaxPreDelayMs = 250.0;
    static constexpr double kMaxSyncedPreDelayMs = 1000.0; ///< (synced to slow tempos)
    static constexpr double kFreezeGain = 0.99995;

    /** What a plate material changes. */
    struct Character
    {
        double size, diffusion, damping, modulation, lowDecay;
    };
    [[nodiscard]] static Character characterOf(int plate) noexcept;
    [[nodiscard]] static const juce::StringArray& divisionNames();
    [[nodiscard]] static double beatsForDivision(int index) noexcept;

    PlateReverb();
    double getTailLengthSeconds() const override;

    /** The tank's feedback gain (per pass of a decay stage) for a reverb time, at the current Size and Plate. */
    [[nodiscard]] double decayGainFor(double seconds) const noexcept;
    /** The tank's feedback gain for the current Decay (the mids and highs). */
    [[nodiscard]] double getDecayGain() const noexcept;
    /** The low frequencies' reverb time (s). */
    [[nodiscard]] double getLowDecaySeconds() const noexcept;
    /** The pre-delay (ms) in effect: the knob, or the synced division at the tempo. */
    [[nodiscard]] double getPreDelayMs() const noexcept;

    juce::AudioParameterChoice* plate_ = nullptr; ///< Steel, Gold Foil, Aluminium
    juce::AudioParameterFloat* preDelay_ = nullptr;
    juce::AudioParameterBool* sync_ = nullptr;
    juce::AudioParameterChoice* division_ = nullptr;
    juce::AudioParameterFloat* decay_ = nullptr;
    juce::AudioParameterFloat* size_ = nullptr;
    juce::AudioParameterFloat* damping_ = nullptr;
    juce::AudioParameterFloat* lowDecay_ = nullptr; ///< % of Decay
    juce::AudioParameterFloat* lowFrequency_ = nullptr;
    juce::AudioParameterFloat* bandwidth_ = nullptr;
    juce::AudioParameterFloat* diffusion_ = nullptr;
    juce::AudioParameterFloat* modRate_ = nullptr;
    juce::AudioParameterFloat* modulation_ = nullptr; ///< depth
    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* ducking_ = nullptr;
    juce::AudioParameterBool* freeze_ = nullptr;
    juce::AudioParameterFloat* lowCut_ = nullptr;
    juce::AudioParameterFloat* highCut_ = nullptr;
    juce::AudioParameterFloat* width_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    /** A delay line read at a (fractional) number of samples behind its input. */
    using Line = dsp::DelayLine;

    /** A Schroeder all-pass built on a delay line. */
    static double allPass(Line& line, double delay, double coefficient, double x) noexcept
    {
        const double delayed = line.read(delay - 1.0);
        const double v = x - coefficient * delayed;
        line.push(v);
        return delayed + coefficient * v;
    }


    double scale_ = 1.0; ///< samples per reference sample
    Line preDelayLine_;
    std::array<Line, 4> diffusers_;
    std::array<Line, 2> modulated_, firstDelay_, secondAllPass_, secondDelay_; ///< the tank's two branches
    std::array<double, 2> dampingState_{}, tankOut_{};
    std::array<std::array<double, 2>, 2> lowState_{}; ///< per branch, per decay stage (Low Decay split)
    double bandwidthState_ = 0.0;
    double lfoPhase_ = 0.0;
    double preDelaySamples_ = -1.0; ///< (glides)
    double inputGain_ = 1.0;        ///< (fades for Freeze)
    double duckEnvelope_ = 0.0;
    std::atomic<double> tempo_{120.0}; ///< the last tempo the play head gave
    std::array<dsp::Biquad, 2> lowCutFilter_, highCutFilter_;
};

} // namespace dojo::devices

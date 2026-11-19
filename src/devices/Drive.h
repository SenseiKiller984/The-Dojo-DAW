#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"
#include "devices/dsp/Oversampler.h"

#include <array>

namespace dojo::devices
{

/**
    Pedal: a guitar stompbox with three circuits, 4x oversampled.
    - Overdrive: the clipping stage only sees what is above ~720 Hz, mixed back with the clean
      signal (the mid-hump, touch-sensitive sound of classic overdrives); soft, symmetric diodes.
    - Distortion: much more gain, harder clipping to ground, a smoother top.
    - Fuzz: transistor fuzz: huge gain into biased (asymmetric) clipping that sputters as notes
      decay, almost square at full gain.
    Then a three-band EQ (Bass, Mid at 500 Hz or 1.5 kHz, Treble), Sub (a low shelf below 250 Hz
    that brings back the bottom), Output and Dry/Wet (the dry signal is latency-aligned).
*/
class Pedal final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:pedal";
    static constexpr double kFuzzBias = 0.3; ///< the fuzz's bias, relative to the input's level

    enum class Type
    {
        overdrive,
        distortion,
        fuzz,
    };

    Pedal();

    /** The transfer curve of a circuit at a drive gain (also used by the tests). */
    [[nodiscard]] static double shape(Type type, double x) noexcept;

    juce::AudioParameterChoice* type_ = nullptr;
    juce::AudioParameterFloat* gain_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* bass_ = nullptr;
    juce::AudioParameterFloat* mid_ = nullptr;
    juce::AudioParameterBool* midHigh_ = nullptr;
    juce::AudioParameterFloat* treble_ = nullptr;
    juce::AudioParameterBool* sub_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    dsp::Oversampler oversampler_;
    std::array<dsp::OnePole, 2> preHighPass_, cleanBlend_;
    std::array<dsp::OnePole, 2> postLowPass_, postLowPass2_;
    std::array<std::array<dsp::Biquad, 4>, 2> eq_;
    std::array<dsp::DcBlocker, 2> dc_;
    std::array<double, 2> fuzzEnvelope_{};
};

/**
    Overdrive: modelled on 1970s overdrive pedals.
    - A band-pass filter in front (Frequency, Width in octaves) chooses what gets driven.
    - Drive into an op-amp stage with diodes in its feedback: soft, symmetric clipping, 4x
      oversampled.
    - Tone: a post-filter from dark to bright.
    - Dynamics: how much of the input's dynamics survives the clipping. At 0 the sound is
      compressed flat; higher values let the input's attacks and swells come back through.
    - Dry/Wet and Output.
*/
class Overdrive final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:overdrive";

    Overdrive();

    juce::AudioParameterFloat* frequency_ = nullptr;
    juce::AudioParameterFloat* width_ = nullptr;
    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* tone_ = nullptr;
    juce::AudioParameterFloat* dynamics_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    dsp::Oversampler oversampler_;
    std::array<dsp::Biquad, 2> bandLow_, bandHigh_;
    std::array<dsp::OnePole, 2> toneFilter_;
    std::array<dsp::DcBlocker, 2> dc_;
    std::array<double, 2> fast_{}, slow_{};
};

/**
    Redux: a bit crusher and sample-rate reducer for the crunch of vintage 8- and 12-bit consoles
    and early samplers.
    - Downsample: holds each sample until the next tick of a slower clock (Rate, in Hz; aliasing
      included, that is the sound). Jitter randomises the clock. The pre-filter removes what would
      alias (cleaner); the post-filter smooths the steps (Octave moves it). Both are eighth-order.
    - Bit reduction: quantises to Bits (fractional values blend between depths). Shape spends more
      levels near zero (like companding converters: quiet parts keep detail). DC Shift truncates
      instead of rounding, the sputtery grit of cheap converters.
*/
class Redux final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:redux";

    Redux();

    /** The bit reduction of one sample. */
    [[nodiscard]] static double quantise(double x, double bits, double shape, bool dcShift) noexcept;

    juce::AudioParameterFloat* rate_ = nullptr;
    juce::AudioParameterFloat* jitter_ = nullptr;
    juce::AudioParameterBool* preFilter_ = nullptr;
    juce::AudioParameterBool* postFilter_ = nullptr;
    juce::AudioParameterFloat* postOctave_ = nullptr;
    juce::AudioParameterFloat* bits_ = nullptr;
    juce::AudioParameterFloat* shape_ = nullptr;
    juce::AudioParameterBool* dcShift_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<std::array<dsp::Biquad, 4>, 2> pre_, post_; ///< eighth-order Butterworth per channel
    std::array<double, 2> held_{};
    double phase_ = 1.0;
    double period_ = 1.0; ///< this tick's length (jitter varies it)
    dsp::Noise noise_;
};

} // namespace dojo::devices

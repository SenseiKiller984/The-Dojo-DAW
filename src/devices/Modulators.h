#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Modulation.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Shifter: pitch shifting, frequency shifting and ring modulation, without changing the timing.
    - Pitch: transposes by semitones and cents (+-48 st) with two crossfading read heads over a
      delay line; Window sets the grain (short: tighter, more metallic; long: smoother, more echo).
    - Frequency: moves every partial by the same number of Hz (single-sideband, through a 90-degree
      phase splitter), so harmonic sounds turn inharmonic and bell-like; negative values shift down.
    - Ring: multiplies the input with a sine (both sidebands).
    - Spread detunes the left and right channels in opposite directions. An LFO (free or synced,
      with a stereo phase) and an envelope follower modulate the shift. Delay and Feedback send the
      shifted signal back into the shifter (rising or falling spirals). Dry/Wet.
*/
class Shifter final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:shifter";
    static constexpr int kControlInterval = 16;
    static constexpr double kMaxWindowMs = 300.0;
    static constexpr double kMaxDelaySeconds = 2.0;

    enum class Mode
    {
        pitch,
        frequency,
        ring,
    };

    Shifter();

    /** The left channel's current shift (semitones in Pitch mode, Hz otherwise), for the editor. */
    [[nodiscard]] float getCurrentShift() const noexcept { return currentShift_.load(std::memory_order_relaxed); }

    juce::AudioParameterChoice* mode_ = nullptr;
    juce::AudioParameterFloat* pitch_ = nullptr;
    juce::AudioParameterFloat* fine_ = nullptr;
    juce::AudioParameterFloat* shift_ = nullptr;
    juce::AudioParameterFloat* spread_ = nullptr;
    juce::AudioParameterFloat* window_ = nullptr;
    juce::AudioParameterFloat* lfoAmount_ = nullptr;
    juce::AudioParameterFloat* lfoRate_ = nullptr;
    juce::AudioParameterBool* lfoSync_ = nullptr;
    juce::AudioParameterChoice* lfoDivision_ = nullptr;
    juce::AudioParameterChoice* lfoShape_ = nullptr;
    juce::AudioParameterFloat* lfoStereo_ = nullptr;
    juce::AudioParameterFloat* envAmount_ = nullptr;
    juce::AudioParameterFloat* envAttack_ = nullptr;
    juce::AudioParameterFloat* envRelease_ = nullptr;
    juce::AudioParameterFloat* delayTime_ = nullptr;
    juce::AudioParameterBool* delaySync_ = nullptr;
    juce::AudioParameterChoice* delayDivision_ = nullptr;
    juce::AudioParameterFloat* feedback_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<dsp::DelayLine, 2> grains_;   ///< the pitch shifter's input
    std::array<dsp::DelayLine, 2> feedbackLine_; ///< the feedback delay (of the wet signal)
    std::array<double, 2> head_{};           ///< read-head A's delay (head B is half a window apart)
    std::array<dsp::HilbertTransformer, 2> hilbert_;
    std::array<double, 2> oscillator_{};     ///< phase (radians)
    dsp::ModLfo lfo_;
    dsp::EnvelopeFollower follower_;
    std::atomic<float> currentShift_{0.0f};
};

/**
    Chorus-Ensemble: thickens and widens with modulated, slightly detuned copies of the signal.
    - Classic: two voices whose delays sweep in opposite directions (left and right).
    - Ensemble: three voices 120 degrees apart, with a second, faster shimmer, spread across the
      stereo field: the lush string-machine sound.
    - Vibrato: a single sweeping delay, pitch modulation only (use Dry/Wet at 100 %); Shape morphs
      the sweep from sine to triangle.
    Rate, Amount (depth), Feedback (negative inverts), Width (of the wet signal), Warmth (gentle
    saturation and a darker top), a high-pass on the wet signal (keeps the bass centred and clean),
    Dry/Wet and Output.
*/
class ChorusEnsemble final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:chorus";

    enum class Mode
    {
        classic,
        ensemble,
        vibrato,
    };

    ChorusEnsemble();

    /** The delay (ms) a voice reads at an LFO value (-1..1), for a mode and amount (0..1). */
    [[nodiscard]] static double voiceDelayMs(Mode mode, double lfo, double amount) noexcept;

    juce::AudioParameterChoice* mode_ = nullptr;
    juce::AudioParameterFloat* rate_ = nullptr;
    juce::AudioParameterFloat* amount_ = nullptr;
    juce::AudioParameterFloat* feedback_ = nullptr;
    juce::AudioParameterFloat* width_ = nullptr;
    juce::AudioParameterFloat* warmth_ = nullptr;
    juce::AudioParameterFloat* highPass_ = nullptr;
    juce::AudioParameterFloat* shape_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<dsp::DelayLine, 2> lines_;
    double phase_ = 0.0, shimmerPhase_ = 0.0;
    std::array<double, 2> feedbackSample_{};
    std::array<dsp::OnePole, 2> warmthFilter_;
    std::array<std::array<dsp::OnePole, 2>, 2> highPassFilter_;
};

/**
    Auto Pan: rhythmically moves the level and the panning.
    - An LFO (eight shapes, free in Hz or synced to the song) lowers the level by Amount.
    - Phase is the offset between the channels: 180 degrees pans from side to side (the total level
      stays constant), 0 is a tremolo (both together). Offset moves the start of the cycle; Invert
      flips it; Square pushes rounded shapes towards hard switching (choppy gating).
*/
class AutoPan final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:autopan";
    static constexpr int kControlInterval = 16;

    AutoPan();

    /** The gain of channel `channel` (0/1) for an LFO value (-1..1 before shaping). */
    [[nodiscard]] double gainFor(double lfo) const noexcept;
    /** The current gains (editor). */
    [[nodiscard]] float getGain(int channel) const noexcept { return gains_[static_cast<std::size_t>(channel)].load(std::memory_order_relaxed); }

    juce::AudioParameterFloat* amount_ = nullptr;
    juce::AudioParameterFloat* rate_ = nullptr;
    juce::AudioParameterBool* sync_ = nullptr;
    juce::AudioParameterChoice* division_ = nullptr;
    juce::AudioParameterChoice* shape_ = nullptr;
    juce::AudioParameterFloat* phase_ = nullptr;
    juce::AudioParameterFloat* offset_ = nullptr;
    juce::AudioParameterBool* invert_ = nullptr;
    juce::AudioParameterFloat* square_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    dsp::ModLfo lfo_;
    std::array<double, 2> current_{1.0, 1.0};
    std::array<std::atomic<float>, 2> gains_{};
};

} // namespace dojo::devices

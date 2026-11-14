#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"

#include <array>
#include <cstdint>
#include <vector>

namespace dojo::devices
{

/**
    Echo: a two-line delay with the character of tape and analogue echoes.

    - Channel modes: Stereo (independent left/right lines), Ping Pong (the echoes bounce between
      the sides) and Mid/Side (the lines carry mid and side).
    - Left and right times, tempo-synced (1/32 .. 1/1, dotted, triplet) or free (1 ms .. 4 s),
      optionally linked. Time changes glide (the repeats re-pitch like tape).
    - Feedback up to 100 % (bounded by soft saturation, so it can self-oscillate without blowing
      up), with optional polarity inversion.
    - Filter: high-pass and resonant low-pass on the echoes (and therefore on every repeat).
    - Modulation: an LFO on the delay time (chorus/vibrato on the repeats) and on the filter.
    - Character: Input gain, Drive (tape saturation in the loop), Noise (hiss), Wobble (random
      wow and flutter), Duck (the echoes get out of the way while the input is playing).
    - Reverb on the echoes: before the delay, after it, or inside the feedback loop.
    - Stereo width of the echoes and dry/wet mix.
*/
class Echo final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:echo";
    static constexpr double kMaxSeconds = 4.0;

    Echo();
    double getTailLengthSeconds() const override { return kMaxSeconds * 6.0; }

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    /** A small stereo reverb (four damped combs and two all-passes per side). */
    struct SmallReverb
    {
        struct Comb
        {
            std::vector<double> data;
            int index = 0;
            double store = 0.0;
        };
        struct AllPass
        {
            std::vector<double> data;
            int index = 0;
        };
        std::array<std::array<Comb, 4>, 2> combs;
        std::array<std::array<AllPass, 2>, 2> allPasses;
        void prepare(double sampleRate);
        void reset() noexcept;
        void process(double inLeft, double inRight, double feedback, double damping, double& outLeft, double& outRight) noexcept;
    };

    /** Zero-delay-feedback state-variable filter (one channel). */
    struct Svf
    {
        double ic1 = 0.0, ic2 = 0.0;
        void reset() noexcept { ic1 = ic2 = 0.0; }
        double lowPass(double x, double g, double k) noexcept;
    };

    [[nodiscard]] double targetDelaySeconds(bool right, double bpm) const noexcept;
    [[nodiscard]] double read(int channel, double delaySamples) const noexcept;
    [[nodiscard]] double nextRandom() noexcept;

    juce::AudioParameterChoice* mode_ = nullptr;
    juce::AudioParameterBool* sync_ = nullptr;
    juce::AudioParameterChoice* divisionLeft_ = nullptr;
    juce::AudioParameterChoice* divisionRight_ = nullptr;
    juce::AudioParameterFloat* timeLeft_ = nullptr;
    juce::AudioParameterFloat* timeRight_ = nullptr;
    juce::AudioParameterBool* link_ = nullptr;
    juce::AudioParameterFloat* feedback_ = nullptr;
    juce::AudioParameterBool* invert_ = nullptr;
    juce::AudioParameterFloat* input_ = nullptr;
    juce::AudioParameterBool* filter_ = nullptr;
    juce::AudioParameterFloat* highPass_ = nullptr;
    juce::AudioParameterFloat* lowPass_ = nullptr;
    juce::AudioParameterFloat* resonance_ = nullptr;
    juce::AudioParameterFloat* modRate_ = nullptr;
    juce::AudioParameterFloat* modDepth_ = nullptr;
    juce::AudioParameterFloat* modFilter_ = nullptr;
    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* noise_ = nullptr;
    juce::AudioParameterFloat* wobble_ = nullptr;
    juce::AudioParameterFloat* duck_ = nullptr;
    juce::AudioParameterFloat* reverb_ = nullptr;
    juce::AudioParameterFloat* reverbDecay_ = nullptr;
    juce::AudioParameterChoice* reverbLocation_ = nullptr;
    juce::AudioParameterFloat* width_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

    std::array<std::vector<double>, 2> lines_;
    int write_ = 0;
    std::array<double, 2> delay_{}; ///< samples, gliding towards the target
    double glide_ = 0.0;
    double lfoPhase_ = 0.0;
    double wobbleValue_ = 0.0, wobbleTarget_ = 0.0;
    int wobbleCountdown_ = 0;
    double duckEnvelope_ = 0.0, duckAttack_ = 0.0, duckRelease_ = 0.0;
    std::uint32_t random_ = 0x2545F491u;
    std::array<dsp::Biquad, 2> highPassFilter_;
    std::array<Svf, 2> lowPassFilter_;
    dsp::BiquadCoefficients highPassCoefficients_;
    float highPassFor_ = -1.0f;
    SmallReverb reverbProcessor_;
};

} // namespace dojo::devices

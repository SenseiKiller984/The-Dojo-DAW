#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/SynthCore.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace dojo::devices
{

/**
    Operator: a 4-operator FM (phase modulation) and subtractive synthesiser.

    - Operators A, B, C and D, each with a waveform (sine, saw, square, triangle — band-limited —
      or noise), a frequency ratio (coarse 0.5 .. 32 plus fine) or a fixed frequency, a level, its
      own ADSR envelope and velocity sensitivity.
    - 11 algorithms decide which operators modulate which and which are heard (carriers): from a
      single 4-operator stack to four sines added together. Operator D has feedback.
    - A multimode filter (optional) with its own envelope, key-independent cutoff.
    - A pitch envelope and an LFO routable to pitch, cutoff, volume or the modulators' levels.
    - 16 voices, glide, transpose, pitch bend (±2 semitones), sustain pedal.
*/
class Operator final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:operator";
    static constexpr int kOperators = 4;
    static constexpr int kVoices = 16;
    static constexpr int kAlgorithms = 11;
    static constexpr int kControlInterval = 16;

    /** Algorithm `index`: which operators each operator is modulated by (bit i = operator i, A = 0),
        and which operators are carriers. */
    struct Algorithm
    {
        std::array<std::uint8_t, kOperators> modulators;
        std::uint8_t carriers;
        const char* description;
    };
    static const std::array<Algorithm, kAlgorithms>& getAlgorithms();
    static juce::StringArray getAlgorithmNames();

    Operator();

    struct OperatorParameters
    {
        juce::AudioParameterChoice* wave = nullptr;
        juce::AudioParameterFloat* coarse = nullptr;
        juce::AudioParameterFloat* fine = nullptr;
        juce::AudioParameterBool* fixed = nullptr;
        juce::AudioParameterFloat* frequency = nullptr;
        juce::AudioParameterFloat* level = nullptr;
        juce::AudioParameterFloat* attack = nullptr;
        juce::AudioParameterFloat* decay = nullptr;
        juce::AudioParameterFloat* sustain = nullptr;
        juce::AudioParameterFloat* release = nullptr;
        juce::AudioParameterFloat* velocity = nullptr;
    };

    std::array<OperatorParameters, kOperators> operators_;
    juce::AudioParameterChoice* algorithm_ = nullptr;
    juce::AudioParameterFloat* feedback_ = nullptr;
    juce::AudioParameterBool* filterOn_ = nullptr;
    juce::AudioParameterChoice* filterType_ = nullptr;
    juce::AudioParameterFloat* cutoff_ = nullptr;
    juce::AudioParameterFloat* resonance_ = nullptr;
    juce::AudioParameterFloat* filterEnvelope_ = nullptr;
    juce::AudioParameterFloat* filterAttack_ = nullptr;
    juce::AudioParameterFloat* filterDecay_ = nullptr;
    juce::AudioParameterFloat* filterSustain_ = nullptr;
    juce::AudioParameterFloat* filterRelease_ = nullptr;
    juce::AudioParameterFloat* pitchEnvelope_ = nullptr;
    juce::AudioParameterFloat* pitchDecay_ = nullptr;
    juce::AudioParameterChoice* lfoShape_ = nullptr;
    juce::AudioParameterFloat* lfoRate_ = nullptr;
    juce::AudioParameterChoice* lfoDestination_ = nullptr;
    juce::AudioParameterFloat* lfoAmount_ = nullptr;
    juce::AudioParameterFloat* transpose_ = nullptr;
    juce::AudioParameterFloat* glide_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;

    [[nodiscard]] int getActiveVoiceCount() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }
    double getTailLengthSeconds() const override { return 5.0; }

    /** Ratio of an operator's frequency to the note (coarse 0 means 0.5). */
    [[nodiscard]] static double ratioFor(float coarse, float fine) noexcept;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Voice
    {
        bool active = false;
        int note = 0;
        double velocity = 0.0;
        double pitch = 0.0;
        double targetPitch = 0.0;
        bool keyDown = false;
        bool sustained = false;
        std::uint64_t age = 0;
        std::array<double, kOperators> phases{};
        std::array<double, kOperators> outputs{};
        std::array<double, 2> feedbackHistory{};
        std::array<dsp::Envelope, kOperators> envelopes;
        dsp::Envelope filterEnvelope;
        double pitchEnvelope = 0.0; ///< 1 at note-on, decays to 0
        dsp::Lfo lfo;
        dsp::MultiModeFilter filter;
    };

    void handleMidi(const juce::MidiMessage& message) noexcept;
    void noteOn(int note, double velocity) noexcept;
    void noteOff(int note) noexcept;
    void updateEnvelopes(Voice& voice) noexcept;
    void renderVoice(Voice& voice, double* output, int start, int end) noexcept;
    [[nodiscard]] double nextNoise() noexcept;

    std::array<Voice, kVoices> voices_;
    std::uint64_t ageCounter_ = 0;
    double lastPitch_ = -1.0;
    bool sustainPedal_ = false;
    double pitchBend_ = 0.0;
    std::uint32_t noiseState_ = 0x7F4A7C15u;
    std::atomic<int> activeVoices_{0};
};

} // namespace dojo::devices

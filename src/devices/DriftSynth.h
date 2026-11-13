#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/AnalogFilters.h"
#include "devices/dsp/Modulation.h"
#include "devices/dsp/SynthCore.h"
#include "devices/dsp/VirtualAnalog.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace dojo::devices
{

/**
    Drift: a compact, MPE-ready analog-modelled synthesiser with the drifting pitch and organic
    inconsistencies of classic hardware.

    - Oscillator 1: Sine, Triangle, Shark Tooth, Saturated, Saw or Pulse, each with a Shape control
      (sine: brighter self-modulation; triangle: skew; shark tooth: curve; saturated: drive; saw: an
      octave-up partner; pulse: width), modulated by Env 2 or the LFO. Oscillator 2 (sine,
      triangle, saw, square) with octave and detune, and noise.
    - A filter (LP I: 12 dB, LP II: 24 dB ladder, or high-pass) with key tracking, Env 2 and LFO
      amounts, and a gentle high-pass after it.
    - Env 1 shapes the level; Env 2 modulates, or cycles as a loop (attack up, decay down) for
      rhythmic movement. An LFO with nine shapes (including one-shot envelopes), free or synced,
      per note or free-running.
    - A three-slot modulation matrix: Env 1, Env 2, LFO, velocity, key, pressure, slide, mod wheel
      or a random value per note, to the oscillators, noise, pitch, filter, LFO and pan.
    - Voice modes: Poly, Mono (with legato and glide), Stereo (two voices panned apart) and Unison
      (four), with Spread. Drift detunes every voice a little, differently, and lets it wander.
    - MPE: each note's own pitch bend (48 semitones by default), pressure and slide.
*/
class DriftSynth final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:drift";
    static constexpr int kVoices = 16;
    static constexpr int kMaxStack = 4; ///< sub-voices per note (Unison)
    static constexpr int kMatrixSlots = 3;
    static constexpr int kControlInterval = 16;

    enum Source
    {
        srcOff,
        srcEnv1,
        srcEnv2,
        srcLfo,
        srcVelocity,
        srcNote,
        srcPressure,
        srcSlide,
        srcModWheel,
        srcRandom,
        numSources
    };

    enum Destination
    {
        dstOff,
        dstOsc1Shape,
        dstOsc1Gain,
        dstOsc2Gain,
        dstOsc2Detune,
        dstNoiseGain,
        dstPitch,
        dstFilterFrequency,
        dstFilterResonance,
        dstLfoRate,
        dstLfoAmount,
        dstPan,
        numDestinations
    };

    enum class Osc1Shape
    {
        sine,
        triangle,
        sharkTooth,
        saturated,
        saw,
        pulse,
    };

    DriftSynth();

    static const juce::StringArray& getSourceNames();
    static const juce::StringArray& getDestinationNames();
    /** Oscillator 1's waveform at phase `p` (naive; the saw-like shapes get PolyBLEP corrections in
        the voices). */
    [[nodiscard]] static double osc1Wave(Osc1Shape shape, double p, double amount) noexcept;

    [[nodiscard]] int getActiveVoiceCount() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }
    /** The most recent note's modulated filter frequency (editor). */
    [[nodiscard]] float getDisplayCutoff() const noexcept { return displayCutoff_.load(std::memory_order_relaxed); }
    double getTailLengthSeconds() const override { return 5.0; }

    struct MatrixSlot
    {
        juce::AudioParameterChoice* source = nullptr;
        juce::AudioParameterChoice* destination = nullptr;
        juce::AudioParameterFloat* amount = nullptr;
    };

    juce::AudioParameterChoice* osc1Shape_ = nullptr;
    juce::AudioParameterFloat* osc1Amount_ = nullptr;
    juce::AudioParameterFloat* osc1Octave_ = nullptr;
    juce::AudioParameterChoice* osc1ShapeSource_ = nullptr; ///< Env 2, LFO
    juce::AudioParameterFloat* osc1ShapeMod_ = nullptr;
    juce::AudioParameterChoice* osc2Shape_ = nullptr;
    juce::AudioParameterFloat* osc2Octave_ = nullptr;
    juce::AudioParameterFloat* osc2Detune_ = nullptr;
    juce::AudioParameterFloat* osc1Level_ = nullptr;
    juce::AudioParameterFloat* osc2Level_ = nullptr;
    juce::AudioParameterFloat* noiseLevel_ = nullptr;
    juce::AudioParameterFloat* pitchEnv_ = nullptr;
    juce::AudioParameterFloat* pitchLfo_ = nullptr;
    juce::AudioParameterChoice* filterType_ = nullptr; ///< LP I, LP II, High Pass
    juce::AudioParameterFloat* frequency_ = nullptr;
    juce::AudioParameterFloat* resonance_ = nullptr;
    juce::AudioParameterFloat* keyTrack_ = nullptr;
    juce::AudioParameterFloat* filterEnv_ = nullptr;
    juce::AudioParameterFloat* filterLfo_ = nullptr;
    juce::AudioParameterFloat* highPass_ = nullptr;
    std::array<juce::AudioParameterFloat*, 4> env1_{}, env2_{}; ///< attack, decay, sustain, release
    juce::AudioParameterBool* env2Cycle_ = nullptr;
    juce::AudioParameterChoice* lfoShape_ = nullptr;
    juce::AudioParameterFloat* lfoRate_ = nullptr;
    juce::AudioParameterBool* lfoSync_ = nullptr;
    juce::AudioParameterChoice* lfoDivision_ = nullptr;
    juce::AudioParameterFloat* lfoAmount_ = nullptr;
    juce::AudioParameterBool* lfoRetrigger_ = nullptr;
    std::array<MatrixSlot, kMatrixSlots> matrix_;
    juce::AudioParameterChoice* voiceMode_ = nullptr; ///< Poly, Mono, Stereo, Unison
    juce::AudioParameterChoice* voiceCount_ = nullptr;
    juce::AudioParameterFloat* spread_ = nullptr;
    juce::AudioParameterFloat* glide_ = nullptr;
    juce::AudioParameterBool* legato_ = nullptr;
    juce::AudioParameterFloat* drift_ = nullptr;
    juce::AudioParameterBool* mpe_ = nullptr;
    juce::AudioParameterFloat* noteBendRange_ = nullptr;
    juce::AudioParameterFloat* bendRange_ = nullptr;
    juce::AudioParameterFloat* velocityToAmp_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Voice
    {
        bool active = false, keyDown = false, sustained = false;
        int note = 60, channel = 1;
        double velocity = 0.0, random = 0.0;
        std::uint64_t age = 0;
        double pitch = 60.0, targetPitch = 60.0;
        std::array<double, kMaxStack> osc1Phase{}, osc2Phase{};
        dsp::Envelope env1, env2;
        double cyclePhase = 0.0;
        double lfoPhase = 0.0;
        std::array<dsp::CharacterFilter, 2> filters;
        std::array<dsp::OnePole, 2> highPass;
        double driftOffset = 0.0, driftCutoff = 0.0, driftTime = 0.0;
        std::uint32_t seed = 1;
    };

    [[nodiscard]] int polyphony() const noexcept;
    void startNote(int note, int channel, double velocity) noexcept;
    void releaseNote(int note, int channel) noexcept;
    void handleMidi(const juce::MidiMessage& message) noexcept;
    void updateEnvelopes(Voice& voice) noexcept;
    void renderVoice(Voice& voice, double* left, double* right, int start, int end, const dsp::TransportState& transport) noexcept;

    std::array<Voice, kVoices> voices_;
    dsp::NoteExpressions expressions_;
    dsp::NoteStack held_;
    dsp::Noise random_{0x7A11D1F7u}, noise_{0x1234ABCDu};
    std::uint64_t ageCounter_ = 0;
    double lastPitch_ = -1.0;
    double freeLfoPhase_ = 0.0;
    bool sustainPedal_ = false;
    std::atomic<int> activeVoices_{0};
    std::atomic<float> displayCutoff_{0.0f};
};

} // namespace dojo::devices

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
    Analog: a subtractive synthesiser modelled on vintage analog circuits.

    - Two band-limited oscillators (sine, saw, rectangle with pulse width and PWM, noise), each
      with octave, semitone and fine tuning, pitch modulation by LFO 1, and a balance between the
      two filters; oscillator 2 can hard-sync to oscillator 1. A noise generator with colour.
    - Two analog-modelled filters (12 dB state-variable and 24 dB transistor-ladder low-, band- and
      high-pass, notch) with drive, key tracking, an envelope each and LFO 2 modulation, in parallel
      (each into its own amplifier) or in series.
    - Two amplifiers with their own envelopes, levels, pans and tremolo (LFO 2).
    - Two LFOs per voice (free or tempo-synced), delayed vibrato, unison (up to 4 detuned voices),
      polyphony up to 16 or mono (with legato), glide, velocity to level and filter envelopes, and
      Drift: each voice is slightly, and slowly changingly, out of tune, as real oscillators are.
*/
class AnalogSynth final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:analog";
    static constexpr int kVoices = 16;
    static constexpr int kMaxUnison = 4;
    static constexpr int kControlInterval = 16;

    AnalogSynth();

    [[nodiscard]] int getActiveVoiceCount() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }
    double getTailLengthSeconds() const override { return 5.0; }

    struct Oscillator
    {
        juce::AudioParameterBool* on = nullptr;
        juce::AudioParameterChoice* shape = nullptr; ///< Sine, Saw, Rectangle, Noise
        juce::AudioParameterFloat* octave = nullptr;
        juce::AudioParameterFloat* semi = nullptr;
        juce::AudioParameterFloat* detune = nullptr;
        juce::AudioParameterFloat* width = nullptr;
        juce::AudioParameterFloat* pwm = nullptr;
        juce::AudioParameterFloat* lfoPitch = nullptr;
        juce::AudioParameterFloat* level = nullptr;
        juce::AudioParameterFloat* toFilter2 = nullptr;
    };
    struct Filter
    {
        juce::AudioParameterChoice* type = nullptr; ///< LP12, LP24, BP12, BP24, HP12, HP24, Notch
        juce::AudioParameterFloat* cutoff = nullptr;
        juce::AudioParameterFloat* resonance = nullptr;
        juce::AudioParameterFloat* drive = nullptr;
        juce::AudioParameterFloat* keyTrack = nullptr;
        juce::AudioParameterFloat* envelope = nullptr;
        juce::AudioParameterFloat* lfo = nullptr;
    };
    struct Adsr
    {
        juce::AudioParameterFloat* attack = nullptr;
        juce::AudioParameterFloat* decay = nullptr;
        juce::AudioParameterFloat* sustain = nullptr;
        juce::AudioParameterFloat* release = nullptr;
    };
    struct Amp
    {
        juce::AudioParameterFloat* level = nullptr;
        juce::AudioParameterFloat* pan = nullptr;
        juce::AudioParameterFloat* tremolo = nullptr;
    };
    struct LfoParameters
    {
        juce::AudioParameterChoice* shape = nullptr;
        juce::AudioParameterFloat* rate = nullptr;
        juce::AudioParameterBool* sync = nullptr;
        juce::AudioParameterChoice* division = nullptr;
    };

    std::array<Oscillator, 2> oscillators_;
    juce::AudioParameterBool* sync_ = nullptr;
    juce::AudioParameterFloat* noise_ = nullptr;
    juce::AudioParameterFloat* noiseColour_ = nullptr;
    juce::AudioParameterFloat* noiseToFilter2_ = nullptr;
    std::array<Filter, 2> filters_;
    juce::AudioParameterChoice* routing_ = nullptr; ///< Parallel, Serial
    std::array<Adsr, 2> filterEnvelopes_, ampEnvelopes_;
    std::array<Amp, 2> amps_;
    std::array<LfoParameters, 2> lfos_;
    juce::AudioParameterChoice* polyphony_ = nullptr; ///< Mono, 2, 4, 8, 16
    juce::AudioParameterBool* legato_ = nullptr;
    juce::AudioParameterFloat* glide_ = nullptr;
    juce::AudioParameterChoice* unison_ = nullptr;
    juce::AudioParameterFloat* unisonDetune_ = nullptr;
    juce::AudioParameterFloat* vibratoAmount_ = nullptr;
    juce::AudioParameterFloat* vibratoRate_ = nullptr;
    juce::AudioParameterFloat* vibratoDelay_ = nullptr;
    juce::AudioParameterFloat* velocityToAmp_ = nullptr;
    juce::AudioParameterFloat* velocityToFilter_ = nullptr;
    juce::AudioParameterFloat* drift_ = nullptr;
    juce::AudioParameterFloat* bendRange_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Voice
    {
        bool active = false, keyDown = false, sustained = false;
        int note = 60, channel = 1;
        double velocity = 0.0;
        std::uint64_t age = 0;
        double pitch = 60.0, targetPitch = 60.0;
        double seconds = 0.0; ///< since the note started (delayed vibrato)
        std::array<std::array<dsp::BlepOscillator, kMaxUnison>, 2> oscillators;
        std::array<dsp::Envelope, 2> filterEnvelopes, ampEnvelopes;
        std::array<dsp::Lfo, 2> lfos;
        std::array<dsp::CharacterFilter, 2> filters;
        dsp::OnePole noiseColour;
        dsp::Noise noise;
        double driftOffset = 0.0, driftPhase = 0.0;
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
    dsp::Noise random_{0x51ED2701u};
    std::uint64_t ageCounter_ = 0;
    double lastPitch_ = -1.0;
    bool sustainPedal_ = false;
    std::atomic<int> activeVoices_{0};
};

} // namespace dojo::devices

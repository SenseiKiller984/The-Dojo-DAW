#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/AnalogFilters.h"
#include "devices/dsp/Modulation.h"
#include "devices/dsp/SynthCore.h"
#include "devices/dsp/VirtualAnalog.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace dojo::devices
{

/**
    Meld: a bi-timbral, MPE-capable macro-oscillator synthesiser for experimental textures, atonal
    drones and complex harmonic pads.

    - Two engines, A and B, layered on every note or split across the keyboard (two instruments in
      one). Each has a macro oscillator with two Macro controls, its own tuning, a filter (low-,
      high-, band-pass, notch; clean, warm, ladder or diode circuit), an amplitude envelope, a
      modulation envelope and two LFOs, a level and a pan.
    - Thirteen macro oscillators: Basic Shapes (morph sine > triangle > saw > square; sub), Swarm
      (seven detuned saws; detune, mix), Harmonic FM (ratio steps, index), Inharmonic FM (free ratio
      for bells and atonal tones), Fold FM (self-modulation into a wavefolder), Squelch (a resonant
      filter riding the pitch), Pluck (a plucked string: brightness, decay), Bitgrunge (bits,
      sample-rate crunch locked to the pitch), Noise Loop (pitched, resonant noise), Shepard's Tone
      (endlessly rising or falling; rate, spread), Chip (pulse duty and bit depth), Additive
      (brightness, odd/even balance) and Rain (granular droplets: density, pitch scatter).
    - A 12-slot modulation matrix: both engines' envelopes and LFOs, velocity, key, pressure,
      slide, pitch bend, mod wheel and a random value per note, to either engine's macros, pitch,
      cutoff, resonance, level, pan and LFO rate.
    - MPE: each note's own pitch bend, pressure and slide.
*/
class Meld final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:meld";
    static constexpr int kVoices = 16;
    static constexpr int kMatrixSlots = 12;
    static constexpr int kControlInterval = 16;
    static constexpr int kPartials = 8;
    static constexpr int kGrains = 4;

    enum class Oscillator
    {
        basicShapes,
        swarm,
        harmonicFm,
        inharmonicFm,
        foldFm,
        squelch,
        pluck,
        bitgrunge,
        noiseLoop,
        shepard,
        chip,
        additive,
        rain,
    };

    enum Source
    {
        srcOff,
        srcEnv2A,
        srcEnv2B,
        srcLfo1A,
        srcLfo2A,
        srcLfo1B,
        srcLfo2B,
        srcVelocity,
        srcNote,
        srcPressure,
        srcSlide,
        srcModWheel,
        srcRandom,
        srcPitchBend,
        numSources
    };

    /** Per-engine destinations: engine B's are engine A's + kEngineDestinations. */
    enum EngineDestination
    {
        edMacro1,
        edMacro2,
        edPitch,
        edCutoff,
        edResonance,
        edLevel,
        edPan,
        edLfoRate,
        kEngineDestinations
    };
    static constexpr int numDestinations = 1 + 2 * kEngineDestinations; ///< (0 = Off)
    [[nodiscard]] static constexpr int destination(int engine, EngineDestination d) noexcept
    {
        return 1 + engine * kEngineDestinations + static_cast<int>(d);
    }

    Meld();

    static const juce::StringArray& getOscillatorNames();
    /** The two macros' names for an oscillator. */
    static std::pair<juce::String, juce::String> getMacroNames(Oscillator oscillator);
    static const juce::StringArray& getSourceNames();
    static const juce::StringArray& getDestinationNames();

    [[nodiscard]] int getActiveVoiceCount() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }
    /** The most recent note's macro values after modulation (editor). */
    [[nodiscard]] float getDisplayMacro(int engine, int macro) const noexcept
    {
        return displayMacros_[static_cast<std::size_t>(engine * 2 + macro)].load(std::memory_order_relaxed);
    }
    double getTailLengthSeconds() const override { return 8.0; }

    struct Engine
    {
        juce::AudioParameterBool* on = nullptr;
        juce::AudioParameterChoice* oscillator = nullptr;
        juce::AudioParameterFloat* macro1 = nullptr;
        juce::AudioParameterFloat* macro2 = nullptr;
        juce::AudioParameterFloat* octave = nullptr;
        juce::AudioParameterFloat* semi = nullptr;
        juce::AudioParameterFloat* fine = nullptr;
        juce::AudioParameterFloat* level = nullptr;
        juce::AudioParameterFloat* pan = nullptr;
        juce::AudioParameterChoice* filterType = nullptr; ///< LP 12, LP 24, HP 12, Band Pass, Notch
        juce::AudioParameterChoice* circuit = nullptr;
        juce::AudioParameterFloat* cutoff = nullptr;
        juce::AudioParameterFloat* resonance = nullptr;
        juce::AudioParameterFloat* keyTrack = nullptr;
        std::array<juce::AudioParameterFloat*, 4> amp{}, mod{}; ///< attack, decay, sustain, release
        std::array<juce::AudioParameterChoice*, 2> lfoShape{};
        std::array<juce::AudioParameterFloat*, 2> lfoRate{};
        juce::AudioParameterBool* lfoSync = nullptr;
        juce::AudioParameterChoice* lfoDivision = nullptr;
    };
    struct MatrixSlot
    {
        juce::AudioParameterChoice* source = nullptr;
        juce::AudioParameterChoice* destination = nullptr;
        juce::AudioParameterFloat* amount = nullptr;
    };

    std::array<Engine, 2> engines_;
    juce::AudioParameterChoice* mode_ = nullptr; ///< Layer, Split
    juce::AudioParameterFloat* splitNote_ = nullptr;
    std::array<MatrixSlot, kMatrixSlots> matrix_;
    juce::AudioParameterChoice* polyphony_ = nullptr;
    juce::AudioParameterFloat* glide_ = nullptr;
    juce::AudioParameterBool* mpe_ = nullptr;
    juce::AudioParameterFloat* noteBendRange_ = nullptr;
    juce::AudioParameterFloat* bendRange_ = nullptr;
    juce::AudioParameterFloat* velocityToAmp_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Grain
    {
        bool active = false;
        double phase = 0.0, increment = 0.0, age = 0.0, length = 1.0;
    };
    struct EngineVoice
    {
        bool on = false;
        std::array<double, kPartials> phase{};
        double aux = 0.0, aux2 = 0.0;
        double shepard = 0.0; ///< Shepard's Tone position (octaves, 0..1)
        std::array<double, kPartials> weights{};   ///< Shepard's Tone partial levels (per chunk)
        std::array<double, 16> partialGain{};      ///< Additive harmonic levels (per chunk)
        std::array<dsp::NonlinearSvf, 2> inner; ///< Squelch / Noise Loop's own filters
        dsp::CharacterFilter filter;
        dsp::Envelope amp, mod;
        std::array<double, 2> lfoPhase{};
        double* pluck = nullptr; ///< this engine voice's string (a slice of pluckPool_)
        int pluckWrite = 0;
        double pluckState = 0.0;
        std::array<Grain, kGrains> grains{};
        dsp::Noise noise;
    };
    struct Voice
    {
        bool active = false, keyDown = false, sustained = false;
        int note = 60, channel = 1;
        double velocity = 0.0, random = 0.0;
        std::uint64_t age = 0;
        double pitch = 60.0, targetPitch = 60.0;
        std::array<EngineVoice, 2> engines;
    };
    struct EngineState ///< one engine's settings for a chunk of one voice
    {
        Oscillator oscillator = Oscillator::basicShapes;
        double dt = 0.0, m1 = 0.0, m2 = 0.0, level = 0.0, panLeft = 1.0, panRight = 1.0;
        dsp::FilterMode filterMode = dsp::FilterMode::lowPass;
    };

    void startNote(int note, int channel, double velocity) noexcept;
    void releaseNote(int note, int channel) noexcept;
    void handleMidi(const juce::MidiMessage& message) noexcept;
    void updateEnvelopes(Voice& voice) noexcept;
    void startEngine(EngineVoice& engine, int index, double frequency) noexcept;
    void renderVoice(Voice& voice, double* left, double* right, int start, int end, const dsp::TransportState& transport) noexcept;
    double oscillate(EngineVoice& engine, const EngineState& state) noexcept;

    std::array<Voice, kVoices> voices_;
    std::vector<double> pluckPool_;
    int pluckCapacity_ = 1;
    dsp::NoteExpressions expressions_;
    dsp::Noise random_{0x0BADC0DEu};
    std::uint64_t ageCounter_ = 0;
    double lastPitch_ = -1.0;
    bool sustainPedal_ = false;
    std::atomic<int> activeVoices_{0};
    std::array<std::atomic<float>, 4> displayMacros_{};
};

} // namespace dojo::devices

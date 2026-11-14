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
    Wavetable: a polyphonic wavetable synthesiser.

    - Two wavetable oscillators. Each picks one of eight built-in tables (Basic Shapes, Harmonics,
      Pulse Width, Sync, Formant, Fold, Organ, Digital) and morphs through its frames with Position.
      Tables are band-limited per octave, so high notes stay clean.
    - Unison (1 .. 8 voices per oscillator) with detune and stereo spread; a sub oscillator and noise.
    - Multimode filter (12/24 dB low-pass, high-pass, band-pass, notch) with drive and key tracking.
    - Three envelopes (Env 1 drives the amplifier) and two LFOs.
    - A modulation matrix of 8 slots: any of Env 2, Env 3, LFO 1, LFO 2, Velocity, Mod Wheel, Note
      (key) and Pressure can modulate oscillator positions, pitches and levels, cutoff, resonance,
      volume, pan, LFO rates, sub and noise, by -100 .. +100 %.
    - 16 voices, glide, pitch bend (±2 semitones), sustain pedal.
*/
class WavetableSynth final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:wavetable";
    static constexpr int kVoices = 16;
    static constexpr int kMaxUnison = 8;
    static constexpr int kMatrixSlots = 8;
    static constexpr int kControlInterval = 16; ///< modulation is computed every 16 samples

    enum Source
    {
        srcOff,
        srcEnv2,
        srcEnv3,
        srcLfo1,
        srcLfo2,
        srcVelocity,
        srcModWheel,
        srcNote,
        srcPressure,
        numSources
    };

    enum Destination
    {
        dstOff,
        dstOsc1Position,
        dstOsc2Position,
        dstOsc1Pitch,
        dstOsc2Pitch,
        dstPitch,
        dstCutoff,
        dstResonance,
        dstVolume,
        dstOsc1Level,
        dstOsc2Level,
        dstPan,
        dstLfo1Rate,
        dstLfo2Rate,
        dstSubLevel,
        dstNoiseLevel,
        numDestinations
    };

    WavetableSynth();

    /** The built-in tables (shared by every instance, built on first use). */
    static const std::vector<dsp::BandLimitedWavetable>& getTables();
    static juce::StringArray getTableNames();
    static const juce::StringArray& getSourceNames();
    static const juce::StringArray& getDestinationNames();

    /** Oscillator `index` (0/1): its table and position after modulation of the most recent voice
        (for the editor's display). */
    [[nodiscard]] float getDisplayPosition(int oscillator) const noexcept
    {
        return displayPosition_[static_cast<std::size_t>(oscillator)].load(std::memory_order_relaxed);
    }
    [[nodiscard]] int getActiveVoiceCount() const noexcept { return activeVoices_.load(std::memory_order_relaxed); }

    double getTailLengthSeconds() const override { return 5.0; }

    struct OscillatorParameters
    {
        juce::AudioParameterBool* on = nullptr;
        juce::AudioParameterChoice* table = nullptr;
        juce::AudioParameterFloat* position = nullptr;
        juce::AudioParameterFloat* octave = nullptr;
        juce::AudioParameterFloat* semi = nullptr;
        juce::AudioParameterFloat* fine = nullptr;
        juce::AudioParameterFloat* level = nullptr;
    };
    struct EnvelopeParameters
    {
        juce::AudioParameterFloat* attack = nullptr;
        juce::AudioParameterFloat* decay = nullptr;
        juce::AudioParameterFloat* sustain = nullptr;
        juce::AudioParameterFloat* release = nullptr;
    };
    struct LfoParameters
    {
        juce::AudioParameterChoice* shape = nullptr;
        juce::AudioParameterFloat* rate = nullptr;
    };
    struct MatrixSlot
    {
        juce::AudioParameterChoice* source = nullptr;
        juce::AudioParameterChoice* destination = nullptr;
        juce::AudioParameterFloat* amount = nullptr;
    };

    // Parameters (for the editor).
    std::array<OscillatorParameters, 2> oscillators_;
    juce::AudioParameterFloat* sub_ = nullptr;
    juce::AudioParameterFloat* noise_ = nullptr;
    juce::AudioParameterChoice* unison_ = nullptr;
    juce::AudioParameterFloat* unisonDetune_ = nullptr;
    juce::AudioParameterFloat* unisonSpread_ = nullptr;
    juce::AudioParameterChoice* filterType_ = nullptr;
    juce::AudioParameterFloat* cutoff_ = nullptr;
    juce::AudioParameterFloat* resonance_ = nullptr;
    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* keyTrack_ = nullptr;
    std::array<EnvelopeParameters, 3> envelopes_;
    std::array<LfoParameters, 2> lfos_;
    std::array<MatrixSlot, kMatrixSlots> matrix_;
    juce::AudioParameterFloat* velocityToAmp_ = nullptr;
    juce::AudioParameterFloat* glide_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Voice
    {
        bool active = false;
        int note = 0;
        double velocity = 0.0;
        double pitch = 0.0;       ///< current (gliding) note
        double targetPitch = 0.0;
        bool keyDown = false;
        bool sustained = false;
        std::uint64_t age = 0;
        std::array<std::array<double, kMaxUnison>, 2> phases{};
        double subPhase = 0.0;
        std::array<dsp::Envelope, 3> envelopes;
        std::array<dsp::Lfo, 2> lfos;
        std::array<dsp::MultiModeFilter, 2> filters; ///< left, right
    };

    void handleMidi(const juce::MidiMessage& message) noexcept;
    void noteOn(int note, double velocity) noexcept;
    void noteOff(int note) noexcept;
    void renderVoice(Voice& voice, double* left, double* right, int start, int end) noexcept;
    [[nodiscard]] double nextNoise() noexcept;

    std::array<Voice, kVoices> voices_;
    std::uint64_t ageCounter_ = 0;
    double lastPitch_ = -1.0;
    bool sustainPedal_ = false;
    double pitchBend_ = 0.0;
    double modWheel_ = 0.0;
    double pressure_ = 0.0;
    std::uint32_t noiseState_ = 0x12345678u;
    std::array<std::atomic<float>, 2> displayPosition_{};
    std::atomic<int> activeVoices_{0};
};

} // namespace dojo::devices

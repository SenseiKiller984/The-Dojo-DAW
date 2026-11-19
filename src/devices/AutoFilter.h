#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/AnalogFilters.h"
#include "devices/dsp/Modulation.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Auto Filter: an analog-modelled filter swept by an LFO and an envelope follower.

    - Low-pass, high-pass, band-pass, notch, or Morph (a continuous sweep from low-pass through
      band-pass and high-pass to notch); 12 or 24 dB per octave.
    - Circuits: Clean (exact), Warm (OTA state-variable whose resonance saturates), Ladder (four-pole
      transistor ladder) and Diode (aggressive, screaming resonance). Drive pushes the circuit.
    - LFO: eight shapes, free (Hz) or synced to the song (1/64 .. 8 bars), a phase offset, and a
      stereo phase that sweeps the two channels apart (180 degrees: one opens while the other closes).
    - Envelope follower: the filter opens (or closes, with a negative amount) as the input gets
      louder; it can follow the sidechain instead (filter one track by another's rhythm).
    Modulation is in octaves: 100 % LFO swings +-3 octaves, 100 % envelope +4.
*/
class AutoFilter final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:autofilter";
    static constexpr int kControlInterval = 16;
    static constexpr double kLfoOctaves = 3.0;
    static constexpr double kEnvelopeOctaves = 4.0;

    AutoFilter();

    /** The left channel's cutoff after modulation (editor). */
    [[nodiscard]] float getModulatedCutoff() const noexcept { return modulatedCutoff_.load(std::memory_order_relaxed); }
    /** The linear response (dB) of the current settings at `frequency`, with the cutoff at `cutoff`
        (for drawing; the non-linear circuits are close to it at low levels). */
    [[nodiscard]] double getResponseDb(double frequency, double cutoff) const noexcept;

    juce::AudioParameterChoice* type_ = nullptr;
    juce::AudioParameterChoice* slope_ = nullptr;
    juce::AudioParameterChoice* circuit_ = nullptr;
    juce::AudioParameterFloat* frequency_ = nullptr;
    juce::AudioParameterFloat* resonance_ = nullptr;
    juce::AudioParameterFloat* morph_ = nullptr;
    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* lfoAmount_ = nullptr;
    juce::AudioParameterFloat* lfoRate_ = nullptr;
    juce::AudioParameterBool* lfoSync_ = nullptr;
    juce::AudioParameterChoice* lfoDivision_ = nullptr;
    juce::AudioParameterChoice* lfoShape_ = nullptr;
    juce::AudioParameterFloat* lfoStereo_ = nullptr;
    juce::AudioParameterFloat* lfoOffset_ = nullptr;
    juce::AudioParameterFloat* envAmount_ = nullptr;
    juce::AudioParameterFloat* envAttack_ = nullptr;
    juce::AudioParameterFloat* envRelease_ = nullptr;
    juce::AudioParameterBool* external_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    std::array<dsp::CharacterFilter, 2> filters_;
    dsp::ModLfo lfo_;
    dsp::EnvelopeFollower follower_;
    std::array<double, 2> cutoff_{1000.0, 1000.0}; ///< smoothed, per channel
    std::atomic<float> modulatedCutoff_{1000.0f};
};

} // namespace dojo::devices

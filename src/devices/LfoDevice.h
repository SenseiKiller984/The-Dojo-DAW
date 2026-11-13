#pragma once

#include "core/ModulationSource.h"
#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Modulation.h"

#include <atomic>

namespace dojo::devices
{

/**
    LFO: a low-frequency oscillator that physically moves other controls anywhere in the project:
    any device or plug-in parameter on any track or the master, and track faders, pans and send
    levels. The mapped controls move on screen as it plays. (The audio passes through untouched.)

    - Eight shapes (sine, triangle, saws, square, sample & hold, smooth random, wander), free in Hz
      or synced to the song (1/64 .. 8 bars; locked to the song position while playing).
    - Depth and Offset set the swing and its centre; Phase shifts the cycle; Jitter adds a random
      deviation each cycle; Smooth rounds off sharp edges; Invert flips it; Hold freezes it where it
      is; Retrigger restarts a free LFO when playback starts.
    - Each of up to eight mappings has its own range (min .. max of the target); removing a mapping
      returns the control to where it was. Mappings are stored in the session (see
      session::modulation); the engine reads advanceModulation() once per block.
*/
class LfoDevice final : public BuiltinProcessor, public ModulationSource
{
public:
    static constexpr const char* kTypeId = "builtin:lfo";

    LfoDevice();

    double advanceModulation(int numSamples, double sampleRate, double bpm, double ppq, bool playing) noexcept override;

    /** The output (0..1) the settings give at a phase (cycles), without jitter or smoothing. */
    [[nodiscard]] double shapeAt(double phase) const noexcept;
    /** The latest output (0..1) and phase (cycles, 0..1), for the editor's scope. */
    [[nodiscard]] float getOutput() const noexcept { return output_.load(std::memory_order_relaxed); }
    [[nodiscard]] float getPhase() const noexcept { return phaseDisplay_.load(std::memory_order_relaxed); }

    juce::AudioParameterChoice* shape_ = nullptr;
    juce::AudioParameterFloat* rate_ = nullptr;
    juce::AudioParameterBool* sync_ = nullptr;
    juce::AudioParameterChoice* division_ = nullptr;
    juce::AudioParameterFloat* depth_ = nullptr;
    juce::AudioParameterFloat* offset_ = nullptr;
    juce::AudioParameterFloat* phase_ = nullptr;
    juce::AudioParameterFloat* jitter_ = nullptr;
    juce::AudioParameterFloat* smooth_ = nullptr;
    juce::AudioParameterBool* hold_ = nullptr;
    juce::AudioParameterBool* retrigger_ = nullptr;
    juce::AudioParameterBool* invert_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    double cycles_ = 0.0;   ///< total phase (cycles since the start)
    double smoothed_ = 0.5;
    bool wasPlaying_ = false;
    bool started_ = false;
    std::atomic<float> output_{0.5f};
    std::atomic<float> phaseDisplay_{0.0f};
};

} // namespace dojo::devices

#pragma once

#include "devices/BuiltinProcessor.h"

#include <array>

namespace dojo::devices
{

/**
    Click: a metronome that follows the transport (tempo and position from the play head). It adds
    a short synthesized click on every beat (or eighth / sixteenth), sample-accurately, while the
    transport plays. The first beat of each bar is accented: louder and higher. Click Tracks are aux
    inputs with this device inserted (Track > Create Click Track).

    - Sound        Beep, Woodblock, Rim or Cowbell
    - Volume       -60 .. +6 dB
    - Accent       the downbeat is accented
    - Subdivision  quarter notes, eighths or sixteenths
    - Play         during playback and recording, or only while recording
*/
class Click final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:click";

    enum class Sound
    {
        beep,
        woodblock,
        rim,
        cowbell,
    };

    Click();

    // Parameters (for tests and the editor).
    juce::AudioParameterChoice* sound_ = nullptr;
    juce::AudioParameterFloat* volume_ = nullptr;
    juce::AudioParameterBool* accent_ = nullptr;
    juce::AudioParameterChoice* subdivision_ = nullptr;
    juce::AudioParameterChoice* when_ = nullptr; ///< 0 = play and record, 1 = record only

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    /** Starts a click at sample `offset` of the block. */
    void trigger(bool accented) noexcept;
    [[nodiscard]] double nextSample() noexcept;

    struct Voice
    {
        bool active = false;
        double age = 0.0; ///< seconds since the click started
        double phase1 = 0.0, phase2 = 0.0;
        double frequency1 = 1000.0, frequency2 = 0.0;
        double partial2 = 0.0;
        double noise = 0.0;
        double decay = 0.03;
        double gain = 1.0;
    };
    Voice voice_;
    juce::Random random_{0x5eed};
    long long lastTick_ = -1; ///< index of the last tick played (in subdivisions), to never play one twice
};

} // namespace dojo::devices

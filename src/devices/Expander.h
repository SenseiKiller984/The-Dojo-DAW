#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Expander: the opposite of a compressor, for more dynamics instead of less.

    - Downward: below the Threshold quiet sound is made quieter still (Ratio 2:1: 1 dB under the
      threshold becomes 2 dB under), at most by Range dB. A gentler, more natural alternative to a
      gate for room noise, spill, hum and breaths.
    - Upward: above the threshold loud sound is made louder (restoring the punch that compression or
      limiting took away), at most by Range dB.
    - Knee: a soft transition around the threshold. Hysteresis: once open, the expander only closes
      again that many dB under the threshold, so a level hovering at the threshold does not chatter.
    - Attack: how fast the gain comes back up; Hold: how long it waits after the level drops before
      it starts to close; Release: how fast it closes.
    - Detector: Peak (fast, follows transients) or RMS (smoother, follows loudness). Lookahead (up
      to 10 ms) opens it before a transient arrives, so the first hit is never cut (the delay
      compensation keeps everything in time).
    - Sidechain: the detector can listen to another track (External Sidechain), through a key
      filter (Key High-Pass and Key Low-Pass) so only the right part of the sound (e.g. the snare,
      not the hi-hat) drives it. Key Listen plays what the detector hears.
    - Output trims the level; Dry/Wet blends with the (latency-aligned) dry signal.
*/
class Expander final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:expander";
    static constexpr double kRmsSeconds = 0.01;

    Expander();
    ~Expander() override;

    /** Message thread: the largest gain change (dB: < 0 turned down, > 0 turned up) and detector level
        (dB) since the last call. */
    [[nodiscard]] float takeGainChangeDb() noexcept { return gainChange_.exchange(0.0f, std::memory_order_acq_rel); }
    [[nodiscard]] float takeDetectorDb() noexcept { return detector_.exchange(-100.0f, std::memory_order_acq_rel); }
    /** The static gain change (dB: negative down, positive up) for a detector level (dB), when closed
        (`open` false) or open (hysteresis), for drawing and tests. */
    [[nodiscard]] double gainChangeFor(double levelDb, bool open = false) const noexcept;
    [[nodiscard]] int lookaheadSamples() const noexcept;
    [[nodiscard]] static double lookaheadMsOf(int choice) noexcept;

    juce::AudioParameterChoice* mode_ = nullptr; ///< Downward, Upward
    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterFloat* ratio_ = nullptr;
    juce::AudioParameterFloat* range_ = nullptr;
    juce::AudioParameterFloat* knee_ = nullptr;
    juce::AudioParameterFloat* hysteresis_ = nullptr;
    juce::AudioParameterFloat* attack_ = nullptr;
    juce::AudioParameterFloat* hold_ = nullptr;
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterChoice* detect_ = nullptr;    ///< Peak, RMS
    juce::AudioParameterChoice* lookahead_ = nullptr; ///< Off, 1, 3, 5, 10 ms
    juce::AudioParameterBool* external_ = nullptr;
    juce::AudioParameterFloat* keyLow_ = nullptr;
    juce::AudioParameterFloat* keyHigh_ = nullptr;
    juce::AudioParameterBool* keyListen_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void parameterValueChanged(int, float) override;
    void parameterGestureChanged(int, bool) override {}

    std::array<dsp::Biquad, 2> keyHighPass_, keyLowPass_;
    std::array<dsp::DelayLine, 2> delay_; ///< lookahead
    double level_ = 0.0;      ///< detector (linear)
    double meanSquare_ = 0.0; ///< RMS detector
    double gainDb_ = 0.0;     ///< smoothed gain change
    double holdLeft_ = 0.0;   ///< seconds of hold left
    bool open_ = false;       ///< hysteresis state
    std::atomic<float> gainChange_{0.0f};
    std::atomic<float> detector_{-100.0f};
};

} // namespace dojo::devices

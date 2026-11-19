#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    De-Esser: tames sibilance ("s", "sh", "t", "ch") and other harsh, spiky highs.

    - Band: the sibilance lives Above the Frequency, or in a Band between Frequency and High (which
      leaves the air above it alone). The detector listens to that band only.
    - Detection: Absolute compares the band's level with the Threshold (dBFS); Relative compares it
      with the level of the whole signal (the highs' share, in dB), so the same setting works
      however loud the singer is (quiet passages are left alone).
    - Above the threshold the gain comes down (4:1 with a soft knee), at most by Range dB, within
      half a millisecond; Release sets how fast it lets go.
    - Mode: Split turns down only the band (the body of the voice is untouched; crossovers that sum
      back flat); Wide turns down the whole signal while the sibilance lasts.
    - Lookahead (0 to 5 ms) delays the audio so the gain is already down when the "s" starts (the
      delay compensation keeps everything in time).
    - Stereo: Linked (one gain for both sides, the image stays put), Unlinked (each side on its
      own), Mid or Side only (e.g. sibilance in a centred vocal, or in the stereo reverb).
    - Listen: Trigger plays what the detector hears (to find the sibilance); Delta plays what is
      being removed.
*/
class DeEsser final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:deesser";
    static constexpr double kRatio = 4.0;
    static constexpr double kKneeDb = 3.0;
    static constexpr double kAttackSeconds = 0.0005;
    static constexpr double kRelativeGateDb = -70.0; ///< Relative: quieter than this is left alone

    DeEsser();
    ~DeEsser() override;

    /** Message thread: the largest gain reduction (dB, >= 0) and detector level (dB: dBFS, or the
        band's share in Relative) since the last call. */
    [[nodiscard]] float takeReductionDb() noexcept { return reduction_.exchange(0.0f, std::memory_order_acq_rel); }
    [[nodiscard]] float takeDetectorDb() noexcept { return detector_.exchange(-100.0f, std::memory_order_acq_rel); }
    /** The reduction (dB, >= 0) for a detector level (dB), for drawing and tests. */
    [[nodiscard]] double reductionFor(double levelDb) const noexcept;
    /** The detector's response (dB) at a frequency, for drawing. */
    [[nodiscard]] double detectorResponseDb(double frequency) const noexcept;
    [[nodiscard]] int lookaheadSamples() const noexcept;
    [[nodiscard]] static double lookaheadMsOf(int choice) noexcept;

    juce::AudioParameterFloat* frequency_ = nullptr;
    juce::AudioParameterFloat* high_ = nullptr;
    juce::AudioParameterChoice* band_ = nullptr;      ///< Above, Band
    juce::AudioParameterFloat* threshold_ = nullptr;
    juce::AudioParameterFloat* range_ = nullptr;
    juce::AudioParameterChoice* detection_ = nullptr; ///< Absolute, Relative
    juce::AudioParameterChoice* mode_ = nullptr;      ///< Split, Wide
    juce::AudioParameterFloat* release_ = nullptr;
    juce::AudioParameterChoice* lookahead_ = nullptr; ///< Off, 1 ms, 2 ms, 5 ms
    juce::AudioParameterChoice* stereo_ = nullptr;    ///< Linked, Unlinked, Mid, Side
    juce::AudioParameterChoice* listen_ = nullptr;    ///< Off, Trigger, Delta

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void parameterValueChanged(int, float) override;
    void parameterGestureChanged(int, bool) override {}

    /** One processed signal path (left, right, mid or side). */
    struct Lane
    {
        std::array<dsp::Biquad, 2> detect;        ///< detector band (two stages)
        dsp::LinkwitzRiley lower, upper;          ///< split at Frequency, and at High (Band)
        dsp::LinkwitzRiley align;                 ///< the lows' phase match for the upper split
        dsp::DelayLine delay;                     ///< lookahead
        double envelope = 0.0, fullEnvelope = 0.0; ///< band level, whole-signal level
        void reset() noexcept;
    };

    std::array<Lane, 2> lanes_;
    double smoothedFrequency_ = -1.0, smoothedHigh_ = -1.0;
    std::atomic<float> reduction_{0.0f};
    std::atomic<float> detector_{-100.0f};
};

} // namespace dojo::devices

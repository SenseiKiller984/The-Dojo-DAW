#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/PhaseOversampler.h"

#include <array>
#include <atomic>
#include <vector>

namespace dojo::devices
{

/**
    A multichannel analog tape recorder after the Studer A800: each channel's complete electronic
    signal paths, switched as the machine's microprocessor switches them.

    Signal paths (Path):
    - Input: the live electronics only (input transformer, line amplifier with +22 dBu of headroom
      and an ultrasonic low-pass, output line amplifier and transformer), no tape.
    - Sync: record electronics onto tape (pre-emphasis, bias, the tape's magnetisation), played
      back off the record head: its wider gap loses more highs and its contour gives a stronger
      low-frequency ripple, partly corrected by the sync EQ.
    - Repro: the same recording played back off the dedicated reproduce head (the narrowest gap:
      the widest, flattest response), through the reproduce EQ and its HF / LF fine trims.
    - Thru: a hard-wire bypass: the input, untouched (only delayed by the oversampling latency, so
      switching compares like with like).
    - Auto Input: while the transport is stopped the tape paths switch to Input, as the machine does
      in Stop; Play brings back Sync or Repro. Every switch crossfades in 20 ms.

    Tape: Speed (7.5, 15 or 30 ips) with its EQ standard (AES 35 us at 30 ips; NAB 50 us + 3180 us
    or CCIR at 15 and 7.5 ips: CCIR 35 us at 15, 70 us at 7.5), four tape Formulas (Vintage,
    Classic, Hi-Output, Mastering: their maximum output level, how gently they saturate, their
    hiss, low-end bump and top end), Calibration (how hot 0 VU, -18 dBFS, records: +3, +6 or +9 dB
    over 185 nWb/m) and Bias (from 3 dB under to 3 dB over: under is brighter and dirtier, over is
    darker and cleaner). The record pre-emphasis boosts the highs (and with NAB the deep lows) before
    the tape saturates, the playback EQ is its exact inverse: highs and NAB lows saturate first, as
    on a real machine. The head bump gives the low-end punch; the frequency response follows the
    machine's specification (30 ips: about 50 Hz - 23 kHz, 15 ips: 25 Hz - 22 kHz, 7.5 ips: 30 Hz -
    16 kHz, within 1 to 1.5 dB). Noise adds the formula's hiss.

    Levels: 0 VU = +4 dBu = -18 dBFS; the input clips at +22 dBu (0 dBFS), the output stage at
    +24 dBm into 600 ohms (+2 dBFS).

    Group (1 to 4): every A800 in the same group moves together: change a setting on one and the
    others follow (a multichannel machine's channels share their transport and alignment).
*/
class TapeA800 final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:tape800";
    static constexpr double kOperatingLevelDbfs = -18.0; ///< 0 VU = +4 dBu
    static constexpr double kInputClipDbfs = 0.0;        ///< +22 dBu
    static constexpr double kOutputClipDbfs = 2.0;       ///< +24 dBm into 600 ohms
    static constexpr double kBiasFrequency = 240000.0;   ///< (the master oscillator, shown for reference)
    static constexpr double kCrossfadeSeconds = 0.02;

    enum Path
    {
        input = 0,
        sync,
        repro,
        thru
    };

    struct Formula
    {
        const char* name;
        double molDb;    ///< maximum output level (3 % THD at 1 kHz), dB over 185 nWb/m
        double hardness; ///< how suddenly it saturates (higher: cleaner, then harder)
        double knee;     ///< the curve's input amplitude at 3 % THD (for this hardness)
        double noiseDb;  ///< hiss, dB under 185 nWb/m at 15 ips
        double bump;     ///< head bump size (1 = nominal)
        double top;      ///< high-frequency extension (1 = nominal)
    };
    [[nodiscard]] static const Formula& formulaOf(int index) noexcept;

    /** The EQ standard's time constants (seconds; 0 = none) for a speed (0 7.5, 1 15, 2 30 ips) and
        a standard choice (0 NAB, 1 CCIR; 30 ips is always AES). */
    struct Emphasis
    {
        double highSeconds;
        double lowSeconds;
        const char* name;
    };
    [[nodiscard]] static Emphasis emphasisOf(int speed, int standard) noexcept;
    [[nodiscard]] static double inchesPerSecond(int speed) noexcept;

    TapeA800();
    ~TapeA800() override;

    void setNonRealtime(bool isNonRealtime) noexcept override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    /** The path playing now (Auto Input may have switched to Input). */
    [[nodiscard]] int getActivePath() const noexcept { return activePath_.load(std::memory_order_relaxed); }
    /** Whether the transport is rolling (the reels turn). */
    [[nodiscard]] bool isRolling() const noexcept { return rolling_.load(std::memory_order_relaxed); }
    /** The output's VU reading per channel (0 VU = -18 dBFS), with VU ballistics. */
    [[nodiscard]] float getVuDb(int channel) const noexcept { return vu_[static_cast<std::size_t>(channel & 1)].load(std::memory_order_relaxed); }
    [[nodiscard]] int currentLatency() const noexcept;
    [[nodiscard]] int latencyFor(bool offline) const noexcept;

    juce::AudioParameterChoice* path_ = nullptr;    ///< Input, Sync, Repro, Thru
    juce::AudioParameterBool* autoInput_ = nullptr;
    juce::AudioParameterChoice* speed_ = nullptr;   ///< 7.5, 15, 30 ips
    juce::AudioParameterChoice* standard_ = nullptr; ///< NAB, CCIR
    juce::AudioParameterChoice* formula_ = nullptr;
    juce::AudioParameterChoice* calibration_ = nullptr; ///< +3, +6, +9 dB
    juce::AudioParameterFloat* input_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* bias_ = nullptr;
    juce::AudioParameterFloat* reproHigh_ = nullptr;
    juce::AudioParameterFloat* reproLow_ = nullptr;
    juce::AudioParameterBool* noise_ = nullptr;
    juce::AudioParameterChoice* group_ = nullptr; ///< Off, 1 .. 4
    juce::AudioParameterChoice* osRealtime_ = nullptr;
    juce::AudioParameterChoice* osOffline_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    void parameterValueChanged(int index, float value) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int factorChoice() const noexcept;
    void updateFilters(double rate) noexcept;
    /** Takes every setting (but the group) from another machine, without passing it on. */
    void copySettingsFrom(const TapeA800& other);

    /** A first-order section y = (b0 x + b1 x1 - a1 y1). */
    struct FirstOrder
    {
        double b0 = 1.0, b1 = 0.0, a1 = 0.0;
        [[nodiscard]] FirstOrder inverse() const noexcept { return {1.0 / b0, a1 / b0, b1 / b0}; }
    };
    struct FirstOrderState
    {
        double x1 = 0.0, y1 = 0.0;
        double process(double x, const FirstOrder& c) noexcept
        {
            const double y = c.b0 * x + c.b1 * x1 - c.a1 * y1;
            x1 = x;
            y1 = y;
            return y;
        }
    };

    /** One audio channel's circuits. */
    struct Channel
    {
        FirstOrderState inputHighPass, inputCore, outputCore;
        dsp::Biquad ultrasonic;
        FirstOrderState recordHigh, recordLow, playHigh, playLow; ///< pre-emphasis and its exact inverse
        std::array<dsp::Biquad, 4> reproHead, syncHead;          ///< low cut, bump, contour dip, gap loss
        dsp::Biquad biasShelf, trimHigh, trimLow;
        void reset() noexcept;
    };

    /** The filter settings of the current speed, standard, formula and rate. */
    struct Filters
    {
        FirstOrder inputHighPass, core, recordHigh, recordLow, playHigh, playLow;
        dsp::BiquadCoefficients ultrasonic;
        bool ultrasonicOn = false;
        std::array<dsp::BiquadCoefficients, 4> reproHead, syncHead;
        bool reproGapLoss = true, syncGapLoss = true;
        dsp::BiquadCoefficients biasShelf, trimHigh, trimLow;
        double rate = 0.0;
        int speed = -1, standard = -1, formula = -1;
        float bias = 1.0e9f, high = 1.0e9f, low = 1.0e9f;
    };

    dsp::PhaseOversampler oversampler_;
    std::array<Channel, 2> channels_;
    Filters filters_;
    std::array<double, 4> weights_{0.0, 0.0, 1.0, 0.0}; ///< each path's share (crossfades)
    std::array<std::vector<double>, 4> weightRamp_;     ///< per base sample of the block
    std::uint32_t noiseState_ = 0x2545f491u;
    std::array<double, 2> vuSquare_{};
    std::atomic<int> activePath_{repro};
    std::atomic<bool> rolling_{false};
    std::array<std::atomic<float>, 2> vu_{};
    bool loading_ = false, applyingGroup_ = false;
};

} // namespace dojo::devices

#pragma once

#include "core/InsertLoop.h"
#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/PhaseOversampler.h"

#include <array>
#include <atomic>
#include <utility>
#include <vector>

namespace dojo::devices
{

/**
    The EMI TG12345 (Abbey Road, 1968), EMI's first transistorised recording and mixing console,
    as one device: a plug-in Cassette of the desk, chosen with Cassette.

    Levels: +4 dBu (0 VU) = -18 dBFS; the Amplifier Type B output drivers deliver +24 dBu into 600
    ohms (+2 dBFS). Inputs and outputs are transformer-balanced; the stud switches are exact.
    Every cassette regulates its own supply, so its headroom never sags and nothing passes between
    cassettes. With everything flat: 20 Hz - 20 kHz within 0.5 dB, under 0.1 % THD at +4 dBu.

    The amplifier cards (each a discrete transistor stage with its own headroom and its own small
    curvature): Type A (low-noise gain stages, the EQ's make-up amplifiers and the mix amplifiers),
    Type B (push-pull line driver, +24 dBu), Type C1 (high-headroom buffer after the input gain) and
    Type E (high input impedance, 50 kOhm, feeding the echo send and the pan-pots in parallel).

    Cassettes:
    - Microphone (a dual-channel cassette: the track's left and right sides are its channels A and
      B; or Mono: A alone, from both sides): mic input transformer (1:2, core saturation in the deep
      lows at high levels), Input Gain stud switches (6 dB coarse, 2 dB fine), Amplifier C1, the
      compressor / limiter (Out, Comp 2:1 or Limit 7:1, 1 ms attack, Hold threshold, Recovery 100
      ms - 5 s; a feedback design), the EQ (Bass 50 Hz shelf, Treble: boost a 5 kHz bell, cut a 10
      kHz shelf, each +-10 dB in 2 dB steps, calibrated at their frequency; Presence on the Mk II),
      Amplifier A, Amplifier E, the echo send (pre or post fader) and the pan-pots (two reverse-
      connected 11 kOhm pots into 22 kOhm mixing resistors: centre -7 dB), the Spreader (Mk I: an
      M/S width matrix), the channel fader, Amplifier B and the output transformer.
      Echo: the echo send feeds this device's insert loop (an echo chamber: put a reverb or delay
      there); Echo Return mixes what comes back into the output, delayed in step with the loop.
    - Group: the sub-group mix amplifier, its compressor / limiter, Presence EQ, group fader,
      Amplifier B and output transformer.
    - Main: the master mix amplifier, main fader, output drivers and transformers; Track Announce
      puts the talkback microphone (the device's sidechain input) or a 1 kHz tone on the main
      outputs; the display monitors the outputs and their phase correlation.
    - Echo / Aux: the master echo send and return: Send Master into the insert loop (the chamber),
      Return Master and the return's width (Spreader) out. With nothing in the loop the send goes
      straight to the return.
    - Control Room & Studio: monitoring. Listen to the Control Room (its level, Dim -20 dB, Mono,
      Cut) or to the Studio playback (its level, plus the talkback from the sidechain input while
      Talkback is held). Talkback dims the control room. Monitoring is for listening: offline
      exports pass the signal through.
*/
class ConsoleTG12345 final : public BuiltinProcessor, public InsertLoopHost, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:tg12345";
    static constexpr double kNominalDbfs = -18.0;  ///< +4 dBu, 0 VU
    static constexpr double kDbuToDbfs = -22.0;    ///< dBFS = dBu - 22
    static constexpr double kMaxOutputDbu = 24.0;  ///< Amplifier Type B into 600 ohms
    static constexpr double kPanPotOhms = 11000.0;
    static constexpr double kMixingOhms = 22000.0;
    static constexpr double kAttackSeconds = 0.001;
    static constexpr double kCompRatio = 2.0;
    static constexpr double kLimitRatio = 7.0;
    static constexpr double kDimDb = -20.0;
    static constexpr double kGlideSeconds = 0.005; ///< switches and faders move without clicks
    static constexpr int kMaxLoopLatency = 16384;

    enum Cassette
    {
        microphone = 0,
        group,
        mainMix,
        echoAux,
        monitor
    };
    enum Mode
    {
        out = 0,
        comp,
        limit
    };

    static constexpr std::array<double, 9> kGainSteps{-18.0, -12.0, -6.0, 0.0, 6.0, 12.0, 18.0, 24.0, 30.0};
    static constexpr std::array<double, 5> kFineSteps{-4.0, -2.0, 0.0, 2.0, 4.0};
    static constexpr std::array<double, 6> kRecoverySeconds{0.1, 0.25, 0.5, 1.0, 2.0, 5.0};
    static constexpr std::array<double, 8> kPresenceHz{500.0, 800.0, 1200.0, 1800.0, 2800.0, 4200.0, 6500.0, 10000.0};
    static constexpr int kEqSteps = 11; ///< -10 .. +10 dB in 2 dB steps
    static constexpr double kBassHz = 50.0, kTrebleBoostHz = 5000.0, kTrebleCutHz = 10000.0;

    /** An amplifier card: its rails (dBu of a sine at the rails) and its residual curvature (the
        2nd and 3rd harmonic terms at the rails). */
    struct AmpCard
    {
        const char* name;
        double railDbu;
        double second;
        double third;
    };
    static constexpr AmpCard kTypeA{"Type A", 26.0, 0.004, 0.04};
    static constexpr AmpCard kTypeB{"Type B", 24.0, 0.002, 0.04};
    static constexpr AmpCard kTypeC1{"Type C1", 28.0, 0.003, 0.03};
    static constexpr AmpCard kTypeE{"Type E", 26.0, 0.001, 0.01};

    /** A card's output for `x` (dBFS-linear). */
    [[nodiscard]] static double amplify(double x, const AmpCard& card) noexcept;
    /** The pan-pots: (left, right) gains for pan -1 (left) .. +1 (right). */
    [[nodiscard]] static std::pair<double, double> panGains(double pan) noexcept;
    [[nodiscard]] static double eqStepDb(int index) noexcept { return -10.0 + 2.0 * index; }
    [[nodiscard]] static juce::StringArray cassetteNames();

    ConsoleTG12345();
    ~ConsoleTG12345() override;

    void setNonRealtime(bool isNonRealtime) noexcept override;

    // InsertLoopHost: the echo send (Microphone, in parallel) or the echo chamber (Echo / Aux, in series).
    [[nodiscard]] bool isLoopEngaged() const noexcept override { return isLoopAvailable(); }
    [[nodiscard]] bool isLoopAvailable() const noexcept override;
    [[nodiscard]] bool isLoopInSeries() const noexcept override { return cassette_->getIndex() == echoAux; }
    void processSend(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;
    void processReturn(juce::AudioBuffer<double>& buffer, int loopLatency) noexcept override;
    [[nodiscard]] juce::String getLoopName() const override;
    [[nodiscard]] juce::String getReturnDescription() const override;

    // --- Displays --------------------------------------------------------------------------------
    [[nodiscard]] int getCassette() const noexcept { return cassette_->getIndex(); }
    [[nodiscard]] bool isMarkTwo() const noexcept { return mark_->getIndex() == 1; }
    /** The compressor's gain reduction now (dB, 0 or more). */
    [[nodiscard]] float getGainReductionDb() const noexcept { return grDisplay_.load(std::memory_order_relaxed); }
    /** The output's peak level (dBu) per channel since the last call. */
    [[nodiscard]] float takeOutputPeakDbu(int channel) noexcept
    {
        return outputPeak_[static_cast<std::size_t>(channel & 1)].exchange(-100.0f, std::memory_order_acq_rel);
    }
    /** The outputs' phase correlation (-1 .. +1). */
    [[nodiscard]] float getCorrelation() const noexcept { return correlation_.load(std::memory_order_relaxed); }
    [[nodiscard]] int currentLatency() const noexcept;
    [[nodiscard]] int latencyFor(bool offline) const noexcept;

    // --- Controls --------------------------------------------------------------------------------
    juce::AudioParameterChoice* cassette_ = nullptr;
    juce::AudioParameterChoice* mark_ = nullptr;  ///< Mk I, Mk II
    juce::AudioParameterChoice* input_ = nullptr; ///< Stereo Pair, Mono
    juce::AudioParameterChoice* gain_ = nullptr;  ///< 6 dB stud switch
    juce::AudioParameterChoice* fine_ = nullptr;  ///< 2 dB stud switch
    juce::AudioParameterChoice* mode_ = nullptr;  ///< Out, Comp, Limit
    juce::AudioParameterFloat* hold_ = nullptr;   ///< dB re +4 dBu
    juce::AudioParameterChoice* recovery_ = nullptr;
    juce::AudioParameterBool* link_ = nullptr;
    juce::AudioParameterChoice* bass_ = nullptr;
    juce::AudioParameterChoice* treble_ = nullptr;
    juce::AudioParameterChoice* presenceFrequency_ = nullptr;
    juce::AudioParameterChoice* presence_ = nullptr;
    juce::AudioParameterFloat* panA_ = nullptr;   ///< -100 (left) .. 100 (right)
    juce::AudioParameterFloat* panB_ = nullptr;
    juce::AudioParameterFloat* spread_ = nullptr; ///< %
    juce::AudioParameterFloat* fader_ = nullptr;  ///< dB (-80 = off)
    juce::AudioParameterFloat* echoSend_ = nullptr;
    juce::AudioParameterBool* echoPre_ = nullptr;
    juce::AudioParameterFloat* echoReturn_ = nullptr;
    juce::AudioParameterBool* announce_ = nullptr;
    juce::AudioParameterChoice* announceSource_ = nullptr; ///< Talkback Mic, Tone 1 kHz
    juce::AudioParameterFloat* announceLevel_ = nullptr;
    juce::AudioParameterChoice* listen_ = nullptr; ///< Control Room, Studio
    juce::AudioParameterFloat* controlRoom_ = nullptr;
    juce::AudioParameterBool* dim_ = nullptr;
    juce::AudioParameterBool* mono_ = nullptr;
    juce::AudioParameterBool* cut_ = nullptr;
    juce::AudioParameterFloat* studio_ = nullptr;
    juce::AudioParameterBool* talkback_ = nullptr;
    juce::AudioParameterFloat* talkbackLevel_ = nullptr;
    juce::AudioParameterChoice* osRealtime_ = nullptr;
    juce::AudioParameterChoice* osOffline_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    /** A value that glides to its target (switches and faders without zipper noise). */
    struct Glide
    {
        double value = 0.0, target = 0.0;
        double next(double coefficient) noexcept { return value += (target - value) * coefficient; }
    };
    /** A first-order section (the passive shelving networks). */
    struct FirstOrder
    {
        double b0 = 1.0, b1 = 0.0, a1 = 0.0;
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
    /** A transformer's low-frequency limit and core: the flux (the integral of the voltage)
        saturates, so loud low frequencies distort first. */
    struct TransformerModel
    {
        dsp::BiquadCoefficients highPass;
        double leak = 0.0, integrate = 0.0, saturation = 0.0;
    };
    struct Transformer
    {
        dsp::Biquad highPass;
        double flux = 0.0;
        double process(double x, const TransformerModel& model) noexcept
        {
            x = highPass.process(x, model.highPass);
            flux = flux * model.leak + x * model.integrate;
            return x - model.saturation * flux * flux * flux;
        }
    };
    /** One channel's EQ networks. */
    struct EqChannel
    {
        FirstOrderState bass, trebleCut;
        dsp::Biquad trebleBoost, presence;
    };

    void parameterValueChanged(int index, float value) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] int factorChoice() const noexcept;
    /** Reads the controls: targets, filters, the oversampling factor. */
    void update(double rate) noexcept;
    /** Runs the selected cassette on the block in place (stereo). Echo: with `captureEcho`, the
        Microphone cassette's echo send is left in echo_ (base rate). */
    void runCassette(double* left, double* right, int n, const double* sidechainLeft, const double* sidechainRight,
                     bool captureEcho) noexcept;
    /** The compressor / limiter on a pair of samples (feedback: the detector hears the output). */
    void dynamics(double& a, double& b, bool stereo) noexcept;
    double equalise(EqChannel& channel, double x) noexcept;
    void meter(const double* left, const double* right, int n) noexcept;
    [[nodiscard]] static FirstOrder shelf(bool high, double rate, double turnoverHz, double calibrationHz, double gainDb) noexcept;

    dsp::PhaseOversampler oversampler_, echoOversampler_;
    std::array<Transformer, 2> inputTransformer_, outputTransformer_;
    TransformerModel micModel_, outputModel_;
    std::array<EqChannel, 2> eq_;
    FirstOrder bassShelf_, trebleCutShelf_;
    dsp::BiquadCoefficients trebleBoostBell_, presenceBell_;
    std::array<double, 2> gr_{0.0, 0.0};
    std::array<double, 2> rectified_{0.0, 0.0}; ///< the rectifier's control voltage (charges at once, ripple decay)
    double ripple_ = 0.0;
    double attack_ = 1.0, release_ = 0.0, ratio_ = 1.0, thresholdDb_ = 0.0, thresholdLinear_ = 1.0;
    int dynamicsMode_ = out;
    bool linked_ = true;
    double glide_ = 1.0, baseGlide_ = 1.0, rate_ = 0.0;
    /** Glides at the processing rate (in the cassette) ... */
    Glide gIn_, gFader_, gSpread_, gEchoSend_, gPanAL_, gPanAR_, gPanBL_, gPanBR_, gReturnMaster_, gSendReturn_;
    /** ... and at the base rate (before and after it). */
    Glide bEchoReturn_, bSendMaster_, bAnnounce_, bProgram_, bTalkback_, bMono_;
    std::array<float, 8> eqSignature_{};
    double tonePhase_ = 0.0;
    std::vector<double> echoOversampled_;           ///< the echo send at the processing rate
    std::array<std::vector<double>, 2> echoBase_;   ///< ... and back at the base rate
    std::array<std::vector<double>, 2> mainRing_;   ///< a parallel loop's host signal, delayed for the loop
    int ringWrite_ = 0;
    double sumLL_ = 0.0, sumRR_ = 0.0, sumLR_ = 0.0;
    std::atomic<float> grDisplay_{0.0f};
    std::array<std::atomic<float>, 2> outputPeak_{};
    std::atomic<float> correlation_{1.0f};
};

} // namespace dojo::devices

#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <utility>

namespace dojo::devices
{

/**
    The Harrison 4032 (1975 - 1984): an in-line console of 32 buses and 40 (to 56) inputs, as one
    device: a module of the frame, picked with Module, built as one of its three revisions.

    Levels: +4 dBu = -18 dBFS. The microphone input takes the track as a microphone would: -18 dBFS
    is -36 dBu, so MIC GAIN +40 dB gives +4 dBu back. The line input and the tape return take the
    track at line level (+4 dBu).

    Revisions (Revision): what each one's circuits do differently.
    - A (1975 - 77): LM301A op-amps after discrete differential pairs, +-15 V rails (+22 dBu out),
      a slew-limited top end at high levels; HF and LF bands peaking only, HMF / LMF at a fixed Q
      of 1.5; the filters switched in with EQ IN; dbx 202 / Allison EGC-101 VCAs (some control
      voltage feedthrough: fast fader moves thump); inverting bus summing (crosstalk into the
      neighbouring buses about -62 dB).
    - B (1977 - 79): TL071 / TL072 FET op-amps, +-18 V (+24.5 dBu), lower distortion at high
      levels; Shelf / Peak switches on HF and LF; differential summing (crosstalk about -85 dB).
    - C (1979 - 84): NE5534 in the gain, summing and driver stages (the lowest noise and
      distortion), +26 dBu; continuously variable Q (0.5 - 3.0) on HMF and LMF; a 3-pole 18 dB/oct
      HPF from 20 Hz (12 dB/oct from 25 Hz before); a FILTERS IN switch of its own; dbx 2150 VCAs
      with better control-voltage isolation.

    Modules:
    - I/O Module (in-line): two paths. The channel path takes the microphone (Jensen JT-115-K-E
      1:10 transformer, PAD -20 dB, polarity, +48 V; MIC GAIN +10 to +60 dB) or the line input
      (LINE TRIM +-10 dB): the track. The monitor path takes the tape return: the device's sidechain
      input. FLIP swaps them (tracking / mixdown). The channel path: HPF (12 or 18 dB/oct) and LPF
      (12 dB/oct), the 4-band proportional-Q EQ (LF 40 - 800 Hz, LMF 80 Hz - 1.5 kHz, HMF 800 Hz -
      15 kHz, HF 800 Hz - 18 kHz, each +-15 dB), the VCA fader, the pan pot (stereo, quad
      front / back or LCRS) to the mix; the monitor path: its level and pan to the mix.
      Sends, on the device's extra outputs (to be routed to busses): 32 bus assignments (with
      odd / even panning), AUX 1 - 4 (pre or post fader), CUE 1 - 2 (level and pan, from the monitor
      or the channel path), the quad bus's channels 3 - 4 and the direct output.
    - Bus Master: a program sub-group, aux or cue master on its bus: the summing amplifier, the
      master fader, the line driver (and the optional Jensen output transformer).
    - Master Section: the mix's summing amplifier, master fader and line drivers, and the control
      room monitor matrix: the mix, an external source (the sidechain input: a tape, bus, 2-track
      or aux return) or both (a quad fold-down), Mono, its level. Monitoring is for listening:
      offline exports leave the matrix out.

    Measured (C, everything flat): 20 Hz - 20 kHz within 0.25 dB, under 0.02 % THD at +4 dBu,
    EIN -128 dBu (Noise on). No latency.
*/
class Harrison4032 final : public BuiltinProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    static constexpr const char* kTypeId = "builtin:harrison4032";
    static constexpr double kNominalDbfs = -18.0;    ///< +4 dBu
    static constexpr double kDbuToDbfs = -22.0;      ///< dBFS = dBu - 22
    static constexpr double kMicOffsetDb = 18.0;     ///< at the mic input: dBFS = dBu + 18 (-36 dBu = -18 dBFS)
    static constexpr double kMicReferenceGainDb = 40.0;
    static constexpr double kPadDb = -20.0;
    static constexpr double kStepUpDb = 20.0;        ///< the Jensen JT-115-K-E's 1:10
    static constexpr double kGlideSeconds = 0.005;
    static constexpr double kFlipSeconds = 0.003;    ///< (CMOS switching, without clicks)
    static constexpr double kEqRangeDb = 15.0;
    static constexpr double kFixedMidQ = 1.5;
    static constexpr double kBellQ = 0.9;            ///< HF / LF bells (at full boost or cut)
    static constexpr double kHpfMaxHz = 3150.0, kLpfMinHz = 160.0, kLpfMaxHz = 20000.0;
    static constexpr int kBuses = 32;

    enum Module
    {
        ioModule = 0,
        busMaster,
        masterSection
    };
    enum Revision
    {
        revisionA = 0,
        revisionB,
        revisionC
    };
    enum PanMode
    {
        stereoPan = 0,
        quadPan,
        lcrsPan
    };
    enum MonitorSource
    {
        monitorMix = 0,
        monitorExternal,
        monitorBoth
    };

    /** The extra outputs (after the main pair): their channels. */
    struct Outputs
    {
        static constexpr int bus(int n) noexcept { return n - 1; } ///< 1 .. 32, one channel each
        static constexpr int aux(int n) noexcept { return 31 + n; } ///< 1 .. 4: 32 .. 35, one channel each
        static constexpr int cue(int n) noexcept { return 36 + 2 * (n - 1); } ///< left (right: + 1)
        static constexpr int quad = 40;   ///< the quad bus's channels 3 and 4 (rear L / R, or C / S)
        static constexpr int direct = 42; ///< (and 43, the same)
        static constexpr int count = 44;
    };

    /** What a revision's circuits are. */
    struct RevisionSpec
    {
        const char* name;
        const char* years;
        const char* opAmps;
        const char* vca;
        double railVolts;
        double maxOutputDbu;
        double second;        ///< an op-amp stage's 2nd-harmonic curvature at its rails
        double third;         ///< ... and 3rd
        double slewVoltsPerUs;
        double einDbu;
        double vcaCurvature;  ///< the VCA's 2nd-harmonic curvature
        double feedthrough;   ///< control-voltage feedthrough, per dB of fader movement
        double crosstalkDb;   ///< from a bus into its neighbours
        bool shelves;         ///< Shelf / Peak switches on HF and LF
        bool variableQ;       ///< on HMF and LMF
        bool filtersSwitch;   ///< FILTERS IN of its own (else EQ IN switches them)
        int hpfPoles;
        double hpfMinHz;
    };
    [[nodiscard]] static const RevisionSpec& revisionSpec(int revision) noexcept;
    [[nodiscard]] static juce::StringArray moduleNames();

    /** The pan pot (mono): constant power, -3 dB in the centre. */
    [[nodiscard]] static std::pair<double, double> panGains(double pan) noexcept;
    /** The EQ and filters as set now: their response (dB) at `hz` (the display). */
    [[nodiscard]] double responseDb(double hz) const;
    /** Proportional Q: a band's Q at `gainDb` for its Q at full boost or cut. */
    [[nodiscard]] static double proportionalQ(double q, double gainDb) noexcept;

    Harrison4032();
    ~Harrison4032() override;

    // --- Displays --------------------------------------------------------------------------------
    [[nodiscard]] int getModule() const noexcept { return module_->getIndex(); }
    [[nodiscard]] int getRevision() const noexcept { return revision_->getIndex(); }
    [[nodiscard]] float takeOutputPeakDbu(int channel) noexcept
    {
        return outputPeak_[static_cast<std::size_t>(channel & 1)].exchange(-100.0f, std::memory_order_acq_rel);
    }
    /** Whether the filters are in the signal path now (A / B: with EQ IN). */
    [[nodiscard]] bool filtersEngaged() const noexcept;

    // --- Controls --------------------------------------------------------------------------------
    juce::AudioParameterChoice* module_ = nullptr;
    juce::AudioParameterChoice* revision_ = nullptr;
    juce::AudioParameterChoice* format_ = nullptr; ///< Stereo (a linked pair of modules), Mono
    juce::AudioParameterChoice* input_ = nullptr;  ///< Mic, Line
    juce::AudioParameterFloat* micGain_ = nullptr;
    juce::AudioParameterFloat* lineTrim_ = nullptr;
    juce::AudioParameterBool* pad_ = nullptr;
    juce::AudioParameterBool* phase_ = nullptr;
    juce::AudioParameterBool* phantom_ = nullptr;
    juce::AudioParameterBool* flip_ = nullptr;
    juce::AudioParameterFloat* hpf_ = nullptr;
    juce::AudioParameterFloat* lpf_ = nullptr;
    juce::AudioParameterBool* filtersIn_ = nullptr;
    juce::AudioParameterFloat* lfFreq_ = nullptr;
    juce::AudioParameterFloat* lfGain_ = nullptr;
    juce::AudioParameterBool* lfShelf_ = nullptr;
    juce::AudioParameterFloat* lmfFreq_ = nullptr;
    juce::AudioParameterFloat* lmfGain_ = nullptr;
    juce::AudioParameterFloat* lmfQ_ = nullptr;
    juce::AudioParameterFloat* hmfFreq_ = nullptr;
    juce::AudioParameterFloat* hmfGain_ = nullptr;
    juce::AudioParameterFloat* hmfQ_ = nullptr;
    juce::AudioParameterFloat* hfFreq_ = nullptr;
    juce::AudioParameterFloat* hfGain_ = nullptr;
    juce::AudioParameterBool* hfShelf_ = nullptr;
    juce::AudioParameterBool* eqIn_ = nullptr;
    juce::AudioParameterFloat* fader_ = nullptr; ///< dB (-80 = off): the VCA fader, bus or master fader
    juce::AudioParameterFloat* pan_ = nullptr;   ///< -100 .. 100
    juce::AudioParameterChoice* panMode_ = nullptr;
    juce::AudioParameterFloat* frontBack_ = nullptr; ///< -100 front .. 100 back (LCRS: the surround)
    juce::AudioParameterBool* mix_ = nullptr;
    juce::AudioParameterFloat* monitorLevel_ = nullptr;
    juce::AudioParameterFloat* monitorPan_ = nullptr;
    std::array<juce::AudioParameterFloat*, 4> aux_{};
    std::array<juce::AudioParameterBool*, 4> auxPre_{};
    std::array<juce::AudioParameterFloat*, 2> cue_{};
    std::array<juce::AudioParameterFloat*, 2> cuePan_{};
    juce::AudioParameterChoice* cueFrom_ = nullptr; ///< Monitor, Channel
    std::array<juce::AudioParameterBool*, kBuses> bus_{};
    juce::AudioParameterBool* busPan_ = nullptr; ///< pan between odd and even buses
    juce::AudioParameterBool* direct_ = nullptr;
    juce::AudioParameterChoice* monitorSource_ = nullptr;
    juce::AudioParameterFloat* monitorVolume_ = nullptr;
    juce::AudioParameterBool* mono_ = nullptr;
    juce::AudioParameterBool* transformer_ = nullptr; ///< the optional Jensen output transformer
    juce::AudioParameterBool* noise_ = nullptr;

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
    /** An op-amp gain stage: its rails, its curvature and (LM301A) its slew rate. */
    struct StageModel
    {
        double rail = 1.0, second = 0.0, third = 0.0, maxStep = 1.0e9;
    };
    struct Stage
    {
        double previous = 0.0;
        double process(double x, const StageModel& model) noexcept;
    };
    /** A transformer's core: its flux saturates in the deep lows at high levels. */
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
    /** The filters and EQ of the current settings. */
    struct Design
    {
        std::array<dsp::BiquadCoefficients, 2> highPass; ///< (18 dB/oct: a first- and a second-order section)
        dsp::BiquadCoefficients lowPass;
        std::array<dsp::BiquadCoefficients, 4> bands; ///< LF, LMF, HMF, HF
    };
    struct EqChannel
    {
        std::array<dsp::Biquad, 2> highPass;
        dsp::Biquad lowPass;
        std::array<dsp::Biquad, 4> bands;
        Stage filterStage, eqStage;
    };
    /** One side (L or R, or the mono module) of the I/O module's circuits. */
    struct Side
    {
        Transformer mic;
        Stage micPreamp, lineReceiver, tapeReceiver, monitorStage;
        EqChannel eq;
        double vcaPrevious = 0.0;
    };

    void parameterValueChanged(int index, float value) override;
    void parameterGestureChanged(int, bool) override {}
    [[nodiscard]] Design design(double rate) const;
    void update() noexcept;
    void runModule(juce::AudioBuffer<double>& buffer) noexcept;
    void runBusMaster(double* left, double* right, int n) noexcept;
    void runMasterSection(double* left, double* right, const double* externalLeft, const double* externalRight, int n) noexcept;
    void meter(const double* left, const double* right, int n) noexcept;
    [[nodiscard]] double noiseSample() noexcept;

    Design design_;
    std::array<float, 20> designSignature_{};
    std::array<Side, 2> sides_;
    std::array<Transformer, 2> outputTransformer_;
    TransformerModel micModel_, outputModel_;
    std::array<Stage, 2> summing_, driver_;
    StageModel stageModel_;
    double vcaCurvature_ = 0.0, feedthrough_ = 0.0, crosstalk_ = 0.0;
    double glide_ = 1.0, flipGlide_ = 1.0, feedthroughDecay_ = 0.0;
    Glide gGain_, gTrim_, gFader_, gPanAL_, gPanAR_, gPanBL_, gPanBR_, gMonL_, gMonR_, gFront_, gRear_, gFlip_, gMix_;
    Glide gFiltersIn_, gEqIn_, gMonitorVolume_, gMono_, gExternal_, gProgram_, gLine_;
    std::array<Glide, 4> gAux_;
    std::array<Glide, 4> gCueL_, gCueR_;
    std::array<Glide, kBuses> gBus_;
    double faderDbPrevious_ = 0.0, thump_ = 0.0;
    bool phantomWas_ = false;
    double pop_ = 0.0, popDecay_ = 0.0;
    std::uint32_t noiseState_ = 0x9e3779b9u;
    double noiseSpare_ = 0.0;
    bool noiseHasSpare_ = false;
    std::array<double, kBuses> busScratch_{};
    juce::AudioBuffer<double> sidechain_;
    std::array<std::atomic<float>, 2> outputPeak_{};
};

} // namespace dojo::devices

#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/AnalogFilters.h"
#include "devices/dsp/Biquad.h"
#include "devices/dsp/Modulation.h"
#include "devices/dsp/Oversampler.h"

#include <array>
#include <atomic>

namespace dojo::devices
{

/**
    Roar: a three-stage colouring and saturation effect, from mastering-grade warmth to wild
    degradation.

    - Input: Drive into the stages and a Tone tilt (darker <-> brighter).
    - Three stages, each with a shape (Soft Sine, Diode Clipper, Tube Preamp, Polynomial, Hard Clip,
      Rectifier, Tri Fold, Sine Fold, Bit Crusher, Fractal, Noise Injection), Amount (drive into the
      shape), Bias (asymmetry: even harmonics, sputter), a filter before or after the shape (low-,
      high-, band-pass, notch or peak, with resonance) and a level.
    - Routing: Single (stage 1), Serial (1 -> 2 -> 3), Parallel (the stages side by side, averaged),
      Mid/Side (stage 1 on the mid, stage 2 on the side) or Multiband (stage 1 lows, 2 mids, 3 highs,
      split at two crossovers).
    - Modulation: two LFOs (free or synced), an envelope follower (the input's level) and a smoothed
      random source, routed by an 8-slot matrix to the drive, tone, every stage's amount, bias and
      filter frequency, the feedback and the dry/wet.
    - Feedback: the output fed back to the input through a delay (time or synced) and a band-pass,
      optionally inverted: from resonant metallic tones to spiralling echoes.
    - A glue compressor, output gain and dry/wet (latency-aligned). The stages run 2x oversampled.
*/
class Roar final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:roar";
    static constexpr int kStages = 3;
    static constexpr int kMatrixSlots = 8;
    static constexpr int kChunk = 32; ///< modulation rate, and the feedback's shortest delay
    static constexpr double kMaxFeedbackSeconds = 2.0;

    enum class Routing
    {
        single,
        serial,
        parallel,
        midSide,
        multiband,
    };

    enum class Shape
    {
        softSine,
        diodeClipper,
        tubePreamp,
        polynomial,
        hardClip,
        rectifier,
        triFold,
        sineFold,
        bitCrusher,
        fractal,
        noiseInjection,
    };

    enum Source
    {
        srcOff,
        srcLfo1,
        srcLfo2,
        srcEnvelope,
        srcNoise,
        numSources
    };

    enum Destination
    {
        dstOff,
        dstDrive,
        dstTone,
        dstStage1Amount,
        dstStage1Bias,
        dstStage1Frequency,
        dstStage2Amount,
        dstStage2Bias,
        dstStage2Frequency,
        dstStage3Amount,
        dstStage3Bias,
        dstStage3Frequency,
        dstFeedback,
        dstFeedbackFrequency,
        dstMix,
        numDestinations
    };

    Roar();

    static const juce::StringArray& getShapeNames();
    static const juce::StringArray& getSourceNames();
    static const juce::StringArray& getDestinationNames();
    /** One shape's transfer curve (noise: the Noise Injection shape's random value, -1..1). */
    [[nodiscard]] static double shape(Shape shape, double x, double noise = 0.0) noexcept;

    /** Editor: each source's current value (-1..1; the envelope 0..1). */
    [[nodiscard]] float getSourceValue(int source) const noexcept
    {
        return source > srcOff && source < numSources ? sourceValues_[static_cast<std::size_t>(source)].load(std::memory_order_relaxed) : 0.0f;
    }
    [[nodiscard]] float takeCompressionDb() noexcept { return compression_.exchange(0.0f, std::memory_order_acq_rel); }

    struct Stage
    {
        juce::AudioParameterBool* on = nullptr;
        juce::AudioParameterChoice* shape = nullptr;
        juce::AudioParameterFloat* amount = nullptr;
        juce::AudioParameterFloat* bias = nullptr;
        juce::AudioParameterChoice* filter = nullptr; ///< Off, Low Pass, High Pass, Band Pass, Notch, Peak
        juce::AudioParameterBool* filterPre = nullptr;
        juce::AudioParameterFloat* frequency = nullptr;
        juce::AudioParameterFloat* resonance = nullptr;
        juce::AudioParameterFloat* level = nullptr;
    };
    struct LfoParameters
    {
        juce::AudioParameterChoice* shape = nullptr;
        juce::AudioParameterFloat* rate = nullptr;
        juce::AudioParameterBool* sync = nullptr;
        juce::AudioParameterChoice* division = nullptr;
    };
    struct MatrixSlot
    {
        juce::AudioParameterChoice* source = nullptr;
        juce::AudioParameterChoice* destination = nullptr;
        juce::AudioParameterFloat* amount = nullptr;
    };

    juce::AudioParameterFloat* drive_ = nullptr;
    juce::AudioParameterFloat* tone_ = nullptr;
    juce::AudioParameterChoice* routing_ = nullptr;
    juce::AudioParameterFloat* crossLow_ = nullptr;
    juce::AudioParameterFloat* crossHigh_ = nullptr;
    std::array<Stage, kStages> stages_;
    std::array<LfoParameters, 2> lfos_;
    juce::AudioParameterFloat* envAttack_ = nullptr;
    juce::AudioParameterFloat* envRelease_ = nullptr;
    juce::AudioParameterFloat* envGain_ = nullptr;
    juce::AudioParameterFloat* noiseRate_ = nullptr;
    juce::AudioParameterFloat* noiseSmooth_ = nullptr;
    std::array<MatrixSlot, kMatrixSlots> matrix_;
    juce::AudioParameterFloat* feedback_ = nullptr;
    juce::AudioParameterFloat* feedbackTime_ = nullptr;
    juce::AudioParameterBool* feedbackSync_ = nullptr;
    juce::AudioParameterChoice* feedbackDivision_ = nullptr;
    juce::AudioParameterFloat* feedbackFrequency_ = nullptr;
    juce::AudioParameterBool* feedbackInvert_ = nullptr;
    juce::AudioParameterFloat* compressor_ = nullptr;
    juce::AudioParameterFloat* output_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct StageState
    {
        bool on = false;
        Shape shape = Shape::softSine;
        double gain = 1.0, makeup = 1.0, bias = 0.0, rest = 0.0, level = 1.0;
        int filter = 0;
        bool pre = false;
    };

    double runStage(std::size_t stage, std::size_t channel, double x) noexcept;

    dsp::Oversampler oversampler_;
    std::array<StageState, kStages> state_{};
    std::array<std::array<dsp::NonlinearSvf, 2>, kStages> stageFilters_; ///< [stage][channel]
    std::array<std::array<dsp::Biquad, 2>, 2> tilt_;                      ///< [channel][low, high shelf]
    std::array<dsp::LinkwitzRiley, 2> lowSplit_, highSplit_, lowAlign_;   ///< multiband, per channel
    std::array<dsp::DelayLine, 2> feedbackLine_;
    std::array<dsp::NonlinearSvf, 2> feedbackFilter_;
    std::array<dsp::DcBlocker, 2> dc_;
    std::array<dsp::ModLfo, 2> lfo_;
    dsp::EnvelopeFollower follower_;
    dsp::Noise noise_, shapeNoise_;
    double noiseTarget_ = 0.0, noiseValue_ = 0.0, noisePhase_ = 0.0;
    double compressorEnvelope_ = 0.0, compressorGainDb_ = 0.0;
    std::array<std::atomic<float>, numSources> sourceValues_{};
    std::atomic<float> compression_{0.0f};
};

} // namespace dojo::devices

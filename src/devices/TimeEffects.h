#pragma once

#include "devices/BuiltinProcessor.h"
#include "devices/dsp/Biquad.h"

#include <array>
#include <memory>
#include <vector>

namespace dojo::devices
{

/**
    Algorithmic stereo reverb: pre-delay, input diffusion (four series all-passes) and an eight-line
    feedback delay network with a Householder matrix. Each line's feedback gain is derived from the
    decay time (RT60), and a one-pole low-pass per line damps the highs as the tail decays.
*/
class Reverb final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:reverb";
    static constexpr int kLines = 8;

    Reverb();
    double getTailLengthSeconds() const override;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct DelayLine
    {
        std::vector<double> data;
        int write = 0;
        int length = 1;

        double read() const noexcept { return data[static_cast<std::size_t>((write - length + static_cast<int>(data.size())) % static_cast<int>(data.size()))]; }
        void push(double x) noexcept
        {
            data[static_cast<std::size_t>(write)] = x;
            write = (write + 1) % static_cast<int>(data.size());
        }
    };

    struct AllPass
    {
        std::vector<double> data;
        int index = 0;
        double process(double x, double g) noexcept
        {
            const double delayed = data[static_cast<std::size_t>(index)];
            const double v = x + g * delayed;
            data[static_cast<std::size_t>(index)] = v;
            index = (index + 1) % static_cast<int>(data.size());
            return delayed - g * v;
        }
    };

    void updateParameters() noexcept;

    juce::AudioParameterFloat* size_ = nullptr;
    juce::AudioParameterFloat* decay_ = nullptr;
    juce::AudioParameterFloat* damping_ = nullptr;
    juce::AudioParameterFloat* predelay_ = nullptr;
    juce::AudioParameterFloat* lowCut_ = nullptr;
    juce::AudioParameterFloat* width_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

    std::array<DelayLine, kLines> lines_;
    std::array<double, kLines> lineGain_{};
    std::array<double, kLines> damp_{};
    std::array<AllPass, 4> diffusers_;
    DelayLine predelay_line_;
    double dampCoefficient_ = 0.5;
    double lowCutState_ = 0.0;
    double lowCutCoefficient_ = 0.0;
    float lastSize_ = -1.0f;
};

/**
    Stereo / ping-pong delay with tempo sync (1/32 .. 1/1, dotted and triplet values), feedback
    filters (low cut, high cut) and smoothed delay-time changes.
*/
class Delay final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:delay";
    static constexpr double kMaxSeconds = 4.0;

    Delay();
    double getTailLengthSeconds() const override { return kMaxSeconds * 4.0; }

    /** Beats per division choice (1/4 = 1 beat). */
    static double beatsForDivision(int index) noexcept;
    static const juce::StringArray& divisionNames();

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    double targetDelaySeconds() const noexcept;
    double readInterpolated(const std::vector<double>& line, double delaySamples) const noexcept;

    juce::AudioParameterBool* sync_ = nullptr;
    juce::AudioParameterChoice* division_ = nullptr;
    juce::AudioParameterFloat* time_ = nullptr;
    juce::AudioParameterFloat* feedback_ = nullptr;
    juce::AudioParameterBool* pingPong_ = nullptr;
    juce::AudioParameterFloat* lowCut_ = nullptr;
    juce::AudioParameterFloat* highCut_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;

    std::vector<double> lines_[2];
    int write_ = 0;
    double currentDelay_ = 0.0; ///< samples, smoothed
    std::array<dsp::Biquad, 2> lowCutFilter_;
    std::array<dsp::Biquad, 2> highCutFilter_;
};

/**
    Real-time pitch shifter (Signalsmith Stretch): transposes by semitones + cents without changing
    timing. Used on live input and instruments; clips use their own time-stretch / pitch settings.
*/
class PitchShifter final : public BuiltinProcessor
{
public:
    static constexpr const char* kTypeId = "builtin:pitch";

    PitchShifter();
    ~PitchShifter() override;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) override;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    juce::AudioParameterFloat* semitones_ = nullptr;
    juce::AudioParameterFloat* cents_ = nullptr;
    juce::AudioParameterFloat* mix_ = nullptr;
};

} // namespace dojo::devices

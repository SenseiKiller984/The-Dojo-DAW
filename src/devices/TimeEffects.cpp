#include "devices/TimeEffects.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr std::array<double, Reverb::kLines> kLineMs{29.7, 37.1, 41.1, 43.7, 53.3, 59.1, 67.3, 73.9};
constexpr std::array<double, 4> kDiffuserSamples44k{142.0, 107.0, 379.0, 277.0};
constexpr double kMaxSizeFactor = 2.0;
constexpr double kMaxPredelayMs = 250.0;
} // namespace

//==================================================================================================
// Reverb

Reverb::Reverb()
    : BuiltinProcessor(kTypeId, "Reverb",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    size_ = addFloat("size", "Size", {0.0f, 1.0f, 0.001f}, 0.6f);
    decay_ = addFloat("decay", "Decay", skewedRange(0.2f, 20.0f, 2.0f), 2.2f, "s");
    damping_ = addFloat("damping", "Damping", {0.0f, 1.0f, 0.001f}, 0.45f);
    predelay_ = addFloat("predelay", "Pre-delay", {0.0f, static_cast<float>(kMaxPredelayMs), 0.1f}, 12.0f, "ms");
    lowCut_ = addFloat("lowcut", "Low Cut", skewedRange(20.0f, 1000.0f, 150.0f), 120.0f, "Hz");
    width_ = addFloat("width", "Width", {0.0f, 1.0f, 0.001f}, 1.0f);
    mix_ = addFloat("mix", "Mix", {0.0f, 100.0f, 0.1f}, 30.0f, "%");
}

double Reverb::getTailLengthSeconds() const
{
    return decay_->get() + predelay_->get() / 1000.0;
}

void Reverb::prepareDsp(double sampleRate, int)
{
    const auto lineCapacity = static_cast<std::size_t>(std::ceil(kLineMs.back() * kMaxSizeFactor / 1000.0 * sampleRate)) + 8;
    for (auto& line : lines_)
    {
        line.data.assign(lineCapacity, 0.0);
        line.write = 0;
    }

    for (std::size_t i = 0; i < diffusers_.size(); ++i)
    {
        const auto length = static_cast<std::size_t>(std::max(1.0, std::round(kDiffuserSamples44k[i] * sampleRate / 44100.0)));
        diffusers_[i].data.assign(length, 0.0);
        diffusers_[i].index = 0;
    }

    predelay_line_.data.assign(static_cast<std::size_t>(std::ceil(kMaxPredelayMs / 1000.0 * sampleRate)) + 4, 0.0);
    predelay_line_.write = 0;
    damp_.fill(0.0);
    lowCutState_ = 0.0;
    lastSize_ = -1.0f;
    updateParameters();
}

void Reverb::updateParameters() noexcept
{
    const float size = size_->get();
    if (std::abs(size - lastSize_) > 0.002f)
    {
        const double factor = 0.5 + size * (kMaxSizeFactor - 0.5);
        for (std::size_t i = 0; i < lines_.size(); ++i)
        {
            const int capacity = static_cast<int>(lines_[i].data.size());
            lines_[i].length = std::clamp(static_cast<int>(std::lround(kLineMs[i] * factor / 1000.0 * sampleRate_)), 1, capacity - 2);
        }
        lastSize_ = size;
    }

    const double decay = std::max(0.05, static_cast<double>(decay_->get()));
    for (std::size_t i = 0; i < lines_.size(); ++i)
        lineGain_[i] = std::pow(10.0, -3.0 * (lines_[i].length / sampleRate_) / decay);

    const double cutoff = 20000.0 * std::pow(1500.0 / 20000.0, static_cast<double>(damping_->get()));
    dampCoefficient_ = 1.0 - std::exp(-2.0 * kPi * std::min(cutoff, sampleRate_ * 0.45) / sampleRate_);
    lowCutCoefficient_ = std::exp(-2.0 * kPi * lowCut_->get() / sampleRate_);

    const int capacity = static_cast<int>(predelay_line_.data.size());
    predelay_line_.length = std::clamp(static_cast<int>(std::lround(predelay_->get() / 1000.0 * sampleRate_)), 1, capacity - 2);
}

void Reverb::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    updateParameters();

    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const double wet = mix_->get() / 100.0;
    const double width = width_->get();
    constexpr double householder = 2.0 / kLines;
    constexpr std::array<double, kLines> inputSigns{1, -1, 1, -1, 1, -1, 1, -1};
    constexpr std::array<double, kLines> outputSigns{1, 1, -1, -1, 1, 1, -1, -1};

    for (int n = 0; n < numSamples; ++n)
    {
        const double input = 0.5 * (left[n] + right[n]);

        double x = predelay_line_.read();
        predelay_line_.push(input);

        lowCutState_ = x + (lowCutState_ - x) * lowCutCoefficient_;
        x -= lowCutState_;

        for (auto& diffuser : diffusers_)
            x = diffuser.process(x, 0.7);

        std::array<double, kLines> outputs{};
        std::array<double, kLines> feedback{};
        double sum = 0.0;
        for (std::size_t i = 0; i < lines_.size(); ++i)
        {
            outputs[i] = lines_[i].read();
            damp_[i] += dampCoefficient_ * (outputs[i] - damp_[i]);
            feedback[i] = damp_[i] * lineGain_[i];
            sum += feedback[i];
        }

        double wetLeft = 0.0;
        double wetRight = 0.0;
        for (std::size_t i = 0; i < lines_.size(); ++i)
        {
            lines_[i].push(feedback[i] - householder * sum + x * inputSigns[i] * 0.5);
            if (i % 2 == 0)
                wetLeft += outputs[i] * outputSigns[i];
            else
                wetRight += outputs[i] * outputSigns[i];
        }

        wetLeft *= 0.35;
        wetRight *= 0.35;
        const double mid = 0.5 * (wetLeft + wetRight);
        const double side = 0.5 * (wetLeft - wetRight) * width;

        left[n] = left[n] * (1.0 - wet) + (mid + side) * wet;
        right[n] = right[n] * (1.0 - wet) + (mid - side) * wet;
    }
}

//==================================================================================================
// Delay

const juce::StringArray& Delay::divisionNames()
{
    static const juce::StringArray names{"1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T", "1/4", "1/4D", "1/2", "1/1"};
    return names;
}

double Delay::beatsForDivision(int index) noexcept
{
    constexpr std::array<double, 12> beats{0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0, 1.0, 1.5, 2.0, 4.0};
    return beats[static_cast<std::size_t>(std::clamp(index, 0, 11))];
}

Delay::Delay()
    : BuiltinProcessor(kTypeId, "Delay",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    sync_ = addBool("sync", "Tempo Sync", true);
    division_ = addChoice("division", "Division", divisionNames(), 6); // 1/8 dotted
    time_ = addFloat("time", "Time", skewedRange(1.0f, static_cast<float>(kMaxSeconds * 1000.0), 300.0f), 375.0f, "ms");
    feedback_ = addFloat("feedback", "Feedback", {0.0f, 95.0f, 0.1f}, 40.0f, "%");
    pingPong_ = addBool("pingpong", "Ping-Pong", true);
    lowCut_ = addFloat("lowcut", "Low Cut", skewedRange(20.0f, 2000.0f, 200.0f), 120.0f, "Hz");
    highCut_ = addFloat("highcut", "High Cut", skewedRange(1000.0f, 20000.0f, 6000.0f), 8000.0f, "Hz");
    mix_ = addFloat("mix", "Mix", {0.0f, 100.0f, 0.1f}, 30.0f, "%");
}

double Delay::targetDelaySeconds() const noexcept
{
    double seconds = time_->get() / 1000.0;
    if (sync_->get())
    {
        double bpm = 120.0;
        if (auto* head = getPlayHead())
            if (const auto position = head->getPosition())
                if (const auto tempo = position->getBpm())
                    bpm = *tempo;
        seconds = beatsForDivision(division_->getIndex()) * 60.0 / std::max(20.0, bpm);
    }
    return std::clamp(seconds, 0.001, kMaxSeconds);
}

void Delay::prepareDsp(double sampleRate, int)
{
    const auto capacity = static_cast<std::size_t>(std::ceil(kMaxSeconds * sampleRate)) + 8;
    for (auto& line : lines_)
        line.assign(capacity, 0.0);
    write_ = 0;
    currentDelay_ = targetDelaySeconds() * sampleRate;
    for (auto& f : lowCutFilter_)
        f.reset();
    for (auto& f : highCutFilter_)
        f.reset();
}

double Delay::readInterpolated(const std::vector<double>& line, double delaySamples) const noexcept
{
    const int capacity = static_cast<int>(line.size());
    double position = static_cast<double>(write_) - delaySamples;
    while (position < 0.0)
        position += capacity;
    const auto index = static_cast<int>(position);
    const double fraction = position - index;
    const double a = line[static_cast<std::size_t>(index % capacity)];
    const double b = line[static_cast<std::size_t>((index + 1) % capacity)];
    return a + (b - a) * fraction;
}

void Delay::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    const double target = targetDelaySeconds() * sampleRate_;
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate_));
    const double feedback = feedback_->get() / 100.0;
    const double wet = mix_->get() / 100.0;
    const bool pingPong = pingPong_->get();
    const auto lowCut = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::lowCut, sampleRate_, lowCut_->get(), 0.707, 0.0);
    const auto highCut = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::highCut, sampleRate_, highCut_->get(), 0.707, 0.0);
    const int capacity = static_cast<int>(lines_[0].size());
    const double maxDelay = capacity - 4.0;

    for (int n = 0; n < numSamples; ++n)
    {
        currentDelay_ += (target - currentDelay_) * glide;
        const double delay = std::clamp(currentDelay_, 1.0, maxDelay);

        const double delayedLeft = readInterpolated(lines_[0], delay);
        const double delayedRight = readInterpolated(lines_[1], delay);
        const double filteredLeft = highCutFilter_[0].process(lowCutFilter_[0].process(delayedLeft, lowCut), highCut);
        const double filteredRight = highCutFilter_[1].process(lowCutFilter_[1].process(delayedRight, lowCut), highCut);

        double writeLeft = 0.0;
        double writeRight = 0.0;
        if (pingPong)
        {
            // Mono input enters on the left; each repeat crosses to the other side.
            writeLeft = 0.5 * (left[n] + right[n]) + feedback * filteredRight;
            writeRight = feedback * filteredLeft;
        }
        else
        {
            writeLeft = left[n] + feedback * filteredLeft;
            writeRight = right[n] + feedback * filteredRight;
        }

        lines_[0][static_cast<std::size_t>(write_)] = writeLeft;
        lines_[1][static_cast<std::size_t>(write_)] = writeRight;
        write_ = (write_ + 1) % capacity;

        left[n] = left[n] * (1.0 - wet) + delayedLeft * wet;
        right[n] = right[n] * (1.0 - wet) + delayedRight * wet;
    }
}

//==================================================================================================
// PitchShifter

struct PitchShifter::Impl
{
    signalsmith::stretch::SignalsmithStretch<float> stretch;
    std::vector<float> input[2];
    std::vector<float> output[2];
    std::vector<double> dry[2];
    int dryWrite = 0;
    int latency = 0;
};

PitchShifter::PitchShifter()
    : BuiltinProcessor(kTypeId, "Pitch Shift",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
    , impl_(std::make_unique<Impl>())
{
    semitones_ = addFloat("semitones", "Semitones", {-24.0f, 24.0f, 1.0f}, 0.0f, "st");
    cents_ = addFloat("cents", "Cents", {-100.0f, 100.0f, 1.0f}, 0.0f, "ct");
    mix_ = addFloat("mix", "Mix", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

PitchShifter::~PitchShifter() = default;

void PitchShifter::prepareDsp(double sampleRate, int maximumBlockSize)
{
    auto& s = *impl_;
    s.stretch.presetDefault(2, static_cast<float>(sampleRate));
    s.stretch.reset();
    s.latency = s.stretch.inputLatency() + s.stretch.outputLatency();

    for (int ch = 0; ch < 2; ++ch)
    {
        s.input[ch].assign(static_cast<std::size_t>(maximumBlockSize), 0.0f);
        s.output[ch].assign(static_cast<std::size_t>(maximumBlockSize), 0.0f);
        s.dry[ch].assign(static_cast<std::size_t>(s.latency + maximumBlockSize + 1), 0.0);
    }
    s.dryWrite = 0;
    setLatencySamples(s.latency);
}

void PitchShifter::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    auto& s = *impl_;
    const int numSamples = std::min(buffer.getNumSamples(), static_cast<int>(s.input[0].size()));
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};

    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < numSamples; ++i)
            s.input[ch][static_cast<std::size_t>(i)] = static_cast<float>(channels[ch][i]);

    s.stretch.setTransposeSemitones(semitones_->get() + cents_->get() / 100.0f);
    float* inputs[2] = {s.input[0].data(), s.input[1].data()};
    float* outputs[2] = {s.output[0].data(), s.output[1].data()};
    s.stretch.process(inputs, numSamples, outputs, numSamples);

    // The dry signal is delayed by the same latency, so the mix stays phase-aligned.
    const double wet = mix_->get() / 100.0;
    const int dryCapacity = static_cast<int>(s.dry[0].size());
    for (int i = 0; i < numSamples; ++i)
    {
        const int read = (s.dryWrite - s.latency + dryCapacity) % dryCapacity;
        for (int ch = 0; ch < 2; ++ch)
        {
            const double dry = s.dry[ch][static_cast<std::size_t>(read)];
            s.dry[ch][static_cast<std::size_t>(s.dryWrite)] = channels[ch][i];
            channels[ch][i] = dry * (1.0 - wet) + static_cast<double>(s.output[ch][static_cast<std::size_t>(i)]) * wet;
        }
        s.dryWrite = (s.dryWrite + 1) % dryCapacity;
    }
}

} // namespace dojo::devices

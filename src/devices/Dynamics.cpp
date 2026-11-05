#include "devices/Dynamics.h"

#include "devices/dsp/Biquad.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

//==================================================================================================
// Compressor

Compressor::Compressor()
    : BuiltinProcessor(kTypeId, "Compressor",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Sidechain", juce::AudioChannelSet::stereo(), true),
                       false)
{
    threshold_ = addFloat("threshold", "Threshold", {-60.0f, 0.0f, 0.1f}, -18.0f, "dB");
    ratio_ = addFloat("ratio", "Ratio", skewedRange(1.0f, 20.0f, 4.0f), 4.0f, ":1");
    attack_ = addFloat("attack", "Attack", skewedRange(0.1f, 100.0f, 10.0f), 10.0f, "ms");
    release_ = addFloat("release", "Release", skewedRange(5.0f, 1000.0f, 120.0f), 120.0f, "ms");
    knee_ = addFloat("knee", "Knee", {0.0f, 24.0f, 0.1f}, 6.0f, "dB");
    makeup_ = addFloat("makeup", "Makeup", {0.0f, 24.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Mix", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    external_ = addBool("sidechain", "External Sidechain", false);
}

double Compressor::gainComputer(double levelDb, double threshold, double ratio, double knee) noexcept
{
    const double over = levelDb - threshold;
    const double slope = 1.0 / ratio - 1.0; // <= 0
    if (knee > 0.0 && std::abs(over) <= knee * 0.5)
    {
        const double x = over + knee * 0.5;
        return slope * x * x / (2.0 * knee);
    }
    return over > 0.0 ? slope * over : 0.0;
}

double Compressor::getOutputLevelDb(double inputDb) const
{
    return inputDb + gainComputer(inputDb, threshold_->get(), ratio_->get(), knee_->get()) + makeup_->get();
}

void Compressor::prepareDsp(double, int)
{
    envelopeDb_ = 0.0;
}

void Compressor::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    const bool external = external_->get() && buffer.getNumChannels() >= 4;
    const double* detectLeft = external ? buffer.getReadPointer(2) : left;
    const double* detectRight = external ? buffer.getReadPointer(3) : right;

    const double threshold = threshold_->get();
    const double ratio = std::max(1.0, static_cast<double>(ratio_->get()));
    const double knee = knee_->get();
    const double makeup = makeup_->get();
    const double wet = mix_->get() / 100.0;
    const double attack = dsp::onePoleCoefficient(attack_->get() / 1000.0, sampleRate_);
    const double release = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);

    double maxReduction = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const double level = std::max(std::abs(detectLeft[i]), std::abs(detectRight[i]));
        const double levelDb = dsp::gainToDb(level);
        const double target = gainComputer(levelDb, threshold, ratio, knee);

        const double coefficient = target < envelopeDb_ ? attack : release;
        envelopeDb_ = target + (envelopeDb_ - target) * coefficient;
        maxReduction = std::min(maxReduction, envelopeDb_);

        const double gain = dsp::dbToGain(envelopeDb_ + makeup);
        left[i] = left[i] * (1.0 - wet) + left[i] * gain * wet;
        right[i] = right[i] * (1.0 - wet) + right[i] * gain * wet;
    }

    const auto reduction = static_cast<float>(-maxReduction);
    float previous = gainReduction_.load(std::memory_order_relaxed);
    while (reduction > previous && !gainReduction_.compare_exchange_weak(previous, reduction, std::memory_order_acq_rel))
    {
    }
}

//==================================================================================================
// Limiter

Limiter::Limiter()
    : BuiltinProcessor(kTypeId, "Limiter",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    input_ = addFloat("input", "Input Gain", {0.0f, 24.0f, 0.1f}, 0.0f, "dB");
    ceiling_ = addFloat("ceiling", "Ceiling", {-24.0f, 0.0f, 0.1f}, -0.3f, "dB");
    release_ = addFloat("release", "Release", skewedRange(1.0f, 1000.0f, 80.0f), 80.0f, "ms");
    lookahead_ = addFloat("lookahead", "Lookahead", {0.1f, static_cast<float>(kMaxLookaheadMs), 0.1f}, 5.0f, "ms",
                          false); // changes the latency, so it is not automatable
    lookahead_->addListener(this);
}

Limiter::~Limiter()
{
    lookahead_->removeListener(this);
}

int Limiter::lookaheadSamples() const noexcept
{
    return std::clamp(static_cast<int>(std::lround(lookahead_->get() / 1000.0 * sampleRate_)), 1, capacity_ - 2);
}

void Limiter::parameterValueChanged(int, float)
{
    // Only the lookahead is listened to. Reporting the new latency lets the engine re-align.
    setLatencySamples(lookaheadSamples());
}

void Limiter::prepareDsp(double sampleRate, int)
{
    capacity_ = static_cast<int>(std::ceil(kMaxLookaheadMs / 1000.0 * sampleRate)) + 4;
    for (auto& d : delay_)
        d.assign(static_cast<std::size_t>(capacity_), 0.0);
    required_.assign(static_cast<std::size_t>(capacity_), 1.0);
    dequeIndex_.assign(static_cast<std::size_t>(capacity_), 0);
    averageRing_.assign(static_cast<std::size_t>(capacity_), 1.0);
    resetState(lookaheadSamples());
    setLatencySamples(lookahead_samples_);
}

void Limiter::resetState(int lookahead) noexcept
{
    lookahead_samples_ = lookahead;
    for (auto& d : delay_)
        std::fill(d.begin(), d.end(), 0.0);
    std::fill(averageRing_.begin(), averageRing_.end(), 1.0);
    delayIndex_ = 0;
    dequeHead_ = dequeTail_ = 0;
    sampleCounter_ = 0;
    averageIndex_ = 0;
    averageSum_ = static_cast<double>(lookahead);
    gain_ = 1.0;
}

void Limiter::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    if (const int wanted = lookaheadSamples(); wanted != lookahead_samples_)
        resetState(wanted);

    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const int L = lookahead_samples_;
    const int cap = capacity_;

    const double inputGain = dsp::dbToGain(input_->get());
    const double ceiling = dsp::dbToGain(ceiling_->get());
    const double release = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);
    double minGain = 1.0;

    for (int i = 0; i < numSamples; ++i)
    {
        const double l = left[i] * inputGain;
        const double r = right[i] * inputGain;
        const double peak = std::max(std::abs(l), std::abs(r));
        const double needed = peak > ceiling ? ceiling / peak : 1.0;

        // Sliding minimum over the last L + 1 samples.
        const long long n = sampleCounter_++;
        while (dequeTail_ != dequeHead_)
        {
            const int back = (dequeTail_ - 1 + cap) % cap;
            if (required_[static_cast<std::size_t>(back)] < needed)
                break;
            dequeTail_ = back;
        }
        required_[static_cast<std::size_t>(dequeTail_)] = needed;
        dequeIndex_[static_cast<std::size_t>(dequeTail_)] = static_cast<int>(n % 1000000000LL);
        dequeTail_ = (dequeTail_ + 1) % cap;

        // Drop entries older than the window (indices wrap at 1e9; windows are tiny in comparison).
        while (true)
        {
            const long long headIndex = dequeIndex_[static_cast<std::size_t>(dequeHead_)];
            long long age = (n % 1000000000LL) - headIndex;
            if (age < 0)
                age += 1000000000LL;
            if (age <= L)
                break;
            dequeHead_ = (dequeHead_ + 1) % cap;
        }
        const double held = required_[static_cast<std::size_t>(dequeHead_)];

        // Average the held gain over L samples: the ramp reaches the held value as the peak arrives.
        averageSum_ += held - averageRing_[static_cast<std::size_t>(averageIndex_)];
        averageRing_[static_cast<std::size_t>(averageIndex_)] = held;
        averageIndex_ = (averageIndex_ + 1) % L;
        const double smoothed = std::min(1.0, averageSum_ / L);

        gain_ = smoothed < gain_ ? smoothed : smoothed + (gain_ - smoothed) * release;
        minGain = std::min(minGain, gain_);

        // Delay the audio by L.
        auto& dl = delay_[0];
        auto& dr = delay_[1];
        const int readIndex = (delayIndex_ - L + cap) % cap;
        const double delayedL = dl[static_cast<std::size_t>(readIndex)];
        const double delayedR = dr[static_cast<std::size_t>(readIndex)];
        dl[static_cast<std::size_t>(delayIndex_)] = l;
        dr[static_cast<std::size_t>(delayIndex_)] = r;
        delayIndex_ = (delayIndex_ + 1) % cap;

        // Final safety clamp guards against rounding at the ceiling.
        left[i] = std::clamp(delayedL * gain_, -ceiling, ceiling);
        right[i] = std::clamp(delayedR * gain_, -ceiling, ceiling);
    }

    const auto reduction = static_cast<float>(-dsp::gainToDb(minGain));
    float previous = gainReduction_.load(std::memory_order_relaxed);
    while (reduction > previous && !gainReduction_.compare_exchange_weak(previous, reduction, std::memory_order_acq_rel))
    {
    }
}

} // namespace dojo::devices

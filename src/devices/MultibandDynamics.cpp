#include "devices/MultibandDynamics.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

MultibandDynamics::MultibandDynamics()
    : BuiltinProcessor(kTypeId, "Multiband Dynamics",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    constexpr std::array<const char*, kBands> ids{"low", "mid", "high"};
    constexpr std::array<const char*, kBands> names{"Low", "Mid", "High"};
    for (std::size_t b = 0; b < bands_.size(); ++b)
    {
        const juce::String id = ids[b];
        const juce::String name = juce::String(names[b]) + " ";
        auto& band = bands_[b];
        band.on = addBool(id + "on", name + "On", true);
        band.aboveThreshold = addFloat(id + "athr", name + "Above Threshold", {-60.0f, 0.0f, 0.1f}, -18.0f, "dB");
        band.aboveRatio = addFloat(id + "arat", name + "Above Ratio", skewedRange(0.5f, 20.0f, 2.0f, 0.01f), 2.0f, ":1");
        band.belowThreshold = addFloat(id + "bthr", name + "Below Threshold", {-80.0f, 0.0f, 0.1f}, -60.0f, "dB");
        band.belowRatio = addFloat(id + "brat", name + "Below Ratio", skewedRange(0.25f, 8.0f, 1.0f, 0.01f), 1.0f, ":1");
        band.gain = addFloat(id + "gain", name + "Gain", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
        band.solo = addBool(id + "solo", name + "Solo", false);
    }
    crossLow_ = addFloat("xlow", "Low/Mid", skewedRange(40.0f, 1000.0f, 150.0f), 120.0f, "Hz");
    crossHigh_ = addFloat("xhigh", "Mid/High", skewedRange(800.0f, 15000.0f, 3000.0f), 2500.0f, "Hz");
    attack_ = addFloat("attack", "Attack", skewedRange(0.1f, 300.0f, 20.0f), 10.0f, "ms");
    release_ = addFloat("release", "Release", skewedRange(5.0f, 3000.0f, 200.0f), 150.0f, "ms");
    detection_ = addChoice("detection", "Detection", {"Peak", "RMS"}, 1);
    softKnee_ = addBool("knee", "Soft Knee", true);
    amount_ = addFloat("amount", "Amount", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    output_ = addFloat("output", "Output", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

double MultibandDynamics::gainComputer(double levelDb, double aboveThreshold, double aboveRatio, double belowThreshold,
                                       double belowRatio, bool softKnee, double amount) noexcept
{
    const double knee = softKnee ? 6.0 : 0.0;
    const double aboveSlope = 1.0 / std::max(0.01, aboveRatio) - 1.0; // < 0 compresses, > 0 expands upward
    const double belowSlope = std::max(0.01, belowRatio) - 1.0;       // > 0 expands downward, < 0 lifts
    double gain = 0.0;

    // Above the upper threshold (soft knee: a quadratic blend across `knee` dB).
    if (knee > 0.0 && std::abs(levelDb - aboveThreshold) < knee * 0.5)
    {
        const double x = levelDb - aboveThreshold + knee * 0.5;
        gain += aboveSlope * x * x / (2.0 * knee);
    }
    else if (levelDb > aboveThreshold)
    {
        gain += aboveSlope * (levelDb - aboveThreshold);
    }

    // Below the lower threshold: output = threshold + (level - threshold) x ratio.
    if (knee > 0.0 && std::abs(levelDb - belowThreshold) < knee * 0.5)
    {
        const double x = belowThreshold + knee * 0.5 - levelDb;
        gain -= belowSlope * x * x / (2.0 * knee);
    }
    else if (levelDb < belowThreshold)
    {
        gain += belowSlope * (levelDb - belowThreshold);
    }

    return std::clamp(gain * amount, kMaxDownwardDb, kMaxUpwardDb);
}

double MultibandDynamics::getBandOutputDb(int band, double inputDb) const noexcept
{
    const auto& b = bands_[static_cast<std::size_t>(std::clamp(band, 0, kBands - 1))];
    if (!b.on->get())
        return inputDb;
    return inputDb + gainComputer(inputDb, b.aboveThreshold->get(), b.aboveRatio->get(), b.belowThreshold->get(),
                                  b.belowRatio->get(), softKnee_->get(), amount_->get() / 100.0) +
           b.gain->get();
}

void MultibandDynamics::prepareDsp(double, int)
{
    for (auto* filters : {&lowSplit_, &highSplit_, &lowAlign_})
        for (auto& f : *filters)
            f.reset();
    envelope_.fill(0.0);
}

void MultibandDynamics::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    const double lowFrequency = crossLow_->get();
    const double highFrequency = std::max(lowFrequency * 1.5, static_cast<double>(crossHigh_->get()));
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        lowSplit_[ch].setFrequency(lowFrequency, sampleRate_);
        highSplit_[ch].setFrequency(highFrequency, sampleRate_);
        lowAlign_[ch].setFrequency(highFrequency, sampleRate_);
    }

    const double attack = dsp::onePoleCoefficient(attack_->get() / 1000.0, sampleRate_);
    const double release = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);
    const bool rms = detection_->getIndex() == 1;
    const bool softKnee = softKnee_->get();
    const double amount = amount_->get() / 100.0;
    const double outputGain = dsp::dbToGain(output_->get());
    const double wet = mix_->get() / 100.0;

    bool anySolo = false;
    for (const auto& b : bands_)
        anySolo = anySolo || b.solo->get();

    struct BandSettings
    {
        bool on, audible;
        double aboveThreshold, aboveRatio, belowThreshold, belowRatio, gain;
    };
    std::array<BandSettings, kBands> settings{};
    for (std::size_t b = 0; b < bands_.size(); ++b)
    {
        const auto& band = bands_[b];
        settings[b] = {band.on->get(), !anySolo || band.solo->get(), band.aboveThreshold->get(), band.aboveRatio->get(),
                       band.belowThreshold->get(), band.belowRatio->get(), dsp::dbToGain(band.gain->get())};
    }

    std::array<double, kBands> lastGainDb{};
    for (int i = 0; i < numSamples; ++i)
    {
        // Split each channel: low | (mid | high); the low band gets the high crossover's phase too.
        std::array<std::array<double, 2>, kBands> x{};
        const double in[2] = {left[i], right[i]};
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            double low = 0.0, rest = 0.0, mid = 0.0, high = 0.0;
            lowSplit_[ch].split(in[ch], low, rest);
            highSplit_[ch].split(rest, mid, high);
            x[0][ch] = lowAlign_[ch].allPass(low);
            x[1][ch] = mid;
            x[2][ch] = high;
        }

        double outLeft = 0.0, outRight = 0.0, dryLeft = 0.0, dryRight = 0.0;
        for (std::size_t b = 0; b < x.size(); ++b)
        {
            const double l = x[b][0], r = x[b][1];
            dryLeft += l; // (the bands' sum: the dry signal with the crossovers' phase, for dry/wet)
            dryRight += r;

            // Level detection, stereo-linked: peak or RMS, with the attack and release.
            const double detector = rms ? 0.5 * (l * l + r * r) : std::max(std::abs(l), std::abs(r));
            envelope_[b] = detector + (envelope_[b] - detector) * (detector > envelope_[b] ? attack : release);
            const double level = rms ? std::sqrt(envelope_[b]) : envelope_[b];

            const auto& s = settings[b];
            if (!s.audible)
                continue;
            double gain = 1.0;
            if (s.on)
            {
                const double change = gainComputer(dsp::gainToDb(level), s.aboveThreshold, s.aboveRatio, s.belowThreshold,
                                                   s.belowRatio, softKnee, amount);
                lastGainDb[b] = change;
                gain = dsp::dbToGain(change) * s.gain;
            }
            outLeft += l * gain;
            outRight += r * gain;
        }

        left[i] = dryLeft * (1.0 - wet) + outLeft * outputGain * wet;
        right[i] = dryRight * (1.0 - wet) + outRight * outputGain * wet;
    }

    for (std::size_t b = 0; b < envelope_.size(); ++b)
    {
        const double level = rms ? std::sqrt(envelope_[b]) : envelope_[b];
        levels_[b].store(static_cast<float>(dsp::gainToDb(level)), std::memory_order_relaxed);
        gains_[b].store(static_cast<float>(settings[b].on ? lastGainDb[b] : 0.0), std::memory_order_relaxed);
    }
}

} // namespace dojo::devices

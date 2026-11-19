#include "devices/Expander.h"

#include "devices/dsp/DynamicsMath.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
using Shape = dsp::BiquadCoefficients::Shape;
constexpr double kMaxLookaheadMs = 10.0;

/** Stores `value` if its magnitude is larger than what is there (signed meters). */
void storeLargest(std::atomic<float>& target, float value) noexcept
{
    float previous = target.load(std::memory_order_relaxed);
    while (std::abs(value) > std::abs(previous) && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed))
    {
    }
}
} // namespace

Expander::Expander()
    : BuiltinProcessor(kTypeId, "Expander",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Sidechain", juce::AudioChannelSet::stereo(), true),
                       false)
{
    mode_ = addChoice("mode", "Mode", {"Downward", "Upward"}, 0);
    threshold_ = addFloat("threshold", "Threshold", {-80.0f, 0.0f, 0.1f}, -40.0f, "dB");
    ratio_ = addFloat("ratio", "Ratio", skewedRange(1.0f, 10.0f, 2.5f), 2.0f, ":1");
    range_ = addFloat("range", "Range", {0.0f, 80.0f, 0.1f}, 20.0f, "dB");
    knee_ = addFloat("knee", "Knee", {0.0f, 24.0f, 0.1f}, 6.0f, "dB");
    hysteresis_ = addFloat("hysteresis", "Hysteresis", {0.0f, 12.0f, 0.1f}, 2.0f, "dB");
    attack_ = addFloat("attack", "Attack", skewedRange(0.05f, 100.0f, 5.0f), 1.0f, "ms");
    hold_ = addFloat("hold", "Hold", skewedRange(0.0f, 500.0f, 50.0f), 30.0f, "ms");
    release_ = addFloat("release", "Release", skewedRange(5.0f, 2000.0f, 150.0f), 150.0f, "ms");
    detect_ = addChoice("detect", "Detector", {"Peak", "RMS"}, 0);
    lookahead_ = addChoice("lookahead", "Lookahead", {"Off", "1 ms", "3 ms", "5 ms", "10 ms"}, 0, false);
    external_ = addBool("sidechain", "External Sidechain", false);
    keyLow_ = addFloat("keylow", "Key High-Pass", skewedRange(20.0f, 2000.0f, 200.0f), 20.0f, "Hz");
    keyHigh_ = addFloat("keyhigh", "Key Low-Pass", skewedRange(500.0f, 20000.0f, 5000.0f), 20000.0f, "Hz");
    keyListen_ = addBool("keylisten", "Key Listen", false);
    output_ = addFloat("output", "Output", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    lookahead_->addListener(this);
}

Expander::~Expander()
{
    lookahead_->removeListener(this);
}

double Expander::lookaheadMsOf(int choice) noexcept
{
    constexpr std::array<double, 5> ms{0.0, 1.0, 3.0, 5.0, 10.0};
    return ms[static_cast<std::size_t>(std::clamp(choice, 0, 4))];
}

int Expander::lookaheadSamples() const noexcept
{
    return static_cast<int>(std::lround(lookaheadMsOf(lookahead_->getIndex()) / 1000.0 * sampleRate_));
}

void Expander::parameterValueChanged(int, float)
{
    setLatencySamples(lookaheadSamples());
}

double Expander::gainChangeFor(double levelDb, bool open) const noexcept
{
    // Hysteresis: once open, the threshold sits lower (it closes only further down).
    const double threshold = threshold_->get() - (open ? hysteresis_->get() : 0.0);
    const double slope = ratio_->get() - 1.0;
    const double range = range_->get();
    if (mode_->getIndex() == 1)
        return std::min(range, softOver(levelDb - threshold, knee_->get()) * slope);   // upward: louder
    return -std::min(range, softOver(threshold - levelDb, knee_->get()) * slope);      // downward: quieter
}

void Expander::prepareDsp(double sampleRate, int)
{
    for (auto& f : keyHighPass_)
        f.reset();
    for (auto& f : keyLowPass_)
        f.reset();
    for (auto& d : delay_)
    {
        d.setMaximum(static_cast<int>(std::ceil(kMaxLookaheadMs / 1000.0 * sampleRate)) + 4);
        d.reset();
    }
    level_ = meanSquare_ = gainDb_ = holdLeft_ = 0.0;
    open_ = false;
    setLatencySamples(lookaheadSamples());
}

void Expander::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const bool external = external_->get() && buffer.getNumChannels() >= 4;
    const double* keyLeft = external ? buffer.getReadPointer(2) : left;
    const double* keyRight = external ? buffer.getReadPointer(3) : right;

    // The detector: Peak (instant attack, a 20 ms decay that rides over the waveform) or RMS (10 ms,
    // scaled so a sine reads its peak). The gain then rises with Attack and falls with Release.
    const bool rms = detect_->getIndex() == 1;
    const double detectorDecay = dsp::onePoleCoefficient(0.02, sampleRate_);
    const double rmsCoefficient = 1.0 - dsp::onePoleCoefficient(kRmsSeconds, sampleRate_);
    const double rising = dsp::onePoleCoefficient(attack_->get() / 1000.0, sampleRate_);
    const double falling = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);
    const double hold = hold_->get() / 1000.0;
    const double step = 1.0 / sampleRate_;
    const double threshold = threshold_->get();
    const double hysteresis = hysteresis_->get();
    const bool highPass = keyLow_->get() > 20.5f;
    const bool lowPass = keyHigh_->get() < 19999.0f;
    const auto hp = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, keyLow_->get(), 0.7071, 0.0);
    const auto lp = dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, std::min(static_cast<double>(keyHigh_->get()), 0.45 * sampleRate_), 0.7071, 0.0);
    const bool listen = keyListen_->get();
    const double output = dsp::dbToGain(output_->get());
    const double wet = mix_->get() / 100.0;
    const auto lookahead = static_cast<double>(lookaheadSamples());
    double largestChange = 0.0, maxLevel = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        double kl = keyLeft[i], kr = keyRight[i];
        if (highPass)
        {
            kl = keyHighPass_[0].process(kl, hp);
            kr = keyHighPass_[1].process(kr, hp);
        }
        if (lowPass)
        {
            kl = keyLowPass_[0].process(kl, lp);
            kr = keyLowPass_[1].process(kr, lp);
        }
        if (rms)
        {
            meanSquare_ += (0.5 * (kl * kl + kr * kr) - meanSquare_) * rmsCoefficient;
            level_ = std::sqrt(2.0 * std::max(0.0, meanSquare_));
        }
        else
        {
            level_ = std::max(std::max(std::abs(kl), std::abs(kr)), level_ * detectorDecay);
        }
        maxLevel = std::max(maxLevel, level_);
        const double levelDb = dsp::gainToDb(level_);

        // Hysteresis: open at the threshold, closed only once under it by the hysteresis.
        if (levelDb >= threshold)
            open_ = true;
        else if (levelDb < threshold - hysteresis)
            open_ = false;

        const double target = gainChangeFor(levelDb, open_);
        if (target >= gainDb_)
        {
            gainDb_ = target + (gainDb_ - target) * rising;
            holdLeft_ = hold;
        }
        else if (holdLeft_ > 0.0)
        {
            holdLeft_ -= step; // (hold before falling)
        }
        else
        {
            gainDb_ = target + (gainDb_ - target) * falling;
        }
        if (std::abs(gainDb_) > std::abs(largestChange))
            largestChange = gainDb_;

        delay_[0].push(left[i]);
        delay_[1].push(right[i]);
        if (listen)
        {
            left[i] = kl;
            right[i] = kr;
            continue;
        }
        const double gain = dsp::dbToGain(gainDb_);
        const double dryLeft = delay_[0].read(lookahead), dryRight = delay_[1].read(lookahead);
        left[i] = (dryLeft * (1.0 - wet) + dryLeft * gain * wet) * output;
        right[i] = (dryRight * (1.0 - wet) + dryRight * gain * wet) * output;
    }

    storeLargest(gainChange_, static_cast<float>(largestChange));
    storeMax(detector_, static_cast<float>(dsp::gainToDb(maxLevel)));
}

} // namespace dojo::devices

#include "devices/Gate.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

Gate::Gate()
    : BuiltinProcessor(kTypeId, "Gate",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Sidechain", juce::AudioChannelSet::stereo(), true),
                       false)
{
    threshold_ = addFloat("threshold", "Threshold", {-80.0f, 0.0f, 0.1f}, -40.0f, "dB");
    return_ = addFloat("return", "Return", {0.0f, 24.0f, 0.1f}, 3.0f, "dB");
    attack_ = addFloat("attack", "Attack", skewedRange(0.02f, 150.0f, 5.0f), 0.1f, "ms");
    hold_ = addFloat("hold", "Hold", skewedRange(0.0f, 500.0f, 50.0f), 20.0f, "ms");
    release_ = addFloat("release", "Release", skewedRange(1.0f, 2000.0f, 150.0f), 150.0f, "ms");
    floor_ = addFloat("floor", "Floor", {-80.0f, 0.0f, 0.1f}, -80.0f, "dB");
    flip_ = addBool("flip", "Flip", false);
    lookahead_ = addChoice("lookahead", "Lookahead", {"Off", "1 ms", "3 ms", "5 ms", "10 ms"}, 0, false); // (latency)
    external_ = addBool("sidechain", "External Sidechain", false);
    keyLow_ = addFloat("keylow", "Key High-Pass", skewedRange(20.0f, 2000.0f, 200.0f), 20.0f, "Hz");
    keyHigh_ = addFloat("keyhigh", "Key Low-Pass", skewedRange(500.0f, 20000.0f, 5000.0f), 20000.0f, "Hz");
    listen_ = addBool("listen", "Listen", false);
    lookahead_->addListener(this);
}

Gate::~Gate()
{
    lookahead_->removeListener(this);
}

double Gate::lookaheadMs(int choice) noexcept
{
    constexpr std::array<double, 5> ms{0.0, 1.0, 3.0, 5.0, 10.0};
    return ms[static_cast<std::size_t>(std::clamp(choice, 0, 4))];
}

int Gate::lookaheadSamples() const noexcept
{
    return static_cast<int>(std::lround(lookaheadMs(lookahead_->getIndex()) / 1000.0 * sampleRate_));
}

void Gate::parameterValueChanged(int, float)
{
    setLatencySamples(lookaheadSamples());
}

void Gate::prepareDsp(double sampleRate, int)
{
    const int capacity = static_cast<int>(std::ceil(kMaxLookaheadMs / 1000.0 * sampleRate)) + 8;
    for (auto& d : delay_)
        d.setMaximum(capacity);
    for (auto& b : keyHighPass_)
        b.reset();
    for (auto& b : keyLowPass_)
        b.reset();
    open_ = false;
    holdCounter_ = 0;
    envelope_ = 0.0;
    detector_ = 0.0;
    setLatencySamples(lookaheadSamples());
}

void Gate::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const bool external = external_->get() && buffer.getNumChannels() >= 4;
    const double* keyLeft = external ? buffer.getReadPointer(2) : left;
    const double* keyRight = external ? buffer.getReadPointer(3) : right;

    const double threshold = threshold_->get();
    const double open = dsp::dbToGain(threshold);
    const double close = dsp::dbToGain(threshold - return_->get());
    const double floorGain = floor_->get() <= -79.95f ? 0.0 : dsp::dbToGain(floor_->get());
    const double attackStep = 1.0 / std::max(1.0, attack_->get() / 1000.0 * sampleRate_);
    const double releaseCoefficient = std::exp(-4.6 / std::max(1.0, release_->get() / 1000.0 * sampleRate_));
    const int holdSamples = static_cast<int>(hold_->get() / 1000.0 * sampleRate_);
    const bool flip = flip_->get();
    const bool listen = listen_->get();
    const int lookahead = std::min(lookaheadSamples(), delay_[0].capacity());
    const double detectorDecay = std::exp(-1.0 / (kDetectorDecaySeconds * sampleRate_));

    const bool highPass = keyLow_->get() > 20.5f;
    const bool lowPass = keyHigh_->get() < 19900.0f;
    const auto hp = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::lowCut, sampleRate_, keyLow_->get(), 0.7071, 0.0);
    const auto lp = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::highCut, sampleRate_, keyHigh_->get(), 0.7071, 0.0);

    double peak = 0.0;
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
        // A peak detector: instant attack, 30 ms decay (rides over the waveform's zero crossings).
        const double instant = std::max(std::abs(kl), std::abs(kr));
        detector_ = std::max(instant, detector_ * detectorDecay);
        const double level = detector_;
        peak = std::max(peak, instant);

        // Open above the threshold; below threshold - return, count the hold down, then close. In
        // between (the hysteresis band) an open gate stays open.
        if (level >= open)
        {
            open_ = true;
            holdCounter_ = holdSamples;
        }
        else if (level < close)
        {
            if (holdCounter_ > 0)
                --holdCounter_;
            else
                open_ = false;
        }
        else if (open_)
        {
            holdCounter_ = holdSamples;
        }

        envelope_ = open_ ? std::min(1.0, envelope_ + attackStep) : envelope_ * releaseCoefficient;
        const double openness = flip ? 1.0 - envelope_ : envelope_;
        const double gain = floorGain + (1.0 - floorGain) * openness;

        double l = left[i], r = right[i];
        if (lookahead > 0)
        {
            delay_[0].push(l);
            delay_[1].push(r);
            l = delay_[0].read(lookahead);
            r = delay_[1].read(lookahead);
        }
        if (listen)
        {
            left[i] = kl;
            right[i] = kr;
        }
        else
        {
            left[i] = l * gain;
            right[i] = r * gain;
        }
    }

    openness_.store(static_cast<float>(flip ? 1.0 - envelope_ : envelope_), std::memory_order_relaxed);
    const auto peakDb = static_cast<float>(dsp::gainToDb(peak));
    float previous = detectorPeak_.load(std::memory_order_relaxed);
    while (peakDb > previous && !detectorPeak_.compare_exchange_weak(previous, peakDb, std::memory_order_acq_rel))
    {
    }
}

} // namespace dojo::devices

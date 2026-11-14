#include "devices/DeEsser.h"

#include "devices/dsp/DynamicsMath.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
using Shape = dsp::BiquadCoefficients::Shape;
constexpr double kMaxLookaheadMs = 5.0;
} // namespace

void DeEsser::Lane::reset() noexcept
{
    for (auto& d : detect)
        d.reset();
    lower.reset();
    upper.reset();
    align.reset();
    delay.reset();
    envelope = fullEnvelope = 0.0;
}

DeEsser::DeEsser()
    : BuiltinProcessor(kTypeId, "De-Esser",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    frequency_ = addFloat("freq", "Frequency", skewedRange(2000.0f, 16000.0f, 6000.0f), 6000.0f, "Hz");
    high_ = addFloat("high", "High", skewedRange(4000.0f, 20000.0f, 10000.0f), 12000.0f, "Hz");
    band_ = addChoice("band", "Band", {"Above", "Band"}, 0);
    threshold_ = addFloat("threshold", "Threshold", {-60.0f, 0.0f, 0.1f}, -24.0f, "dB");
    range_ = addFloat("range", "Range", {0.0f, 24.0f, 0.1f}, 8.0f, "dB");
    detection_ = addChoice("detect", "Detection", {"Absolute", "Relative"}, 0);
    mode_ = addChoice("mode", "Mode", {"Split", "Wide"}, 0);
    release_ = addFloat("release", "Release", skewedRange(10.0f, 300.0f, 60.0f), 60.0f, "ms");
    lookahead_ = addChoice("lookahead", "Lookahead", {"Off", "1 ms", "2 ms", "5 ms"}, 0, false);
    stereo_ = addChoice("stereo", "Stereo", {"Linked", "Unlinked", "Mid", "Side"}, 0);
    listen_ = addChoice("listen", "Listen", {"Off", "Trigger", "Delta"}, 0);
    lookahead_->addListener(this);
}

DeEsser::~DeEsser()
{
    lookahead_->removeListener(this);
}

double DeEsser::lookaheadMsOf(int choice) noexcept
{
    constexpr std::array<double, 4> ms{0.0, 1.0, 2.0, 5.0};
    return ms[static_cast<std::size_t>(std::clamp(choice, 0, 3))];
}

int DeEsser::lookaheadSamples() const noexcept
{
    return static_cast<int>(std::lround(lookaheadMsOf(lookahead_->getIndex()) / 1000.0 * sampleRate_));
}

void DeEsser::parameterValueChanged(int, float)
{
    setLatencySamples(lookaheadSamples());
}

double DeEsser::reductionFor(double levelDb) const noexcept
{
    const double over = softOver(levelDb - threshold_->get(), kKneeDb);
    return std::min(static_cast<double>(range_->get()), over * (1.0 - 1.0 / kRatio));
}

double DeEsser::detectorResponseDb(double frequency) const noexcept
{
    const double rate = std::max(44100.0, sampleRate_);
    const double low = frequency_->get();
    const bool band = band_->getIndex() == 1;
    const auto first = dsp::BiquadCoefficients::make(Shape::lowCut, rate, low, 0.7071, 0.0);
    const auto second = band ? dsp::BiquadCoefficients::make(Shape::highCut, rate, std::max(static_cast<double>(high_->get()), low * 1.25), 0.7071, 0.0)
                             : first;
    return dsp::gainToDb(first.magnitudeAt(frequency, rate) * second.magnitudeAt(frequency, rate));
}

void DeEsser::prepareDsp(double sampleRate, int)
{
    for (auto& lane : lanes_)
    {
        lane.delay.setMaximum(static_cast<int>(std::ceil(kMaxLookaheadMs / 1000.0 * sampleRate)) + 4);
        lane.reset();
    }
    smoothedFrequency_ = smoothedHigh_ = -1.0; // (the first block starts where the knobs are)
    setLatencySamples(lookaheadSamples());
}

void DeEsser::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    // (The frequencies glide, so sweeping them does not click.)
    if (smoothedFrequency_ < 0.0)
    {
        smoothedFrequency_ = frequency_->get();
        smoothedHigh_ = high_->get();
    }
    smoothedFrequency_ += (frequency_->get() - smoothedFrequency_) * 0.5;
    smoothedHigh_ += (high_->get() - smoothedHigh_) * 0.5;
    const double frequency = smoothedFrequency_;
    const double high = std::min(std::max(smoothedHigh_, frequency * 1.25), 0.45 * sampleRate_);
    const bool band = band_->getIndex() == 1;
    const auto first = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, frequency, 0.7071, 0.0);
    const auto second = band ? dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, high, 0.7071, 0.0) : first;
    for (auto& lane : lanes_)
    {
        lane.lower.setFrequency(frequency, sampleRate_);
        lane.upper.setFrequency(high, sampleRate_);
        lane.align.setFrequency(high, sampleRate_);
    }

    const double attack = dsp::onePoleCoefficient(kAttackSeconds, sampleRate_);
    const double release = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);
    const bool split = mode_->getIndex() == 0;
    const bool relative = detection_->getIndex() == 1;
    const int stereo = stereo_->getIndex(); // Linked, Unlinked, Mid, Side
    const int processed = stereo < 2 ? 2 : 1; // lanes turned down (the other passes, phase-matched)
    const int listen = listen_->getIndex();   // Off, Trigger, Delta
    const auto lookahead = static_cast<double>(lookaheadSamples());
    const double gate = dsp::dbToGain(kRelativeGateDb);
    double maxReduction = 0.0, maxLevel = -100.0;

    for (int i = 0; i < numSamples; ++i)
    {
        // The lanes: left/right, or mid/side (the processed one first).
        double in[2] = {left[i], right[i]};
        if (stereo == 2)
            in[0] = 0.5 * (left[i] + right[i]), in[1] = 0.5 * (left[i] - right[i]);
        else if (stereo == 3)
            in[0] = 0.5 * (left[i] - right[i]), in[1] = 0.5 * (left[i] + right[i]);

        double detected[2] = {0.0, 0.0}, levelDb[2] = {-100.0, -100.0};
        for (int k = 0; k < processed; ++k)
        {
            auto& lane = lanes_[static_cast<std::size_t>(k)];
            detected[k] = lane.detect[1].process(lane.detect[0].process(in[k], first), second);
        }
        // Linked: one detector for both sides (it hears the louder); otherwise one per lane.
        const int detectors = stereo == 0 ? 1 : processed;
        for (int k = 0; k < detectors; ++k)
        {
            auto& lane = lanes_[static_cast<std::size_t>(k)];
            const double e = stereo == 0 ? std::max(std::abs(detected[0]), std::abs(detected[1])) : std::abs(detected[k]);
            lane.envelope = e + (lane.envelope - e) * (e > lane.envelope ? attack : release);
            if (relative)
            {
                const double full = stereo == 0 ? std::max(std::abs(in[0]), std::abs(in[1])) : std::abs(in[k]);
                lane.fullEnvelope = full + (lane.fullEnvelope - full) * (full > lane.fullEnvelope ? attack : release);
                levelDb[k] = lane.fullEnvelope > gate ? dsp::gainToDb(lane.envelope) - dsp::gainToDb(lane.fullEnvelope) : -100.0;
            }
            else
            {
                levelDb[k] = dsp::gainToDb(lane.envelope);
            }
        }
        if (stereo == 0)
            levelDb[1] = levelDb[0];

        double out[2] = {0.0, 0.0};
        for (int k = 0; k < 2; ++k)
        {
            auto& lane = lanes_[static_cast<std::size_t>(k)];
            lane.delay.push(in[k]);
            const double d = lane.delay.read(lookahead);
            const bool turnedDown = k < processed;
            const double reductionDb = turnedDown ? reductionFor(levelDb[k]) : 0.0;
            const double gain = reductionDb > 1.0e-4 ? dsp::dbToGain(-reductionDb) : 1.0;
            if (turnedDown)
            {
                maxReduction = std::max(maxReduction, reductionDb);
                maxLevel = std::max(maxLevel, levelDb[k]);
            }

            double kept = d, removed = 0.0; // what stays, what is taken out (Delta)
            if (split && !band)
            {
                double low = 0.0, top = 0.0;
                lane.lower.split(d, low, top);
                kept = low + top * gain;
                removed = top * (1.0 - gain);
            }
            else if (split)
            {
                double low = 0.0, rest = 0.0, mid = 0.0, top = 0.0;
                lane.lower.split(d, low, rest);
                lane.upper.split(rest, mid, top);
                kept = lane.align.allPass(low) + mid * gain + top;
                removed = mid * (1.0 - gain);
            }
            else
            {
                kept = d * gain;
                removed = d * (1.0 - gain);
            }

            if (listen == 1)
                out[k] = turnedDown ? detected[k] : 0.0;
            else if (listen == 2)
                out[k] = removed;
            else
                out[k] = kept;
        }

        if (stereo == 2)
            left[i] = out[0] + out[1], right[i] = out[0] - out[1];
        else if (stereo == 3)
            left[i] = out[1] + out[0], right[i] = out[1] - out[0];
        else
            left[i] = out[0], right[i] = out[1];
    }

    storeMax(reduction_, static_cast<float>(maxReduction));
    storeMax(detector_, static_cast<float>(maxLevel));
}

} // namespace dojo::devices

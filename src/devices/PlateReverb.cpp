#include "devices/PlateReverb.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kTwoPi = 6.28318530717958647692;

// Delay lengths at the reference rate (Dattorro, "Effect Design, Part 1: Reverberator and Other
// Filters", J. Audio Eng. Soc. 45(9), 1997).
constexpr std::array<double, 4> kDiffusers{142.0, 107.0, 379.0, 277.0};
constexpr std::array<double, 2> kModulated{672.0, 908.0};
constexpr std::array<double, 2> kFirstDelay{4453.0, 4217.0};
constexpr std::array<double, 2> kSecondAllPass{1800.0, 2656.0};
constexpr std::array<double, 2> kSecondDelay{3720.0, 3163.0};
constexpr double kExcursion = 16.0; ///< the tank modulation's reach at 100 % ... x2
constexpr double kTapGain = 0.6;
/** One pass around the whole tank (both branches), in reference samples. */
constexpr double kLoopLength = 672.0 + 4453.0 + 1800.0 + 3720.0 + 908.0 + 4217.0 + 2656.0 + 3163.0;
} // namespace

PlateReverb::PlateReverb()
    : BuiltinProcessor(kTypeId, "Plate Reverb",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    preDelay_ = addFloat("predelay", "Pre-delay", skewedRange(0.0f, static_cast<float>(kMaxPreDelayMs), 30.0f), 10.0f, "ms");
    decay_ = addFloat("decay", "Decay", skewedRange(0.2f, 10.0f, 2.0f), 1.5f, "s");
    size_ = addFloat("size", "Size", {50.0f, 150.0f, 0.1f}, 100.0f, "%");
    damping_ = addFloat("damping", "Damping", skewedRange(1000.0f, 20000.0f, 6000.0f), 9000.0f, "Hz");
    bandwidth_ = addFloat("bandwidth", "Bandwidth", skewedRange(1000.0f, 20000.0f, 6000.0f), 16000.0f, "Hz");
    lowCut_ = addFloat("lowcut", "Low Cut", skewedRange(20.0f, 1000.0f, 150.0f), 100.0f, "Hz");
    diffusion_ = addFloat("diffusion", "Diffusion", {0.0f, 100.0f, 0.1f}, 70.0f, "%");
    modulation_ = addFloat("modulation", "Modulation", {0.0f, 100.0f, 0.1f}, 30.0f, "%");
    width_ = addFloat("width", "Width", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 25.0f, "%");
}

double PlateReverb::getTailLengthSeconds() const
{
    return decay_->get() + preDelay_->get() / 1000.0;
}

double PlateReverb::getDecayGain() const noexcept
{
    // The signal passes four decay stages per trip around the tank: g^4 per loop, -60 dB after Decay.
    const double loopSeconds = kLoopLength / kReferenceRate * size_->get() / 100.0;
    return std::min(0.9995, std::pow(10.0, -3.0 * loopSeconds / (4.0 * std::max(0.05, static_cast<double>(decay_->get())))));
}

void PlateReverb::prepareDsp(double sampleRate, int)
{
    scale_ = sampleRate / kReferenceRate;
    const auto room = [&](double referenceLength, double extra = 0.0)
    { return static_cast<int>(std::ceil((referenceLength * kMaxSize + extra) * scale_)) + 8; };

    preDelayLine_.setMaximum(static_cast<int>(std::ceil(kMaxPreDelayMs / 1000.0 * sampleRate)) + 8);
    for (std::size_t i = 0; i < diffusers_.size(); ++i)
        diffusers_[i].setMaximum(room(kDiffusers[i]));
    for (std::size_t b = 0; b < 2; ++b)
    {
        modulated_[b].setMaximum(room(kModulated[b], 2.0 * kExcursion + 4.0));
        firstDelay_[b].setMaximum(room(kFirstDelay[b]));
        secondAllPass_[b].setMaximum(room(kSecondAllPass[b]));
        secondDelay_[b].setMaximum(room(kSecondDelay[b]));
    }
    dampingState_.fill(0.0);
    tankOut_.fill(0.0);
    bandwidthState_ = 0.0;
    lfoPhase_ = 0.0;
    for (auto& f : lowCutFilter_)
        f.reset();
}

void PlateReverb::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    const double size = size_->get() / 100.0;
    const double s = scale_ * size; // samples per reference sample, at this size
    const double preDelay = preDelay_->get() / 1000.0 * sampleRate_;
    const double decay = getDecayGain();
    const double diffusion = diffusion_->get() / 100.0;
    const double inputDiffusion1 = std::min(0.85, 0.75 * diffusion / 0.7);
    const double inputDiffusion2 = std::min(0.75, 0.625 * diffusion / 0.7);
    const double decayDiffusion1 = std::clamp(0.5 + 0.2 * diffusion / 0.7, 0.4, 0.75);
    const double decayDiffusion2 = std::clamp(decay + 0.15, 0.25, 0.5);
    const double bandwidth = 1.0 - std::exp(-kTwoPi * bandwidth_->get() / sampleRate_);
    const double damping = std::exp(-kTwoPi * damping_->get() / sampleRate_);
    const double excursion = modulation_->get() / 100.0 * 2.0 * kExcursion * scale_;
    const double lfoStep = kTwoPi * 0.8 / sampleRate_;
    const double width = width_->get() / 100.0;
    const double wet = mix_->get() / 100.0;
    const auto lowCut = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::lowCut, sampleRate_, lowCut_->get(), 0.7071, 0.0);

    for (int i = 0; i < numSamples; ++i)
    {
        const double dryLeft = left[i], dryRight = right[i];

        // Input: pre-delay, bandwidth, four diffusers.
        preDelayLine_.push(0.5 * (dryLeft + dryRight));
        double x = preDelayLine_.read(preDelay);
        bandwidthState_ += bandwidth * (x - bandwidthState_);
        x = bandwidthState_;
        x = allPass(diffusers_[0], kDiffusers[0] * s, inputDiffusion1, x);
        x = allPass(diffusers_[1], kDiffusers[1] * s, inputDiffusion1, x);
        x = allPass(diffusers_[2], kDiffusers[2] * s, inputDiffusion2, x);
        x = allPass(diffusers_[3], kDiffusers[3] * s, inputDiffusion2, x);

        // The tank: each branch takes the input plus the other branch's output.
        lfoPhase_ += lfoStep;
        if (lfoPhase_ >= kTwoPi)
            lfoPhase_ -= kTwoPi;
        const std::array<double, 2> wobble{std::sin(lfoPhase_), std::cos(lfoPhase_)};
        std::array<double, 2> out{};
        for (std::size_t b = 0; b < 2; ++b)
        {
            double y = x + decay * tankOut_[1 - b];
            y = allPass(modulated_[b], kModulated[b] * s + excursion * (1.0 + wobble[b]) * 0.5, -decayDiffusion1, y);
            const double delayed = firstDelay_[b].read(kFirstDelay[b] * s - 1.0);
            firstDelay_[b].push(y);
            dampingState_[b] = delayed * (1.0 - damping) + dampingState_[b] * damping;
            y = allPass(secondAllPass_[b], kSecondAllPass[b] * s, decayDiffusion2, dampingState_[b] * decay);
            out[b] = secondDelay_[b].read(kSecondDelay[b] * s - 1.0);
            secondDelay_[b].push(y);
        }
        tankOut_ = out;

        // Output taps across both branches.
        const auto tap = [&](const Line& line, double position) { return line.read(position * s); };
        double wetLeft = kTapGain * (tap(firstDelay_[1], 266.0) + tap(firstDelay_[1], 2974.0) - tap(secondAllPass_[1], 1913.0) +
                                     tap(secondDelay_[1], 1996.0) - tap(firstDelay_[0], 1990.0) - tap(secondAllPass_[0], 187.0) -
                                     tap(secondDelay_[0], 1066.0));
        double wetRight = kTapGain * (tap(firstDelay_[0], 353.0) + tap(firstDelay_[0], 3627.0) - tap(secondAllPass_[0], 1228.0) +
                                      tap(secondDelay_[0], 2673.0) - tap(firstDelay_[1], 2111.0) - tap(secondAllPass_[1], 335.0) -
                                      tap(secondDelay_[1], 121.0));
        wetLeft = lowCutFilter_[0].process(wetLeft, lowCut);
        wetRight = lowCutFilter_[1].process(wetRight, lowCut);
        const double mid = 0.5 * (wetLeft + wetRight);
        const double side = 0.5 * (wetLeft - wetRight) * width;

        left[i] = dryLeft * (1.0 - wet) + (mid + side) * wet;
        right[i] = dryRight * (1.0 - wet) + (mid - side) * wet;
    }
}

} // namespace dojo::devices

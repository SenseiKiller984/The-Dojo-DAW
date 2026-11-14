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
constexpr double kMaxModulationScale = 1.6;
constexpr double kTapGain = 0.6;
/** One pass around the whole tank (both branches), in reference samples. */
constexpr double kLoopLength = 672.0 + 4453.0 + 1800.0 + 3720.0 + 908.0 + 4217.0 + 2656.0 + 3163.0;
} // namespace

PlateReverb::Character PlateReverb::characterOf(int plate) noexcept
{
    //                                size  diffusion damping modulation lowDecay
    constexpr std::array<Character, 3> plates{{{1.0, 1.0, 1.0, 1.0, 1.0},     // Steel: the classic studio plate
                                               {0.75, 1.12, 1.8, 0.6, 0.8},   // Gold Foil: small, dense, bright, tight lows
                                               {1.2, 0.9, 1.15, 1.6, 1.1}}}; // Aluminium: large, open, shimmering
    return plates[static_cast<std::size_t>(std::clamp(plate, 0, 2))];
}

const juce::StringArray& PlateReverb::divisionNames()
{
    static const juce::StringArray names{"1/64", "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/8D", "1/4"};
    return names;
}

double PlateReverb::beatsForDivision(int index) noexcept
{
    constexpr std::array<double, 8> beats{1.0 / 16.0, 0.125, 1.0 / 6.0, 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0};
    return beats[static_cast<std::size_t>(std::clamp(index, 0, 7))];
}

PlateReverb::PlateReverb()
    : BuiltinProcessor(kTypeId, "Plate Reverb",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    plate_ = addChoice("plate", "Plate", {"Steel", "Gold Foil", "Aluminium"}, 0);
    preDelay_ = addFloat("predelay", "Pre-delay", skewedRange(0.0f, static_cast<float>(kMaxPreDelayMs), 30.0f), 10.0f, "ms");
    sync_ = addBool("sync", "Pre-delay Sync", false);
    division_ = addChoice("division", "Division", divisionNames(), 1);
    decay_ = addFloat("decay", "Decay", skewedRange(0.2f, 20.0f, 2.0f), 1.5f, "s");
    size_ = addFloat("size", "Size", {50.0f, 150.0f, 0.1f}, 100.0f, "%");
    damping_ = addFloat("damping", "Damping", skewedRange(1000.0f, 20000.0f, 6000.0f), 9000.0f, "Hz");
    lowDecay_ = addFloat("lowdecay", "Low Decay", {25.0f, 200.0f, 1.0f}, 100.0f, "%");
    lowFrequency_ = addFloat("lowfreq", "Low Freq", skewedRange(50.0f, 1000.0f, 250.0f), 250.0f, "Hz");
    bandwidth_ = addFloat("bandwidth", "Bandwidth", skewedRange(1000.0f, 20000.0f, 6000.0f), 16000.0f, "Hz");
    diffusion_ = addFloat("diffusion", "Diffusion", {0.0f, 100.0f, 0.1f}, 70.0f, "%");
    modRate_ = addFloat("modrate", "Mod Rate", skewedRange(0.1f, 5.0f, 1.0f), 0.8f, "Hz");
    modulation_ = addFloat("modulation", "Mod Depth", {0.0f, 100.0f, 0.1f}, 30.0f, "%");
    drive_ = addFloat("drive", "Drive", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    ducking_ = addFloat("ducking", "Ducking", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    freeze_ = addBool("freeze", "Freeze", false);
    lowCut_ = addFloat("lowcut", "Low Cut", skewedRange(20.0f, 1000.0f, 150.0f), 100.0f, "Hz");
    highCut_ = addFloat("highcut", "High Cut", skewedRange(1000.0f, 20000.0f, 6000.0f), 20000.0f, "Hz");
    width_ = addFloat("width", "Width", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 25.0f, "%");
}

double PlateReverb::getTailLengthSeconds() const
{
    if (freeze_->get())
        return 3600.0;
    return std::max(static_cast<double>(decay_->get()), getLowDecaySeconds()) + getPreDelayMs() / 1000.0;
}

double PlateReverb::decayGainFor(double seconds) const noexcept
{
    // The signal passes four decay stages per trip around the tank: g^4 per loop, -60 dB after the time.
    const double loopSeconds = kLoopLength / kReferenceRate * size_->get() / 100.0 * characterOf(plate_->getIndex()).size;
    return std::min(0.9995, std::pow(10.0, -3.0 * loopSeconds / (4.0 * std::max(0.05, seconds))));
}

double PlateReverb::getDecayGain() const noexcept
{
    return decayGainFor(decay_->get());
}

double PlateReverb::getLowDecaySeconds() const noexcept
{
    return decay_->get() * lowDecay_->get() / 100.0 * characterOf(plate_->getIndex()).lowDecay;
}

double PlateReverb::getPreDelayMs() const noexcept
{
    if (!sync_->get())
        return preDelay_->get();
    const double ms = beatsForDivision(division_->getIndex()) * 60000.0 / std::max(20.0, tempo_.load(std::memory_order_relaxed));
    return std::min(ms, kMaxSyncedPreDelayMs);
}

void PlateReverb::prepareDsp(double sampleRate, int)
{
    scale_ = sampleRate / kReferenceRate;
    const double largest = kMaxSize * kMaxPlateScale;
    const auto room = [&](double referenceLength, double extra = 0.0)
    { return static_cast<int>(std::ceil((referenceLength * largest + extra) * scale_)) + 8; };

    preDelayLine_.setMaximum(static_cast<int>(std::ceil(kMaxSyncedPreDelayMs / 1000.0 * sampleRate)) + 8);
    for (std::size_t i = 0; i < diffusers_.size(); ++i)
        diffusers_[i].setMaximum(room(kDiffusers[i]));
    for (std::size_t b = 0; b < 2; ++b)
    {
        modulated_[b].setMaximum(room(kModulated[b], 2.0 * kExcursion * kMaxModulationScale + 4.0));
        firstDelay_[b].setMaximum(room(kFirstDelay[b]));
        secondAllPass_[b].setMaximum(room(kSecondAllPass[b]));
        secondDelay_[b].setMaximum(room(kSecondDelay[b]));
    }
    dampingState_.fill(0.0);
    tankOut_.fill(0.0);
    for (auto& branch : lowState_)
        branch.fill(0.0);
    bandwidthState_ = 0.0;
    lfoPhase_ = 0.0;
    preDelaySamples_ = -1.0;
    inputGain_ = freeze_->get() ? 0.0 : 1.0;
    duckEnvelope_ = 0.0;
    for (auto& f : lowCutFilter_)
        f.reset();
    for (auto& f : highCutFilter_)
        f.reset();
}

void PlateReverb::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            if (const auto bpm = position->getBpm())
                tempo_.store(*bpm, std::memory_order_relaxed);

    const auto character = characterOf(plate_->getIndex());
    const bool frozen = freeze_->get();
    const double size = size_->get() / 100.0 * character.size;
    const double s = scale_ * size; // samples per reference sample, at this size
    const double preDelayTarget = std::min(getPreDelayMs() / 1000.0 * sampleRate_, static_cast<double>(preDelayLine_.capacity() - 4));
    if (preDelaySamples_ < 0.0)
        preDelaySamples_ = preDelayTarget;
    const double glide = 1.0 - std::exp(-1.0 / (0.02 * sampleRate_));
    const double decay = frozen ? kFreezeGain : getDecayGain();
    const double lowDecay = frozen ? kFreezeGain : decayGainFor(getLowDecaySeconds());
    const double lowSplit = 1.0 - std::exp(-kTwoPi * lowFrequency_->get() / sampleRate_);
    const double diffusion = std::min(1.0, diffusion_->get() / 100.0 * character.diffusion);
    const double inputDiffusion1 = std::min(0.85, 0.75 * diffusion / 0.7);
    const double inputDiffusion2 = std::min(0.75, 0.625 * diffusion / 0.7);
    const double decayDiffusion1 = std::clamp(0.5 + 0.2 * diffusion / 0.7, 0.4, 0.75);
    const double decayDiffusion2 = std::clamp(decay + 0.15, 0.25, 0.5);
    const double bandwidth = 1.0 - std::exp(-kTwoPi * bandwidth_->get() / sampleRate_);
    const double dampingFrequency = std::min(damping_->get() * character.damping, 0.45 * sampleRate_);
    const double damping = frozen ? 0.0 : std::exp(-kTwoPi * dampingFrequency / sampleRate_);
    const double excursion = modulation_->get() / 100.0 * character.modulation * 2.0 * kExcursion * scale_;
    const double lfoStep = kTwoPi * modRate_->get() / sampleRate_;
    const double driveAmount = drive_->get() / 100.0;
    const double driveGain = 1.0 + 4.0 * driveAmount;
    const double ducking = ducking_->get() / 100.0;
    const double duckAttack = dsp::onePoleCoefficient(0.005, sampleRate_);
    const double duckRelease = dsp::onePoleCoefficient(0.2, sampleRate_);
    const double fade = 1.0 - std::exp(-1.0 / (0.01 * sampleRate_)); // (Freeze fades the input in 10 ms)
    const double width = width_->get() / 100.0;
    const double wet = mix_->get() / 100.0;
    const auto lowCut = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::lowCut, sampleRate_, lowCut_->get(), 0.7071, 0.0);
    const bool highCutOn = highCut_->get() < 19999.0f;
    const auto highCut = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::highCut, sampleRate_,
                                                       std::min(static_cast<double>(highCut_->get()), 0.45 * sampleRate_), 0.7071, 0.0);

    /** A decay stage: the mids and highs by `decay`, the lows (below Low Freq) by `lowDecay`. */
    const auto decayStage = [&](double x, double& lowState) noexcept
    {
        lowState += lowSplit * (x - lowState);
        return x * decay + lowState * (lowDecay - decay);
    };

    for (int i = 0; i < numSamples; ++i)
    {
        const double dryLeft = left[i], dryRight = right[i];

        // Input: drive, pre-delay, bandwidth, four diffusers.
        inputGain_ += ((frozen ? 0.0 : 1.0) - inputGain_) * fade;
        double x = 0.5 * (dryLeft + dryRight) * inputGain_;
        if (driveAmount > 0.0)
            x = std::tanh(driveGain * x) / driveGain * (1.0 + 0.5 * driveAmount); // (the driver saturating)
        preDelayLine_.push(x);
        preDelaySamples_ += (preDelayTarget - preDelaySamples_) * glide;
        x = preDelayLine_.read(preDelaySamples_);
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
            double y = x + decayStage(tankOut_[1 - b], lowState_[b][0]);
            y = allPass(modulated_[b], kModulated[b] * s + excursion * (1.0 + wobble[b]) * 0.5, -decayDiffusion1, y);
            const double delayed = firstDelay_[b].read(kFirstDelay[b] * s - 1.0);
            firstDelay_[b].push(y);
            dampingState_[b] = delayed * (1.0 - damping) + dampingState_[b] * damping;
            y = allPass(secondAllPass_[b], kSecondAllPass[b] * s, decayDiffusion2, decayStage(dampingState_[b], lowState_[b][1]));
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
        if (highCutOn)
        {
            wetLeft = highCutFilter_[0].process(wetLeft, highCut);
            wetRight = highCutFilter_[1].process(wetRight, highCut);
        }

        // Ducking: the dry signal's level turns the reverb down (up to 24 dB at -12 dBFS and above).
        double duck = 1.0;
        if (ducking > 0.0)
        {
            const double level = std::max(std::abs(dryLeft), std::abs(dryRight));
            duckEnvelope_ = level + (duckEnvelope_ - level) * (level > duckEnvelope_ ? duckAttack : duckRelease);
            const double amount = std::clamp((dsp::gainToDb(duckEnvelope_) + 48.0) / 36.0, 0.0, 1.0);
            duck = dsp::dbToGain(-24.0 * ducking * amount);
        }

        const double mid = 0.5 * (wetLeft + wetRight) * duck;
        const double side = 0.5 * (wetLeft - wetRight) * width * duck;
        left[i] = dryLeft * (1.0 - wet) + (mid + side) * wet;
        right[i] = dryRight * (1.0 - wet) + (mid - side) * wet;
    }
}

} // namespace dojo::devices

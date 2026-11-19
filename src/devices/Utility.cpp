#include "devices/Utility.h"

#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ = 0.70710678118654752;
} // namespace

Utility::Utility()
    : BuiltinProcessor(kTypeId, "Utility",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    gain_ = addFloat("gain", "Gain", {-35.0f, 35.0f, 0.01f}, 0.0f, "dB");
    width_ = addFloat("width", "Width", {0.0f, 200.0f, 0.1f}, 100.0f, "%");
    mono_ = addBool("mono", "Mono", false);
    bassMono_ = addBool("bassmono", "Bass Mono", false);
    bassFrequency_ = addFloat("bassfreq", "Bass Freq", skewedRange(50.0f, 500.0f, 150.0f), 120.0f, "Hz");
    phaseLeft_ = addBool("phasel", "Phase L", false);
    phaseRight_ = addBool("phaser", "Phase R", false);
    channel_ = addChoice("channel", "Channel", {"Stereo", "Left", "Right", "Swap"}, 0);
    balance_ = addFloat("balance", "Balance", {-100.0f, 100.0f, 0.1f}, 0.0f, "");
    mute_ = addBool("mute", "Mute", false);
    dc_ = addBool("dc", "DC Filter", false);
}

void Utility::prepareDsp(double sampleRate, int)
{
    smoothing_ = 1.0 - std::exp(-1.0 / (0.01 * sampleRate)); // 10 ms
    muteStep_ = 1.0 / (0.01 * sampleRate);
    dcCoefficient_ = std::exp(-2.0 * kPi * 5.0 / sampleRate);
    dcState_ = {};
    dcInput_ = {};
    for (auto* filters : {&midLow_, &midHigh_, &sideHigh_})
        for (auto& f : *filters)
            f.reset();

    const double gain = juce::Decibels::decibelsToGain(static_cast<double>(gain_->get()), -100.0);
    const double balance = balance_->get() / 100.0;
    gainLeft_ = gain * std::min(1.0, 1.0 - balance);
    gainRight_ = gain * std::min(1.0, 1.0 + balance);
    widthValue_ = mono_->get() ? 0.0 : width_->get() / 100.0;
    muteGain_ = mute_->get() ? 0.0 : 1.0;
    crossoverFor_ = -1.0f;
    updateCrossover();
}

void Utility::updateCrossover() noexcept
{
    const float frequency = bassFrequency_->get();
    if (std::abs(frequency - crossoverFor_) < 1.0e-3f)
        return;
    crossoverFor_ = frequency;
    lowPass_ = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::highCut, sampleRate_, frequency, kButterworthQ, 0.0);
    highPass_ = dsp::BiquadCoefficients::make(dsp::BiquadCoefficients::Shape::lowCut, sampleRate_, frequency, kButterworthQ, 0.0);
}

void Utility::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);

    const double gain = juce::Decibels::decibelsToGain(static_cast<double>(gain_->get()), -100.0);
    const double balance = balance_->get() / 100.0;
    const double targetLeft = gain * std::min(1.0, 1.0 - balance);
    const double targetRight = gain * std::min(1.0, 1.0 + balance);
    const double targetWidth = mono_->get() ? 0.0 : width_->get() / 100.0;
    const double targetMute = mute_->get() ? 0.0 : 1.0;
    const int channel = channel_->getIndex();
    const bool flipLeft = phaseLeft_->get();
    const bool flipRight = phaseRight_->get();
    const bool dc = dc_->get();
    const bool bassMono = bassMono_->get();
    if (bassMono)
        updateCrossover();

    for (int i = 0; i < numSamples; ++i)
    {
        double l = left[i];
        double r = right[i];

        if (dc)
        {
            // y[n] = x[n] - x[n-1] + c * y[n-1]
            const double yl = l - dcInput_[0] + dcCoefficient_ * dcState_[0];
            const double yr = r - dcInput_[1] + dcCoefficient_ * dcState_[1];
            dcInput_[0] = l;
            dcInput_[1] = r;
            dcState_[0] = l = yl;
            dcState_[1] = r = yr;
        }

        switch (channel)
        {
        case 1: r = l; break;
        case 2: l = r; break;
        case 3: std::swap(l, r); break;
        default: break;
        }

        if (flipLeft)
            l = -l;
        if (flipRight)
            r = -r;

        widthValue_ += (targetWidth - widthValue_) * smoothing_;
        double mid = 0.5 * (l + r);
        double side = 0.5 * (l - r);

        if (bassMono)
        {
            // Linkwitz-Riley 4th order: LP and HP sum to an all-pass, so the mid keeps its level
            // and the side loses only what is below the crossover.
            const double midLow = midLow_[1].process(midLow_[0].process(mid, lowPass_), lowPass_);
            const double midHigh = midHigh_[1].process(midHigh_[0].process(mid, highPass_), highPass_);
            mid = midLow + midHigh;
            side = sideHigh_[1].process(sideHigh_[0].process(side, highPass_), highPass_);
        }

        side *= widthValue_;
        l = mid + side;
        r = mid - side;

        gainLeft_ += (targetLeft - gainLeft_) * smoothing_;
        gainRight_ += (targetRight - gainRight_) * smoothing_;
        // Mute ramps linearly (10 ms) so it reaches true silence.
        muteGain_ = targetMute > muteGain_ ? std::min(targetMute, muteGain_ + muteStep_) : std::max(targetMute, muteGain_ - muteStep_);
        left[i] = l * gainLeft_ * muteGain_;
        right[i] = r * gainRight_ * muteGain_;
    }

    // Settle exactly on the targets (no endless asymptotic tails).
    if (std::abs(widthValue_ - targetWidth) < 1.0e-9)
        widthValue_ = targetWidth;
}

} // namespace dojo::devices

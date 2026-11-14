#include "devices/SoftClipper.h"

#include "devices/dsp/Biquad.h"
#include "devices/dsp/DynamicsMath.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
} // namespace

SoftClipper::SoftClipper()
    : BuiltinProcessor(kTypeId, "Soft Clipper",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    drive_ = addFloat("drive", "Input", {0.0f, 24.0f, 0.1f}, 0.0f, "dB");
    ceiling_ = addFloat("ceiling", "Ceiling", {-24.0f, 0.0f, 0.1f}, -0.3f, "dB");
    shape_ = addChoice("shape", "Shape", {"Soft", "Smooth", "Hard", "Tube"}, 0);
    softness_ = addFloat("softness", "Softness", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    asymmetry_ = addFloat("asymmetry", "Asymmetry", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    link_ = addBool("link", "Stereo Link", true);
    autoGain_ = addBool("autogain", "Auto Gain", false);
    listen_ = addChoice("listen", "Listen", {"Normal", "Delta"}, 0);
    guard_ = addBool("guard", "Ceiling Guard", true);
    osRealtime_ = addChoice("osrealtime", "Oversampling", dsp::PhaseOversampler::factorNames(), 1, false);
    osOffline_ = addChoice("osoffline", "Offline Oversampling", dsp::PhaseOversampler::factorNames(), 3, false);
    osPhase_ = addChoice("osphase", "Oversampling Phase", dsp::PhaseOversampler::phaseNames(), 2, false);
    output_ = addFloat("output", "Output", {-24.0f, 6.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    for (auto* parameter : {static_cast<juce::AudioProcessorParameter*>(osRealtime_), static_cast<juce::AudioProcessorParameter*>(osOffline_),
                            static_cast<juce::AudioProcessorParameter*>(osPhase_)})
        parameter->addListener(this);
}

SoftClipper::~SoftClipper()
{
    for (auto* parameter : {static_cast<juce::AudioProcessorParameter*>(osRealtime_), static_cast<juce::AudioProcessorParameter*>(osOffline_),
                            static_cast<juce::AudioProcessorParameter*>(osPhase_)})
        parameter->removeListener(this);
}

double SoftClipper::unitShape(double x, int shape, double softness) noexcept
{
    const double u = std::abs(x);
    const double knee = 1.0 - std::clamp(softness, 0.0, 1.0); // where the rounding starts
    double y = 0.0;
    if (shape == 2 || u <= knee)
        y = std::min(u, 1.0); // (hard, or below the knee)
    else if (knee >= 1.0)
        y = 1.0;
    else
    {
        const double span = 1.0 - knee;
        const double v = (u - knee) / span;
        if (shape == 1)
            y = knee + span * (2.0 / kPi) * std::atan(kPi * 0.5 * v); // smooth
        else if (shape == 3)
            y = knee + span * (1.0 - std::exp(-v)); // tube: a gradual, second-order bend
        else
            y = knee + span * std::tanh(v); // soft
    }
    return std::copysign(y, x);
}

double SoftClipper::curve(double u, int shape, double softness, double negativeCeiling) noexcept
{
    if (u >= 0.0 || negativeCeiling >= 1.0)
        return unitShape(u, shape, softness);
    return negativeCeiling * unitShape(u / negativeCeiling, shape, softness); // (asymmetry)
}

double SoftClipper::shape(double x) const noexcept
{
    const double ceiling = dsp::dbToGain(ceiling_->get());
    const double negative = dsp::dbToGain(-kMaxAsymmetryDb * asymmetry_->get() / 100.0);
    return ceiling * curve(x * dsp::dbToGain(drive_->get()) / ceiling, shape_->getIndex(), softness_->get() / 100.0, negative);
}

int SoftClipper::factorChoice() const noexcept
{
    return isNonRealtime() ? osOffline_->getIndex() : osRealtime_->getIndex();
}

int SoftClipper::currentLatency() const noexcept
{
    return oversampler_.latencyOf(factorChoice(), osPhase_->getIndex());
}

void SoftClipper::setNonRealtime(bool isNonRealtime) noexcept
{
    BuiltinProcessor::setNonRealtime(isNonRealtime);
    setLatencySamples(currentLatency());
}

void SoftClipper::parameterValueChanged(int, float)
{
    setLatencySamples(currentLatency());
}

void SoftClipper::prepareDsp(double, int maximumBlockSize)
{
    oversampler_.prepare(maximumBlockSize);
    oversampler_.select(factorChoice(), osPhase_->getIndex());
    oversampler_.reset();
    dcIn_.fill(0.0);
    dcOut_.fill(0.0);
    setLatencySamples(currentLatency());
}

void SoftClipper::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    oversampler_.select(factorChoice(), osPhase_->getIndex());
    const double rate = sampleRate_ * oversampler_.getRateMultiple();

    const double drive = dsp::dbToGain(drive_->get());
    const double ceiling = dsp::dbToGain(ceiling_->get());
    const int shapeIndex = shape_->getIndex();
    const double softness = softness_->get() / 100.0;
    const double negative = dsp::dbToGain(-kMaxAsymmetryDb * asymmetry_->get() / 100.0);
    const bool asymmetric = negative < 0.9999;
    const double dcPole = 1.0 - 2.0 * kPi * 10.0 / rate; // (a 10 Hz DC blocker)
    const bool link = link_->get();
    const bool delta = listen_->getIndex() == 1;
    const bool guard = guard_->get();
    const double post = dsp::dbToGain(output_->get()) / (autoGain_->get() ? drive : 1.0);
    const double wet = mix_->get() / 100.0;
    double peakIn = 0.0, peakOut = 0.0;

    const auto clip = [&](double** data, std::size_t count) noexcept
    {
        for (std::size_t j = 0; j < count; ++j)
        {
            const double x[2] = {data[0][j] * drive, data[1][j] * drive};
            double y[2] = {ceiling * curve(x[0] / ceiling, shapeIndex, softness, negative),
                           ceiling * curve(x[1] / ceiling, shapeIndex, softness, negative)};
            if (link)
            {
                // One gain for both: the stronger reduction of the two.
                double gain = 1.0;
                for (std::size_t ch = 0; ch < 2; ++ch)
                    if (std::abs(x[ch]) > 1.0e-12)
                        gain = std::min(gain, y[ch] / x[ch]);
                y[0] = x[0] * gain;
                y[1] = x[1] * gain;
            }
            for (std::size_t ch = 0; ch < 2; ++ch)
            {
                if (asymmetric)
                {
                    const double blocked = y[ch] - dcIn_[ch] + dcPole * dcOut_[ch];
                    dcIn_[ch] = y[ch];
                    dcOut_[ch] = blocked;
                    y[ch] = blocked;
                }
                peakIn = std::max(peakIn, std::abs(x[ch]));
                peakOut = std::max(peakOut, std::abs(y[ch]));
                data[ch][j] = delta ? x[ch] - y[ch] : y[ch];
            }
        }
    };

    oversampler_.process(channels, numSamples, clip);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < numSamples; ++i)
        {
            if (delta)
            {
                channels[ch][i] *= post;
                continue;
            }
            // Ceiling Guard: the ceiling holds after the downsampling filter too.
            const double y = guard ? std::clamp(channels[ch][i], -ceiling, ceiling) : channels[ch][i];
            channels[ch][i] = oversampler_.dry(ch, i) * (1.0 - wet) + y * post * wet;
        }

    if (peakIn > 1.0e-9)
        storeMax(clip_, static_cast<float>(std::max(0.0, dsp::gainToDb(peakIn) - dsp::gainToDb(std::max(peakOut, 1.0e-12)))));
}

} // namespace dojo::devices

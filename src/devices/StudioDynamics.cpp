#include "devices/StudioDynamics.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
using Shape = dsp::BiquadCoefficients::Shape;
constexpr double kPi = 3.14159265358979323846;

/** Over-threshold amount with a soft knee of `knee` dB: 0 below, `over` well above, quadratic between. */
double softOver(double over, double knee) noexcept
{
    if (knee <= 0.0)
        return std::max(0.0, over);
    if (over <= -0.5 * knee)
        return 0.0;
    if (over >= 0.5 * knee)
        return over;
    const double t = over + 0.5 * knee;
    return t * t / (2.0 * knee);
}
} // namespace

//==================================================================================================
// De-Esser

DeEsser::DeEsser()
    : BuiltinProcessor(kTypeId, "De-Esser",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    frequency_ = addFloat("freq", "Frequency", skewedRange(2000.0f, 16000.0f, 6000.0f), 6500.0f, "Hz");
    threshold_ = addFloat("threshold", "Threshold", {-60.0f, 0.0f, 0.1f}, -24.0f, "dB");
    range_ = addFloat("range", "Range", {0.0f, 24.0f, 0.1f}, 8.0f, "dB");
    mode_ = addChoice("mode", "Mode", {"Split", "Wide"}, 0);
    detection_ = addChoice("detection", "Detection", {"High-Pass", "Band-Pass"}, 0);
    release_ = addFloat("release", "Release", skewedRange(10.0f, 300.0f, 60.0f), 60.0f, "ms");
    listen_ = addBool("listen", "Listen", false);
}

double DeEsser::reductionFor(double levelDb) const noexcept
{
    const double over = softOver(levelDb - threshold_->get(), kKneeDb);
    return std::min(static_cast<double>(range_->get()), over * (1.0 - 1.0 / kRatio));
}

void DeEsser::prepareDsp(double, int)
{
    for (auto& channel : detectFilter_)
        for (auto& stage : channel)
            stage.reset();
    for (auto& s : split_)
        s.reset();
    envelope_ = 0.0;
    smoothedFrequency_ = frequency_->get();
}

void DeEsser::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};

    // (The frequency glides, so sweeping it does not click.)
    smoothedFrequency_ += (frequency_->get() - smoothedFrequency_) * 0.5;
    const double frequency = smoothedFrequency_;
    const bool bandPass = detection_->getIndex() == 1;
    const auto first = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, frequency, bandPass ? 1.4 : 0.7071, 0.0);
    const auto second = bandPass ? dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, frequency, 1.4, 0.0)
                                 : dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, frequency, 0.7071, 0.0);
    for (auto& s : split_)
        s.setFrequency(frequency, sampleRate_);

    const double attack = dsp::onePoleCoefficient(kAttackSeconds, sampleRate_);
    const double release = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);
    const bool split = mode_->getIndex() == 0;
    const bool listen = listen_->get();
    double maxReduction = 0.0, maxLevel = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        double detected[2];
        for (std::size_t ch = 0; ch < 2; ++ch)
            detected[ch] = detectFilter_[ch][1].process(detectFilter_[ch][0].process(channels[ch][i], first), second);
        const double level = std::max(std::abs(detected[0]), std::abs(detected[1]));
        envelope_ = level + (envelope_ - level) * (level > envelope_ ? attack : release);
        maxLevel = std::max(maxLevel, envelope_);
        const double reductionDb = reductionFor(dsp::gainToDb(envelope_));
        maxReduction = std::max(maxReduction, reductionDb);
        const double gain = reductionDb > 1.0e-4 ? dsp::dbToGain(-reductionDb) : 1.0;

        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            double& x = channels[ch][i];
            if (listen)
            {
                x = detected[ch];
                continue;
            }
            if (split)
            {
                double low = 0.0, high = 0.0;
                split_[ch].split(x, low, high);
                x = low + high * gain;
            }
            else
            {
                x *= gain;
            }
        }
    }

    storeMax(reduction_, static_cast<float>(maxReduction));
    storeMax(detector_, static_cast<float>(dsp::gainToDb(maxLevel)));
}

//==================================================================================================
// Glue Compressor

GlueCompressor::GlueCompressor()
    : BuiltinProcessor(kTypeId, "Glue Compressor",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    threshold_ = addFloat("threshold", "Threshold", {-40.0f, 0.0f, 0.1f}, -15.0f, "dB");
    ratio_ = addChoice("ratio", "Ratio", {"2:1", "4:1", "10:1"}, 1);
    attack_ = addChoice("attack", "Attack", {"0.01 ms", "0.1 ms", "0.3 ms", "1 ms", "3 ms", "10 ms", "30 ms"}, 5);
    release_ = addChoice("release", "Release", {"0.1 s", "0.2 s", "0.4 s", "0.6 s", "0.8 s", "1.2 s", "Auto"}, 6);
    makeup_ = addFloat("makeup", "Makeup", {0.0f, 20.0f, 0.1f}, 0.0f, "dB");
    range_ = addFloat("range", "Range", {0.0f, 60.0f, 0.1f}, 60.0f, "dB");
    scHighPass_ = addFloat("schp", "SC High-Pass", skewedRange(20.0f, 500.0f, 100.0f), 20.0f, "Hz");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    softClip_ = addBool("softclip", "Soft Clip", false);
}

double GlueCompressor::ratioOf(int choice) noexcept
{
    constexpr std::array<double, 3> ratios{2.0, 4.0, 10.0};
    return ratios[static_cast<std::size_t>(std::clamp(choice, 0, 2))];
}

double GlueCompressor::attackMsOf(int choice) noexcept
{
    constexpr std::array<double, 7> ms{0.01, 0.1, 0.3, 1.0, 3.0, 10.0, 30.0};
    return ms[static_cast<std::size_t>(std::clamp(choice, 0, 6))];
}

double GlueCompressor::releaseSecondsOf(int choice) noexcept
{
    constexpr std::array<double, 7> seconds{0.1, 0.2, 0.4, 0.6, 0.8, 1.2, -1.0};
    return seconds[static_cast<std::size_t>(std::clamp(choice, 0, 6))];
}

double GlueCompressor::reductionFor(double levelDb) const noexcept
{
    const double over = softOver(levelDb - threshold_->get(), kKneeDb);
    return std::min(static_cast<double>(range_->get()), over * (1.0 - 1.0 / ratioOf(ratio_->getIndex())));
}

double GlueCompressor::softClip(double x) noexcept
{
    // Unchanged up to about -3 dBFS, then rounded towards -0.5 dBFS.
    constexpr double knee = 0.7;
    const double magnitude = std::abs(x);
    if (magnitude <= knee)
        return x;
    const double span = kSoftClipCeiling - knee;
    return std::copysign(knee + span * std::tanh((magnitude - knee) / span), x);
}

void GlueCompressor::prepareDsp(double, int)
{
    for (auto& f : scFilter_)
        f.reset();
    fastDb_ = slowDb_ = 0.0;
}

void GlueCompressor::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};

    const double attack = dsp::onePoleCoefficient(attackMsOf(attack_->getIndex()) / 1000.0, sampleRate_);
    const double releaseSeconds = releaseSecondsOf(release_->getIndex());
    const bool autoRelease = releaseSeconds < 0.0;
    // Auto: a fast release (0.1 s) after short peaks; a slow copy of the reduction (built up over
    // half a second, released over 1.2 s) holds it once compression is sustained.
    const double release = dsp::onePoleCoefficient(autoRelease ? 0.1 : releaseSeconds, sampleRate_);
    const double slowAttack = dsp::onePoleCoefficient(0.5, sampleRate_);
    const double slowRelease = dsp::onePoleCoefficient(1.2, sampleRate_);
    const bool highPass = scHighPass_->get() > 20.5f;
    const auto hp = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, scHighPass_->get(), 0.7071, 0.0);
    const double makeup = dsp::dbToGain(makeup_->get());
    const double wet = mix_->get() / 100.0;
    const bool clip = softClip_->get();
    double maxReduction = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        double l = channels[0][i], r = channels[1][i];
        double kl = l, kr = r;
        if (highPass)
        {
            kl = scFilter_[0].process(kl, hp);
            kr = scFilter_[1].process(kr, hp);
        }
        const double target = reductionFor(dsp::gainToDb(std::max(std::abs(kl), std::abs(kr))));
        // (in dB: rising reduction is the attack, falling the release)
        fastDb_ = target + (fastDb_ - target) * (target > fastDb_ ? attack : release);
        double reduction = fastDb_;
        if (autoRelease)
        {
            slowDb_ = fastDb_ + (slowDb_ - fastDb_) * (fastDb_ > slowDb_ ? slowAttack : slowRelease);
            reduction = std::max(fastDb_, slowDb_);
        }
        maxReduction = std::max(maxReduction, reduction);
        const double gain = dsp::dbToGain(-reduction) * makeup;

        double outL = l * (1.0 - wet) + l * gain * wet;
        double outR = r * (1.0 - wet) + r * gain * wet;
        if (clip)
        {
            outL = softClip(outL);
            outR = softClip(outR);
        }
        channels[0][i] = outL;
        channels[1][i] = outR;
    }

    storeMax(reduction_, static_cast<float>(maxReduction));
}

//==================================================================================================
// Expander

Expander::Expander()
    : BuiltinProcessor(kTypeId, "Expander",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Sidechain", juce::AudioChannelSet::stereo(), true),
                       false)
{
    threshold_ = addFloat("threshold", "Threshold", {-80.0f, 0.0f, 0.1f}, -40.0f, "dB");
    ratio_ = addFloat("ratio", "Ratio", skewedRange(1.0f, 10.0f, 2.5f), 2.0f, ":1");
    range_ = addFloat("range", "Range", {0.0f, 80.0f, 0.1f}, 20.0f, "dB");
    knee_ = addFloat("knee", "Knee", {0.0f, 24.0f, 0.1f}, 6.0f, "dB");
    attack_ = addFloat("attack", "Attack", skewedRange(0.05f, 100.0f, 5.0f), 1.0f, "ms");
    release_ = addFloat("release", "Release", skewedRange(5.0f, 2000.0f, 150.0f), 150.0f, "ms");
    external_ = addBool("sidechain", "External Sidechain", false);
    keyLow_ = addFloat("keylow", "Key High-Pass", skewedRange(20.0f, 2000.0f, 200.0f), 20.0f, "Hz");
    output_ = addFloat("output", "Output", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
}

double Expander::attenuationFor(double levelDb) const noexcept
{
    const double under = softOver(threshold_->get() - levelDb, knee_->get());
    return std::min(static_cast<double>(range_->get()), under * (ratio_->get() - 1.0));
}

void Expander::prepareDsp(double, int)
{
    for (auto& f : keyFilter_)
        f.reset();
    level_ = 0.0;
    attenuationDb_ = 0.0;
}

void Expander::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const bool external = external_->get() && buffer.getNumChannels() >= 4;
    const double* keyLeft = external ? buffer.getReadPointer(2) : left;
    const double* keyRight = external ? buffer.getReadPointer(3) : right;

    // The detector: instant attack, a 20 ms decay (rides over the waveform). The attenuation then
    // opens with Attack and closes with Release.
    const double detectorDecay = dsp::onePoleCoefficient(0.02, sampleRate_);
    const double opening = dsp::onePoleCoefficient(attack_->get() / 1000.0, sampleRate_);
    const double closing = dsp::onePoleCoefficient(release_->get() / 1000.0, sampleRate_);
    const bool highPass = keyLow_->get() > 20.5f;
    const auto hp = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate_, keyLow_->get(), 0.7071, 0.0);
    const double output = dsp::dbToGain(output_->get());
    double maxAttenuation = 0.0, maxLevel = 0.0;

    for (int i = 0; i < numSamples; ++i)
    {
        double kl = keyLeft[i], kr = keyRight[i];
        if (highPass)
        {
            kl = keyFilter_[0].process(kl, hp);
            kr = keyFilter_[1].process(kr, hp);
        }
        level_ = std::max(std::max(std::abs(kl), std::abs(kr)), level_ * detectorDecay);
        maxLevel = std::max(maxLevel, level_);
        const double target = attenuationFor(dsp::gainToDb(level_));
        attenuationDb_ = target + (attenuationDb_ - target) * (target < attenuationDb_ ? opening : closing);
        maxAttenuation = std::max(maxAttenuation, attenuationDb_);
        const double gain = dsp::dbToGain(-attenuationDb_) * output;
        left[i] *= gain;
        right[i] *= gain;
    }

    storeMax(reduction_, static_cast<float>(maxAttenuation));
    storeMax(detector_, static_cast<float>(dsp::gainToDb(maxLevel)));
}

//==================================================================================================
// Soft Clipper

SoftClipper::SoftClipper()
    : BuiltinProcessor(kTypeId, "Soft Clipper",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    drive_ = addFloat("drive", "Drive", {0.0f, 24.0f, 0.1f}, 0.0f, "dB");
    ceiling_ = addFloat("ceiling", "Ceiling", {-24.0f, 0.0f, 0.1f}, -0.3f, "dB");
    shape_ = addChoice("shape", "Shape", {"Soft", "Smooth", "Hard"}, 0);
    softness_ = addFloat("softness", "Softness", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    oversampling_ = addChoice("oversampling", "Oversampling", {"Off", "2x", "4x"}, 1, false); // (latency)
    output_ = addFloat("output", "Output", {-24.0f, 6.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    oversampling_->addListener(this);
}

SoftClipper::~SoftClipper()
{
    oversampling_->removeListener(this);
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
    else if (shape == 1)
        y = knee + (1.0 - knee) * (2.0 / kPi) * std::atan(kPi * 0.5 * (u - knee) / (1.0 - knee));
    else
        y = knee + (1.0 - knee) * std::tanh((u - knee) / (1.0 - knee));
    return std::copysign(y, x);
}

double SoftClipper::shape(double x) const noexcept
{
    const double ceiling = dsp::dbToGain(ceiling_->get());
    return ceiling * unitShape(x / ceiling, shape_->getIndex(), softness_->get() / 100.0);
}

int SoftClipper::currentLatency() const noexcept
{
    const int choice = oversampling_->getIndex();
    return choice == 0 ? 0 : oversamplers_[static_cast<std::size_t>(choice - 1)].getLatency();
}

void SoftClipper::parameterValueChanged(int, float)
{
    setLatencySamples(currentLatency());
}

void SoftClipper::prepareDsp(double, int maximumBlockSize)
{
    oversamplers_[0].prepare(1, maximumBlockSize);
    oversamplers_[1].prepare(2, maximumBlockSize);
    setLatencySamples(currentLatency());
}

void SoftClipper::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    const double drive = dsp::dbToGain(drive_->get());
    const double ceiling = dsp::dbToGain(ceiling_->get());
    const int shapeIndex = shape_->getIndex();
    const double softness = softness_->get() / 100.0;
    const double output = dsp::dbToGain(output_->get());
    const double wet = mix_->get() / 100.0;
    const int choice = oversampling_->getIndex();
    double peakIn = 0.0, peakOut = 0.0;

    const auto clip = [&](double* data, std::size_t count)
    {
        for (std::size_t j = 0; j < count; ++j)
        {
            const double x = data[j] * drive;
            peakIn = std::max(peakIn, std::abs(x));
            data[j] = ceiling * unitShape(x / ceiling, shapeIndex, softness);
            peakOut = std::max(peakOut, std::abs(data[j]));
        }
    };

    if (choice == 0)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            double* data = channels[ch];
            for (int i = 0; i < numSamples; ++i)
            {
                const double dry = data[i];
                double y = dry;
                clip(&y, 1);
                data[i] = dry * (1.0 - wet) + y * output * wet;
            }
        }
    }
    else
    {
        auto& oversampler = oversamplers_[static_cast<std::size_t>(choice - 1)];
        oversampler.process(channels, numSamples, [&](double* data, std::size_t count, int) { clip(data, count); });
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
            {
                // The ceiling holds after the downsampling filter too.
                const double y = std::clamp(channels[ch][i], -ceiling, ceiling);
                channels[ch][i] = oversampler.dry(ch, i) * (1.0 - wet) + y * output * wet;
            }
    }

    if (peakIn > 1.0e-9)
        storeMax(clip_, static_cast<float>(std::max(0.0, dsp::gainToDb(peakIn) - dsp::gainToDb(std::max(peakOut, 1.0e-12)))));
}

} // namespace dojo::devices

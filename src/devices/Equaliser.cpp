#include "devices/Equaliser.h"

#include <cmath>

namespace dojo::devices
{
namespace
{
constexpr int kSubBlock = 32;

const juce::StringArray& shapeNames()
{
    static const juce::StringArray names{"Low Cut", "Low Shelf", "Peak", "High Shelf", "High Cut"};
    return names;
}

struct BandDefaults
{
    int shape;
    float frequency;
};
constexpr std::array<BandDefaults, Equaliser::kNumBands> kDefaults{{{1, 100.0f}, {2, 500.0f}, {2, 2500.0f}, {3, 8000.0f}}};
} // namespace

Equaliser::Equaliser()
    : BuiltinProcessor(kTypeId, "EQ",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto prefix = "band" + juce::String(b + 1);
        const auto label = "Band " + juce::String(b + 1) + " ";
        auto& band = bands_[static_cast<std::size_t>(b)];
        band.enabled = addBool(prefix + "On", label + "On", true);
        band.shape = addChoice(prefix + "Shape", label + "Shape", shapeNames(), kDefaults[static_cast<std::size_t>(b)].shape);
        band.frequency = addFloat(prefix + "Freq", label + "Frequency", skewedRange(20.0f, 20000.0f, 1000.0f),
                                  kDefaults[static_cast<std::size_t>(b)].frequency, "Hz");
        band.gain = addFloat(prefix + "Gain", label + "Gain", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
        band.q = addFloat(prefix + "Q", label + "Q", skewedRange(0.1f, 18.0f, 1.0f), 0.71f);
    }

    output_ = addFloat("output", "Output", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");

    for (auto& value : analyser_)
        value.store(0.0f, std::memory_order_relaxed);
}

dsp::BiquadCoefficients::Shape Equaliser::shapeOf(int index) noexcept
{
    using Shape = dsp::BiquadCoefficients::Shape;
    switch (index)
    {
    case 0: return Shape::lowCut;
    case 1: return Shape::lowShelf;
    case 3: return Shape::highShelf;
    case 4: return Shape::highCut;
    default: return Shape::peak;
    }
}

void Equaliser::prepareDsp(double sampleRate, int)
{
    // Per-sub-block smoothing: ~20 ms to settle.
    smoothing_ = std::exp(-static_cast<double>(kSubBlock) / (0.02 * sampleRate));
    for (auto& band : state_)
    {
        band.initialised = false;
        for (auto& f : band.filters)
            f.reset();
    }
    updateCoefficients(true);
}

void Equaliser::updateCoefficients(bool snap) noexcept
{
    for (int b = 0; b < kNumBands; ++b)
    {
        const auto& p = bands_[static_cast<std::size_t>(b)];
        auto& s = state_[static_cast<std::size_t>(b)];

        const double targetFrequency = p.frequency->get();
        const double targetGain = p.gain->get();
        const double targetQ = p.q->get();
        const int shape = p.shape->getIndex();
        const bool enabled = p.enabled->get();

        if (snap || !s.initialised || shape != s.shape)
        {
            s.frequency = targetFrequency;
            s.gain = targetGain;
            s.q = targetQ;
            s.initialised = true;
        }
        else
        {
            const double k = smoothing_;
            s.frequency = std::exp(std::log(targetFrequency) + (std::log(s.frequency) - std::log(targetFrequency)) * k);
            s.gain = targetGain + (s.gain - targetGain) * k;
            s.q = targetQ + (s.q - targetQ) * k;
        }

        if (shape != s.shape || enabled != s.enabled)
            for (auto& f : s.filters)
                f.reset();

        s.shape = shape;
        s.enabled = enabled;
        s.coefficients = dsp::BiquadCoefficients::make(shapeOf(shape), sampleRate_, s.frequency, s.q, s.gain);
    }

    const double targetOutput = dsp::dbToGain(output_->get());
    outputGain_ = snap ? targetOutput : targetOutput + (outputGain_ - targetOutput) * smoothing_;
}

void Equaliser::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer(1) : left;
    std::uint32_t write = analyserWrite_.load(std::memory_order_relaxed);

    for (int start = 0; start < numSamples; start += kSubBlock)
    {
        updateCoefficients(false);
        const int end = std::min(numSamples, start + kSubBlock);

        for (int i = start; i < end; ++i)
        {
            double l = left[i];
            double r = right[i];
            for (auto& band : state_)
            {
                if (!band.enabled)
                    continue;
                l = band.filters[0].process(l, band.coefficients);
                r = band.filters[1].process(r, band.coefficients);
            }
            l *= outputGain_;
            r *= outputGain_;
            left[i] = l;
            if (right != left)
                right[i] = r;

            analyser_[write % kAnalyserSize].store(static_cast<float>(0.5 * (l + r)), std::memory_order_relaxed);
            ++write;
        }
    }

    analyserWrite_.store(write, std::memory_order_release);
}

double Equaliser::getBandMagnitudeAt(int band, double frequency) const
{
    const auto& p = bands_[static_cast<std::size_t>(band)];
    if (!p.enabled->get())
        return 1.0;
    const auto c = dsp::BiquadCoefficients::make(shapeOf(p.shape->getIndex()), sampleRate_, p.frequency->get(), p.q->get(),
                                                 p.gain->get());
    return c.magnitudeAt(frequency, sampleRate_);
}

double Equaliser::getMagnitudeAt(double frequency) const
{
    double magnitude = dsp::dbToGain(output_->get());
    for (int b = 0; b < kNumBands; ++b)
        magnitude *= getBandMagnitudeAt(b, frequency);
    return magnitude;
}

void Equaliser::copyAnalyserSamples(float* destination, int count) const noexcept
{
    count = std::min(count, kAnalyserSize);
    const std::uint32_t write = analyserWrite_.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t index = write - static_cast<std::uint32_t>(count) + static_cast<std::uint32_t>(i);
        destination[i] = analyser_[index % kAnalyserSize].load(std::memory_order_relaxed);
    }
}

} // namespace dojo::devices

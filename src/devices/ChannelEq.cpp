#include "devices/ChannelEq.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

ChannelEq::ChannelEq()
    : BuiltinProcessor(kTypeId, "Channel EQ",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    lowCut_ = addBool("lowcut", "Low Cut", false);
    low_ = addFloat("low", "Low", {-15.0f, 15.0f, 0.1f}, 0.0f, "dB");
    midFrequency_ = addFloat("midfreq", "Mid Freq", skewedRange(120.0f, 7500.0f, 1000.0f), 1000.0f, "Hz");
    mid_ = addFloat("mid", "Mid", {-12.0f, 12.0f, 0.1f}, 0.0f, "dB");
    high_ = addFloat("high", "High", {-15.0f, 15.0f, 0.1f}, 0.0f, "dB");
    output_ = addFloat("output", "Output", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
}

double ChannelEq::midQ(double gainDb) noexcept
{
    return 0.5 + std::abs(gainDb) / 12.0 * 1.3; // 0.5 (gentle) .. 1.8 (at +-12 dB)
}

ChannelEq::Coefficients ChannelEq::makeCoefficients(double sampleRate) const noexcept
{
    using Shape = dsp::BiquadCoefficients::Shape;
    Coefficients c;
    c.lowCut = dsp::BiquadCoefficients::make(Shape::lowCut, sampleRate, kLowCutHz, 0.7071, 0.0);
    c.low = dsp::BiquadCoefficients::make(Shape::lowShelf, sampleRate, kLowShelfHz, 0.6, low_->get());
    c.mid = dsp::BiquadCoefficients::make(Shape::peak, sampleRate, midFrequency_->get(), midQ(mid_->get()), mid_->get());
    c.high = dsp::BiquadCoefficients::make(Shape::highShelf, sampleRate, kHighShelfHz, 0.6, high_->get());
    return c;
}

double ChannelEq::getResponseDb(double frequency) const noexcept
{
    const auto c = makeCoefficients(sampleRate_);
    double db = dsp::gainToDb(c.low.magnitudeAt(frequency, sampleRate_) * c.mid.magnitudeAt(frequency, sampleRate_) *
                              c.high.magnitudeAt(frequency, sampleRate_));
    if (lowCut_->get())
        db += dsp::gainToDb(c.lowCut.magnitudeAt(frequency, sampleRate_));
    return db + output_->get();
}

void ChannelEq::prepareDsp(double, int)
{
    for (auto& channel : filters_)
        for (auto& f : channel)
            f.reset();
}

void ChannelEq::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const auto c = makeCoefficients(sampleRate_);
    const bool lowCut = lowCut_->get();
    const double gain = dsp::dbToGain(output_->get());
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& f = filters_[static_cast<std::size_t>(ch)];
        double* x = buffer.getWritePointer(ch);
        for (int i = 0; i < numSamples; ++i)
        {
            double y = x[i];
            if (lowCut)
                y = f[0].process(y, c.lowCut);
            y = f[1].process(y, c.low); // (at 0 dB the bands are transparent)
            y = f[2].process(y, c.mid);
            y = f[3].process(y, c.high);
            x[i] = y * gain;
        }
    }
}

} // namespace dojo::devices

#include "devices/Drive.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
//==================================================================================================
// Pedal

Pedal::Pedal()
    : BuiltinProcessor(kTypeId, "Pedal", BusesProperties()
                                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput("Output", juce::AudioChannelSet::stereo(), true), false)
{
    type_ = addChoice("type", "Type", {"Overdrive", "Distortion", "Fuzz"}, 0);
    gain_ = addFloat("gain", "Gain", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    output_ = addFloat("output", "Output", {-36.0f, 12.0f, 0.1f}, 0.0f, "dB");
    bass_ = addFloat("bass", "Bass", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    mid_ = addFloat("mid", "Mid", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    midHigh_ = addBool("midhigh", "Mid Freq High", false);
    treble_ = addFloat("treble", "Treble", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    sub_ = addBool("sub", "Sub", false);
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

double Pedal::shape(Type type, double x) noexcept
{
    switch (type)
    {
    case Type::overdrive: return std::tanh(x);
    case Type::distortion: return x / std::pow(1.0 + std::pow(std::abs(x), 4.0), 0.25);
    case Type::fuzz:
    {
        constexpr double bias = 0.35;
        constexpr double rest = 0.33637554433633; // tanh(bias)
        return std::tanh(2.0 * (std::tanh(x + bias) - rest));
    }
    }
    return x;
}

void Pedal::prepareDsp(double, int maximumBlockSize)
{
    oversampler_.prepare(2, maximumBlockSize);
    setLatencySamples(oversampler_.getLatency());
    for (auto* filters : {&preHighPass_, &cleanBlend_, &postLowPass_, &postLowPass2_})
        for (auto& f : *filters)
            f.reset();
    for (auto& channel : eq_)
        for (auto& b : channel)
            b.reset();
    fuzzEnvelope_.fill(0.0);
    for (auto& d : dc_)
    {
        d.prepare(sampleRate_);
        d.reset();
    }
}

void Pedal::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const auto type = static_cast<Type>(std::clamp(type_->getIndex(), 0, 2));
    const double gain = gain_->get() / 100.0;
    const double oversampledRate = sampleRate_ * oversampler_.getFactor();

    double driveDb = 0.0, preFrequency = 720.0, postFrequency = 6000.0, level = 0.6;
    switch (type)
    {
    case Type::overdrive: driveDb = gain * 36.0; preFrequency = 720.0; postFrequency = 6000.0; level = 0.55; break;
    case Type::distortion: driveDb = 6.0 + gain * 42.0; preFrequency = 90.0; postFrequency = 5000.0; level = 0.45; break;
    case Type::fuzz: driveDb = 18.0 + gain * 42.0; preFrequency = 40.0; postFrequency = 4000.0; level = 0.4; break;
    }
    const double drive = dsp::dbToGain(driveDb);
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        preHighPass_[ch].setFrequency(preFrequency, oversampledRate);
        postLowPass_[ch].setFrequency(postFrequency, oversampledRate);
        postLowPass2_[ch].setFrequency(postFrequency * 1.5, oversampledRate);
    }

    const double envelopeAttack = dsp::onePoleCoefficient(0.001, oversampledRate);
    const double envelopeRelease = dsp::onePoleCoefficient(0.05, oversampledRate);

    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    oversampler_.process(channels, numSamples,
                         [&](double* data, std::size_t count, int channel)
                         {
                             auto& hp = preHighPass_[static_cast<std::size_t>(channel)];
                             auto& envelope = fuzzEnvelope_[static_cast<std::size_t>(channel)];
                             auto& lp = postLowPass_[static_cast<std::size_t>(channel)];
                             auto& lp2 = postLowPass2_[static_cast<std::size_t>(channel)];
                             for (std::size_t j = 0; j < count; ++j)
                             {
                                 const double x = data[j];
                                 const double filtered = hp.highPass(x);
                                 double driven = drive * filtered;
                                 if (type == Type::fuzz)
                                 {
                                     // The transistor's bias moves with the signal: the clipping
                                     // point sits off-centre at every level (uneven duty cycle, the
                                     // fuzz's even harmonics and sputter as notes decay).
                                     const double magnitude = std::abs(filtered);
                                     envelope = magnitude + (envelope - magnitude) * (magnitude > envelope ? envelopeAttack : envelopeRelease);
                                     driven += drive * kFuzzBias * envelope;
                                 }
                                 double y = shape(type, driven);
                                 if (type == Type::overdrive)
                                     y = 0.5 * x + 0.7 * y; // the clean path of the classic circuit
                                 y = lp.lowPass(y);
                                 if (type == Type::distortion)
                                     y = lp2.lowPass(y);
                                 data[j] = y;
                             }
                         });

    // Tone section at the normal rate.
    using Shape = dsp::BiquadCoefficients::Shape;
    const auto bass = dsp::BiquadCoefficients::make(Shape::lowShelf, sampleRate_, 120.0, 0.7, bass_->get() * 0.12);
    const auto mid = dsp::BiquadCoefficients::make(Shape::peak, sampleRate_, midHigh_->get() ? 1500.0 : 500.0, 0.8, mid_->get() * 0.12);
    const auto treble = dsp::BiquadCoefficients::make(Shape::highShelf, sampleRate_, 3500.0, 0.7, treble_->get() * 0.12);
    const auto sub = dsp::BiquadCoefficients::make(Shape::lowShelf, sampleRate_, 250.0, 0.7, sub_->get() ? 6.0 : 0.0);
    const double outputGain = dsp::dbToGain(output_->get()) * level;
    const double wet = mix_->get() / 100.0;
    for (std::size_t ch = 0; ch < 2; ++ch)
    {
        auto& f = eq_[ch];
        double* x = channels[ch];
        for (int i = 0; i < numSamples; ++i)
        {
            double y = f[0].process(x[i], bass);
            y = f[1].process(y, mid);
            y = f[2].process(y, treble);
            y = f[3].process(y, sub);
            y = dc_[ch].process(y) * outputGain;
            x[i] = oversampler_.dry(static_cast<int>(ch), i) * (1.0 - wet) + y * wet;
        }
    }
}

//==================================================================================================
// Overdrive

Overdrive::Overdrive()
    : BuiltinProcessor(kTypeId, "Overdrive", BusesProperties()
                                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput("Output", juce::AudioChannelSet::stereo(), true), false)
{
    frequency_ = addFloat("freq", "Frequency", skewedRange(50.0f, 20000.0f, 1000.0f), 1100.0f, "Hz");
    width_ = addFloat("width", "Width", {0.5f, 9.0f, 0.01f}, 6.5f, "oct");
    drive_ = addFloat("drive", "Drive", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    tone_ = addFloat("tone", "Tone", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    dynamics_ = addFloat("dynamics", "Dynamics", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    output_ = addFloat("output", "Output", {-24.0f, 12.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

void Overdrive::prepareDsp(double, int maximumBlockSize)
{
    oversampler_.prepare(2, maximumBlockSize);
    setLatencySamples(oversampler_.getLatency());
    for (auto* filters : {&bandLow_, &bandHigh_})
        for (auto& f : *filters)
            f.reset();
    for (auto& f : toneFilter_)
        f.reset();
    for (auto& d : dc_)
    {
        d.prepare(sampleRate_);
        d.reset();
    }
    fast_.fill(0.0);
    slow_.fill(0.0);
}

void Overdrive::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const double rate = sampleRate_ * oversampler_.getFactor();
    const double centre = frequency_->get();
    const double half = std::exp2(width_->get() * 0.5);
    const double lowCut = std::clamp(centre / half, 10.0, 0.2 * rate);
    const double highCut = std::clamp(centre * half, 40.0, std::min(20000.0, 0.45 * sampleRate_));
    using Shape = dsp::BiquadCoefficients::Shape;
    const auto low = dsp::BiquadCoefficients::make(Shape::lowCut, rate, lowCut, 0.7071, 0.0);
    const auto high = dsp::BiquadCoefficients::make(Shape::highCut, rate, highCut, 0.7071, 0.0);
    const double drive = dsp::dbToGain(drive_->get() * 0.4); // 0 .. 40 dB
    const double dynamics = dynamics_->get() / 100.0;
    const double toneFrequency = 800.0 * std::exp2(tone_->get() / 100.0 * 4.0); // 800 Hz .. 12.8 kHz
    const double fastAttack = dsp::onePoleCoefficient(0.001, rate), fastRelease = dsp::onePoleCoefficient(0.06, rate);
    const double slowCoefficient = dsp::onePoleCoefficient(0.4, rate);
    for (auto& f : toneFilter_)
        f.setFrequency(toneFrequency, rate);

    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    oversampler_.process(channels, numSamples,
                         [&](double* data, std::size_t count, int channel)
                         {
                             const auto ch = static_cast<std::size_t>(channel);
                             for (std::size_t j = 0; j < count; ++j)
                             {
                                 const double band = bandHigh_[ch].process(bandLow_[ch].process(data[j], low), high);
                                 // Dynamics: the input's short-term level over its longer average,
                                 // brought back after the clipping flattened it.
                                 const double level = std::abs(band);
                                 fast_[ch] = level + (fast_[ch] - level) * (level > fast_[ch] ? fastAttack : fastRelease);
                                 slow_[ch] = fast_[ch] + (slow_[ch] - fast_[ch]) * slowCoefficient;
                                 const double ratio = (fast_[ch] + 1.0e-5) / (slow_[ch] + 1.0e-5);
                                 const double restore = std::clamp(std::pow(ratio, dynamics), 0.25, 4.0);
                                 const double clipped = std::tanh(drive * band);
                                 data[j] = toneFilter_[ch].lowPass(clipped * restore);
                             }
                         });

    const double outputGain = dsp::dbToGain(output_->get()) * 0.6;
    const double wet = mix_->get() / 100.0;
    for (std::size_t ch = 0; ch < 2; ++ch)
        for (int i = 0; i < numSamples; ++i)
        {
            const double y = dc_[ch].process(channels[ch][i]) * outputGain;
            channels[ch][i] = oversampler_.dry(static_cast<int>(ch), i) * (1.0 - wet) + y * wet;
        }
}

//==================================================================================================
// Redux

Redux::Redux()
    : BuiltinProcessor(kTypeId, "Redux", BusesProperties()
                                     .withInput("Input", juce::AudioChannelSet::stereo(), true)
                                     .withOutput("Output", juce::AudioChannelSet::stereo(), true), false)
{
    rate_ = addFloat("rate", "Rate", skewedRange(40.0f, 48000.0f, 4000.0f), 8000.0f, "Hz");
    jitter_ = addFloat("jitter", "Jitter", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    preFilter_ = addBool("prefilter", "Pre-Filter", true);
    postFilter_ = addBool("postfilter", "Post-Filter", false);
    postOctave_ = addFloat("postoct", "Post Octave", {-4.0f, 4.0f, 0.01f}, 0.0f, "oct");
    bits_ = addFloat("bits", "Bits", {1.0f, 16.0f, 0.01f}, 8.0f, "bit", true, 2);
    shape_ = addFloat("shape", "Shape", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    dcShift_ = addBool("dcshift", "DC Shift", false);
    output_ = addFloat("output", "Output", {-24.0f, 12.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

double Redux::quantise(double x, double bits, double shape, bool dcShift) noexcept
{
    const double levels = std::exp2(std::clamp(bits, 1.0, 24.0) - 1.0);
    const double exponent = 1.0 / (1.0 + 3.0 * std::clamp(shape, 0.0, 1.0));
    const double companded = std::copysign(std::pow(std::min(1.0, std::abs(x)), exponent), x);
    const double scaled = companded * levels;
    const double stepped = (dcShift ? std::floor(scaled) : std::round(scaled)) / levels;
    return std::copysign(std::pow(std::abs(stepped), 1.0 / exponent), stepped);
}

void Redux::prepareDsp(double, int)
{
    for (auto* filters : {&pre_, &post_})
        for (auto& channel : *filters)
            for (auto& b : channel)
                b.reset();
    held_.fill(0.0);
    phase_ = 1.0;
    period_ = 1.0;
}

void Redux::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const double rate = std::min(static_cast<double>(rate_->get()), sampleRate_);
    const bool downsampling = rate < sampleRate_ * 0.999;
    const double step = rate / sampleRate_;
    const double jitter = jitter_->get() / 100.0 * 0.9;
    const bool pre = preFilter_->get() && downsampling;
    const bool post = postFilter_->get();
    const double bits = bits_->get();
    const double shape = shape_->get() / 100.0;
    const bool dcShift = dcShift_->get();
    const double outputGain = dsp::dbToGain(output_->get());
    const double wet = mix_->get() / 100.0;

    // Eighth-order Butterworth low-passes (four biquads).
    using Shape = dsp::BiquadCoefficients::Shape;
    constexpr std::array<double, 4> q{0.5098, 0.6013, 0.9000, 2.5629};
    const double nyquist = sampleRate_ * 0.45;
    const double preCut = std::clamp(rate * 0.45, 20.0, nyquist);
    const double postCut = std::clamp(rate * 0.5 * std::exp2(postOctave_->get()), 20.0, nyquist);
    std::array<dsp::BiquadCoefficients, 4> preCoefficients, postCoefficients;
    for (std::size_t k = 0; k < q.size(); ++k)
    {
        preCoefficients[k] = dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, preCut, q[k], 0.0);
        postCoefficients[k] = dsp::BiquadCoefficients::make(Shape::highCut, sampleRate_, postCut, q[k], 0.0);
    }

    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    for (int i = 0; i < numSamples; ++i)
    {
        std::array<double, 2> x{channels[0][i], channels[1][i]};
        if (pre)
            for (std::size_t ch = 0; ch < 2; ++ch)
                for (std::size_t k = 0; k < q.size(); ++k)
                    x[ch] = pre_[ch][k].process(x[ch], preCoefficients[k]);

        // Sample and hold on the slower (jittering) clock.
        if (downsampling)
        {
            phase_ += step;
            if (phase_ >= period_)
            {
                phase_ -= period_;
                held_ = x;
                period_ = 1.0 + jitter * noise_.next();
            }
        }
        else
        {
            held_ = x;
        }

        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            double y = quantise(held_[ch], bits, shape, dcShift);
            if (post)
                for (std::size_t k = 0; k < q.size(); ++k)
                    y = post_[ch][k].process(y, postCoefficients[k]);
            const double dry = channels[ch][i];
            channels[ch][i] = dry * (1.0 - wet) + y * outputGain * wet;
        }
    }
}

} // namespace dojo::devices

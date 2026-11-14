#include "devices/Roar.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

const juce::StringArray& Roar::getShapeNames()
{
    static const juce::StringArray names{"Soft Sine", "Diode Clipper", "Tube Preamp", "Polynomial", "Hard Clip", "Rectifier",
                                         "Tri Fold",  "Sine Fold",     "Bit Crusher", "Fractal",    "Noise Injection"};
    return names;
}

const juce::StringArray& Roar::getSourceNames()
{
    static const juce::StringArray names{"Off", "LFO 1", "LFO 2", "Envelope", "Noise"};
    return names;
}

const juce::StringArray& Roar::getDestinationNames()
{
    static const juce::StringArray names{"Off",          "Drive",        "Tone",          "Stage 1 Amount", "Stage 1 Bias",
                                         "Stage 1 Freq", "Stage 2 Amount", "Stage 2 Bias", "Stage 2 Freq",   "Stage 3 Amount",
                                         "Stage 3 Bias", "Stage 3 Freq", "Feedback",      "Feedback Freq",  "Dry/Wet"};
    return names;
}

double Roar::shape(Shape shape, double x, double noise) noexcept
{
    switch (shape)
    {
    case Shape::softSine: return std::sin(std::clamp(x, -dsp::kPi * 0.5, dsp::kPi * 0.5));
    case Shape::diodeClipper: return x >= 0.0 ? 1.0 - std::exp(-x) : -0.6 * (1.0 - std::exp(1.6 * x));
    case Shape::tubePreamp: return x >= 0.0 ? x / (1.0 + 0.6 * x) : std::tanh(1.4 * x) / 1.4;
    case Shape::polynomial:
    {
        const double c = std::clamp(x, -1.0, 1.0);
        return 1.5 * c - 0.5 * c * c * c + 0.3 * c * c;
    }
    case Shape::hardClip: return std::clamp(x, -1.0, 1.0);
    case Shape::rectifier: return std::abs(std::tanh(x));
    case Shape::triFold:
    {
        double f = x * 0.25 + 0.25;
        f -= std::floor(f);
        return f < 0.5 ? 4.0 * f - 1.0 : 3.0 - 4.0 * f;
    }
    case Shape::sineFold: return std::sin(x);
    case Shape::bitCrusher: return std::round(std::tanh(x) * 12.0) / 12.0;
    case Shape::fractal: return (std::sin(x) + 0.5 * std::sin(2.0 * x) + 0.25 * std::sin(4.0 * x) + 0.125 * std::sin(8.0 * x)) / 1.875;
    case Shape::noiseInjection:
    {
        const double t = std::tanh(x);
        return t + noise * std::abs(t) * 0.6;
    }
    }
    return x;
}

Roar::Roar()
    : BuiltinProcessor(kTypeId, "Roar",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    drive_ = addFloat("drive", "Drive", {-24.0f, 36.0f, 0.1f}, 6.0f, "dB");
    tone_ = addFloat("tone", "Tone", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    routing_ = addChoice("routing", "Routing", {"Single", "Serial", "Parallel", "Mid/Side", "Multiband"}, 0);
    crossLow_ = addFloat("xlow", "Low/Mid", skewedRange(50.0f, 1000.0f, 200.0f), 200.0f, "Hz");
    crossHigh_ = addFloat("xhigh", "Mid/High", skewedRange(1000.0f, 12000.0f, 3000.0f), 3000.0f, "Hz");

    constexpr std::array<int, kStages> defaultShapes{0, 2, 1};
    for (std::size_t s = 0; s < stages_.size(); ++s)
    {
        const juce::String id = "s" + juce::String(s + 1);
        const juce::String name = "Stage " + juce::String(s + 1) + " ";
        auto& stage = stages_[s];
        stage.on = addBool(id + "on", name + "On", true);
        stage.shape = addChoice(id + "shape", name + "Shape", getShapeNames(), defaultShapes[s]);
        stage.amount = addFloat(id + "amount", name + "Amount", {0.0f, 100.0f, 0.1f}, 30.0f, "%");
        stage.bias = addFloat(id + "bias", name + "Bias", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
        stage.filter = addChoice(id + "filter", name + "Filter", {"Off", "Low Pass", "High Pass", "Band Pass", "Notch", "Peak"}, 0);
        stage.filterPre = addBool(id + "pre", name + "Filter Pre", false);
        stage.frequency = addFloat(id + "freq", name + "Frequency", skewedRange(20.0f, 20000.0f, 1000.0f), 2000.0f, "Hz");
        stage.resonance = addFloat(id + "res", name + "Resonance", {0.0f, 100.0f, 0.1f}, 20.0f, "%");
        stage.level = addFloat(id + "level", name + "Level", {-36.0f, 12.0f, 0.1f}, 0.0f, "dB");
    }

    for (std::size_t l = 0; l < lfos_.size(); ++l)
    {
        const juce::String id = "l" + juce::String(l + 1);
        const juce::String name = "LFO " + juce::String(l + 1) + " ";
        lfos_[l].shape = addChoice(id + "shape", name + "Shape", dsp::waveformNames(), l == 0 ? 0 : 1);
        lfos_[l].rate = addFloat(id + "rate", name + "Rate", skewedRange(0.01f, 40.0f, 2.0f), l == 0 ? 1.0f : 0.25f, "Hz");
        lfos_[l].sync = addBool(id + "sync", name + "Sync", false);
        lfos_[l].division = addChoice(id + "div", name + "Division", dsp::syncDivisionNames(), 9);
    }
    envAttack_ = addFloat("envattack", "Env Attack", skewedRange(0.1f, 100.0f, 10.0f), 5.0f, "ms");
    envRelease_ = addFloat("envrelease", "Env Release", skewedRange(10.0f, 2000.0f, 200.0f), 150.0f, "ms");
    envGain_ = addFloat("envgain", "Env Gain", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
    noiseRate_ = addFloat("noiserate", "Noise Rate", skewedRange(0.1f, 50.0f, 4.0f), 4.0f, "Hz");
    noiseSmooth_ = addFloat("noisesmooth", "Noise Smooth", {0.0f, 100.0f, 0.1f}, 50.0f, "%");

    for (std::size_t m = 0; m < matrix_.size(); ++m)
    {
        const juce::String id = "m" + juce::String(m + 1);
        const juce::String name = "Mod " + juce::String(m + 1) + " ";
        matrix_[m].source = addChoice(id + "src", name + "Source", getSourceNames(), 0);
        matrix_[m].destination = addChoice(id + "dst", name + "Target", getDestinationNames(), 0);
        matrix_[m].amount = addFloat(id + "amt", name + "Amount", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    }

    feedback_ = addFloat("fbamount", "Feedback", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    feedbackTime_ = addFloat("fbtime", "Feedback Time", skewedRange(1.0f, 1000.0f, 100.0f), 120.0f, "ms");
    feedbackSync_ = addBool("fbsync", "Feedback Sync", false);
    feedbackDivision_ = addChoice("fbdiv", "Feedback Division", dsp::syncDivisionNames(), 6);
    feedbackFrequency_ = addFloat("fbfreq", "Feedback Freq", skewedRange(100.0f, 10000.0f, 1000.0f), 1000.0f, "Hz");
    feedbackInvert_ = addBool("fbinvert", "Feedback Invert", false);
    compressor_ = addFloat("comp", "Compressor", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    output_ = addFloat("output", "Output", {-36.0f, 12.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

void Roar::prepareDsp(double sampleRate, int maximumBlockSize)
{
    oversampler_.prepare(1, std::max(maximumBlockSize, kChunk));
    setLatencySamples(oversampler_.getLatency());
    for (auto& d : feedbackLine_)
        d.setMaximum(static_cast<int>(kMaxFeedbackSeconds * sampleRate) + kChunk);
    for (auto& stage : stageFilters_)
        for (auto& f : stage)
            f.reset();
    for (auto& channel : tilt_)
        for (auto& b : channel)
            b.reset();
    for (auto* filters : {&lowSplit_, &highSplit_, &lowAlign_})
        for (auto& f : *filters)
            f.reset();
    for (auto& f : feedbackFilter_)
        f.reset();
    for (auto& d : dc_)
    {
        d.prepare(sampleRate);
        d.reset();
    }
    for (auto& l : lfo_)
        l.reset();
    follower_.reset();
    noiseTarget_ = noiseValue_ = noisePhase_ = 0.0;
    compressorEnvelope_ = compressorGainDb_ = 0.0;
}

double Roar::runStage(std::size_t stage, std::size_t channel, double x) noexcept
{
    const auto& s = state_[stage];
    if (!s.on)
        return x;
    auto& filter = stageFilters_[stage][channel];
    const auto filtered = [&](double v)
    {
        const auto o = filter.process(v);
        switch (s.filter)
        {
        case 1: return o.lowPass;
        case 2: return o.highPass;
        case 3: return o.bandPass;
        case 4: return o.notch;
        case 5: return v + 2.0 * o.bandPass; // peak: about +9.5 dB at the frequency
        default: return v;
        }
    };
    double y = x;
    if (s.filter > 0 && s.pre)
        y = filtered(y);
    y = (shape(s.shape, y * s.gain + s.bias, shapeNoise_.next()) - s.rest) * s.makeup;
    if (s.filter > 0 && !s.pre)
        y = filtered(y);
    return y * s.level;
}

void Roar::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const auto transport = dsp::readTransport(getPlayHead());
    const double oversampledRate = sampleRate_ * oversampler_.getFactor();
    const auto routing = static_cast<Routing>(std::clamp(routing_->getIndex(), 0, 4));
    const double filterCeiling = std::min(20000.0, oversampledRate * 0.45);
    follower_.setTimes(envAttack_->get() / 1000.0, envRelease_->get() / 1000.0, sampleRate_);
    const double envelopeGain = dsp::dbToGain(envGain_->get());
    const double noiseSmoothing = 1.0 - 0.98 * noiseSmooth_->get() / 100.0;
    const double outputGain = dsp::dbToGain(output_->get());
    const double compressorAmount = compressor_->get() / 100.0;
    const double compressorThreshold = -30.0 * compressorAmount;
    const double compressorDetector = dsp::onePoleCoefficient(0.01, sampleRate_);
    const double compressorAttack = dsp::onePoleCoefficient(0.005, sampleRate_);
    const double compressorRelease = dsp::onePoleCoefficient(0.15, sampleRate_);
    double maxCompression = 0.0;

    // Multiband crossovers (at the oversampled rate).
    if (routing == Routing::multiband)
    {
        const double low = crossLow_->get();
        const double high = std::max(low * 1.5, static_cast<double>(crossHigh_->get()));
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            lowSplit_[ch].setFrequency(low, oversampledRate);
            highSplit_[ch].setFrequency(high, oversampledRate);
            lowAlign_[ch].setFrequency(high, oversampledRate);
        }
    }

    double* io[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    for (int start = 0; start < numSamples; start += kChunk)
    {
        const int n = std::min(kChunk, numSamples - start);
        double* channels[2] = {io[0] + start, io[1] + start};

        // --- Modulation sources -------------------------------------------------------------------
        for (int i = 0; i < n; ++i)
            follower_.process(std::max(std::abs(channels[0][i]), std::abs(channels[1][i])) * envelopeGain);
        std::array<double, numSources> sources{};
        sources[srcEnvelope] = dsp::EnvelopeFollower::normalise(follower_.getLevel());
        const double ppqAtEnd = transport.ppq + (start + n) / sampleRate_ * transport.bpm / 60.0;
        for (std::size_t l = 0; l < 2; ++l)
        {
            lfo_[l].advance(n, sampleRate_, lfos_[l].rate->get(), lfos_[l].sync->get(), dsp::syncDivisionBeats(lfos_[l].division->getIndex()),
                            transport, ppqAtEnd);
            sources[l == 0 ? srcLfo1 : srcLfo2] = lfo_[l].value(static_cast<dsp::Waveform>(std::clamp(lfos_[l].shape->getIndex(), 0, 7)), 0.0,
                                                                static_cast<std::uint32_t>(l + 1));
        }
        noisePhase_ += n * static_cast<double>(noiseRate_->get()) / sampleRate_;
        if (noisePhase_ >= 1.0)
        {
            noisePhase_ -= std::floor(noisePhase_);
            noiseTarget_ = noise_.next();
        }
        noiseValue_ += (noiseTarget_ - noiseValue_) * noiseSmoothing;
        sources[srcNoise] = noiseValue_;

        std::array<double, numDestinations> mod{};
        for (const auto& slot : matrix_)
        {
            const int source = slot.source->getIndex();
            const int destination = slot.destination->getIndex();
            if (source > srcOff && source < numSources && destination > dstOff && destination < numDestinations)
                mod[static_cast<std::size_t>(destination)] += slot.amount->get() / 100.0 * sources[static_cast<std::size_t>(source)];
        }
        for (int s = srcLfo1; s < numSources; ++s)
            sourceValues_[static_cast<std::size_t>(s)].store(static_cast<float>(sources[static_cast<std::size_t>(s)]), std::memory_order_relaxed);

        // --- Settings after modulation ----------------------------------------------------------
        const double driveGain = dsp::dbToGain(drive_->get() + mod[dstDrive] * 24.0);
        const double tilt = std::clamp(tone_->get() / 100.0 + mod[dstTone], -1.0, 1.0) * 6.0;
        using Coefficients = dsp::BiquadCoefficients;
        const auto lowShelf = Coefficients::make(Coefficients::Shape::lowShelf, sampleRate_, 800.0, 0.5, -tilt);
        const auto highShelf = Coefficients::make(Coefficients::Shape::highShelf, sampleRate_, 2000.0, 0.5, tilt);
        for (std::size_t s = 0; s < stages_.size(); ++s)
        {
            const auto& p = stages_[s];
            auto& st = state_[s];
            const auto base = static_cast<std::size_t>(dstStage1Amount) + s * 3;
            st.on = p.on->get();
            st.shape = static_cast<Shape>(std::clamp(p.shape->getIndex(), 0, getShapeNames().size() - 1));
            const double amount = std::clamp(p.amount->get() / 100.0 + mod[base], 0.0, 1.0);
            st.gain = dsp::dbToGain(amount * 36.0);
            st.makeup = 1.0 / std::sqrt(st.gain);
            st.bias = std::clamp(p.bias->get() / 100.0 + mod[base + 1], -1.0, 1.0) * 0.5;
            st.rest = shape(st.shape, st.bias, 0.0);
            st.level = dsp::dbToGain(p.level->get());
            st.filter = p.filter->getIndex();
            st.pre = p.filterPre->get();
            if (st.filter > 0)
            {
                const double frequency = std::clamp(p.frequency->get() * std::exp2(mod[base + 2] * 4.0), 20.0, filterCeiling);
                for (auto& f : stageFilters_[s])
                    f.set(frequency, p.resonance->get() / 100.0 * 0.95, oversampledRate, dsp::FilterCircuit::clean);
            }
        }
        const double feedbackAmount = std::clamp(feedback_->get() / 100.0 + mod[dstFeedback], 0.0, 1.0);
        const double feedback = feedbackAmount * 0.95 * (feedbackInvert_->get() ? -1.0 : 1.0);
        const double feedbackFrequency = std::clamp(feedbackFrequency_->get() * std::exp2(mod[dstFeedbackFrequency] * 3.0), 50.0, 15000.0);
        for (auto& f : feedbackFilter_)
            f.set(feedbackFrequency, 0.35, sampleRate_, dsp::FilterCircuit::warm);
        const double feedbackDelay =
            std::clamp((feedbackSync_->get() ? dsp::syncDivisionBeats(feedbackDivision_->getIndex()) * 60.0 / transport.bpm
                                             : feedbackTime_->get() / 1000.0) * sampleRate_,
                       1.0, static_cast<double>(feedbackLine_[0].capacity() - kChunk));
        const double wet = std::clamp(mix_->get() / 100.0 + mod[dstMix], 0.0, 1.0);

        // --- Input: drive, tone, feedback (the original kept for dry/wet) -------------------------
        std::array<std::array<double, kChunk>, 2> original{};
        for (std::size_t ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
            {
                original[ch][static_cast<std::size_t>(i)] = channels[ch][i];
                double x = channels[ch][i] * driveGain;
                x = tilt_[ch][1].process(tilt_[ch][0].process(x, lowShelf), highShelf);
                if (feedbackAmount > 0.0)
                {
                    // The newest output in the line is the previous chunk's last sample.
                    const double delayed = feedbackLine_[ch].read(std::max(0.0, feedbackDelay - i - 1.0));
                    x += dsp::softSaturate(feedbackFilter_[ch].process(delayed).bandPass) * feedback;
                }
                channels[ch][i] = x;
            }
        if (routing == Routing::midSide)
            for (int i = 0; i < n; ++i)
            {
                const double mid = 0.5 * (channels[0][i] + channels[1][i]);
                const double side = 0.5 * (channels[0][i] - channels[1][i]);
                channels[0][i] = mid;
                channels[1][i] = side;
            }

        // --- The stages, oversampled ----------------------------------------------------------------
        const double* dry[2] = {original[0].data(), original[1].data()};
        oversampler_.process(
            channels, n,
            [&](double* data, std::size_t count, int channel)
            {
                const auto ch = static_cast<std::size_t>(channel);
                for (std::size_t j = 0; j < count; ++j)
                {
                    const double x = data[j];
                    double y = x;
                    switch (routing)
                    {
                    case Routing::single: y = runStage(0, ch, x); break;
                    case Routing::serial: y = runStage(2, ch, runStage(1, ch, runStage(0, ch, x))); break;
                    case Routing::parallel:
                    {
                        double total = 0.0;
                        int active = 0;
                        for (std::size_t s = 0; s < state_.size(); ++s)
                            if (state_[s].on)
                            {
                                total += runStage(s, ch, x);
                                ++active;
                            }
                        y = active > 0 ? total / active : x;
                        break;
                    }
                    case Routing::midSide: y = runStage(ch == 0 ? 0 : 1, ch, x); break;
                    case Routing::multiband:
                    {
                        double low = 0.0, rest = 0.0, mid = 0.0, high = 0.0;
                        lowSplit_[ch].split(x, low, rest);
                        highSplit_[ch].split(rest, mid, high);
                        y = runStage(0, ch, lowAlign_[ch].allPass(low)) + runStage(1, ch, mid) + runStage(2, ch, high);
                        break;
                    }
                    }
                    data[j] = y;
                }
            },
            dry);

        if (routing == Routing::midSide)
            for (int i = 0; i < n; ++i)
            {
                const double mid = channels[0][i], side = channels[1][i];
                channels[0][i] = mid + side;
                channels[1][i] = mid - side;
            }

        // --- Feedback tap, glue compressor, output --------------------------------------------------
        for (int i = 0; i < n; ++i)
        {
            const double l = dc_[0].process(channels[0][i]);
            const double r = dc_[1].process(channels[1][i]);
            feedbackLine_[0].push(l);
            feedbackLine_[1].push(r);

            double gain = 1.0;
            if (compressorAmount > 0.0)
            {
                const double power = std::max(l * l, r * r);
                compressorEnvelope_ = power + (compressorEnvelope_ - power) * compressorDetector;
                const double levelDb = 10.0 * std::log10(std::max(compressorEnvelope_, 1.0e-12));
                const double target = levelDb > compressorThreshold ? -(levelDb - compressorThreshold) * 0.75 : 0.0;
                compressorGainDb_ = target + (compressorGainDb_ - target) * (target < compressorGainDb_ ? compressorAttack : compressorRelease);
                maxCompression = std::min(maxCompression, compressorGainDb_);
                gain = dsp::dbToGain(compressorGainDb_ + compressorAmount * 6.0);
            }
            channels[0][i] = oversampler_.dry(0, i) * (1.0 - wet) + l * gain * outputGain * wet;
            channels[1][i] = oversampler_.dry(1, i) * (1.0 - wet) + r * gain * outputGain * wet;
        }
    }

    const auto reduction = static_cast<float>(-maxCompression);
    float previous = compression_.load(std::memory_order_relaxed);
    while (reduction > previous && !compression_.compare_exchange_weak(previous, reduction, std::memory_order_acq_rel))
    {
    }
}

} // namespace dojo::devices

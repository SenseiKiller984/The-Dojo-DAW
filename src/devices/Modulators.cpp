#include "devices/Modulators.h"

#include "devices/dsp/AnalogFilters.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

//==================================================================================================
// Shifter

Shifter::Shifter()
    : BuiltinProcessor(kTypeId, "Shifter",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    mode_ = addChoice("mode", "Mode", {"Pitch", "Frequency", "Ring"}, 0);
    pitch_ = addFloat("pitch", "Pitch", {-48.0f, 48.0f, 1.0f}, 12.0f, "st");
    fine_ = addFloat("fine", "Fine", {-50.0f, 50.0f, 0.1f}, 0.0f, "ct");
    shift_ = addFloat("shift", "Shift", juce::NormalisableRange<float>(-5000.0f, 5000.0f, 0.01f, 0.35f, true), 100.0f, "Hz");
    spread_ = addFloat("spread", "Spread", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    window_ = addFloat("window", "Window", skewedRange(20.0f, static_cast<float>(kMaxWindowMs), 80.0f), 60.0f, "ms");
    lfoAmount_ = addFloat("lfoamt", "LFO Amount", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    lfoRate_ = addFloat("lforate", "LFO Rate", skewedRange(0.01f, 40.0f, 2.0f), 1.0f, "Hz");
    lfoSync_ = addBool("lfosync", "LFO Sync", false);
    lfoDivision_ = addChoice("lfodiv", "LFO Division", dsp::syncDivisionNames(), 9);
    lfoShape_ = addChoice("lfoshape", "LFO Shape", dsp::waveformNames(), 0);
    lfoStereo_ = addFloat("lfostereo", "LFO Stereo", {0.0f, 360.0f, 1.0f}, 0.0f, "deg");
    envAmount_ = addFloat("envamt", "Env Amount", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    envAttack_ = addFloat("envattack", "Env Attack", skewedRange(0.1f, 100.0f, 10.0f), 5.0f, "ms");
    envRelease_ = addFloat("envrelease", "Env Release", skewedRange(10.0f, 2000.0f, 200.0f), 200.0f, "ms");
    delayTime_ = addFloat("delay", "Delay", skewedRange(1.0f, static_cast<float>(kMaxDelaySeconds * 1000.0), 250.0f), 250.0f, "ms");
    delaySync_ = addBool("delaysync", "Delay Sync", false);
    delayDivision_ = addChoice("delaydiv", "Delay Division", dsp::syncDivisionNames(), 6);
    feedback_ = addFloat("feedback", "Feedback", {0.0f, 95.0f, 0.1f}, 0.0f, "%");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
}

void Shifter::prepareDsp(double sampleRate, int)
{
    for (auto& g : grains_)
        g.setMaximum(static_cast<int>(kMaxWindowMs / 1000.0 * sampleRate) + 8);
    for (auto& f : feedbackLine_)
        f.setMaximum(static_cast<int>(kMaxDelaySeconds * sampleRate) + 8);
    head_.fill(0.0);
    for (auto& h : hilbert_)
        h.reset();
    oscillator_.fill(0.0);
    lfo_.reset();
    follower_.reset();
}

void Shifter::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    const auto transport = dsp::readTransport(getPlayHead());
    const auto mode = static_cast<Mode>(std::clamp(mode_->getIndex(), 0, 2));
    const auto shape = static_cast<dsp::Waveform>(std::clamp(lfoShape_->getIndex(), 0, 7));
    const double lfoDepth = lfoAmount_->get() / 100.0;
    const double envDepth = envAmount_->get() / 100.0;
    const double spread = spread_->get() / 100.0;
    const double stereo = lfoStereo_->get() / 360.0;
    const double window = std::min(window_->get() / 1000.0 * sampleRate_, static_cast<double>(grains_[0].capacity() - 2));
    const double feedback = feedback_->get() / 100.0;
    const double delay = std::clamp((delaySync_->get() ? dsp::syncDivisionBeats(delayDivision_->getIndex()) * 60.0 / transport.bpm
                                                       : delayTime_->get() / 1000.0) * sampleRate_,
                                    1.0, static_cast<double>(feedbackLine_[0].capacity()));
    const double wet = mix_->get() / 100.0;
    follower_.setTimes(envAttack_->get() / 1000.0, envRelease_->get() / 1000.0, sampleRate_);

    for (int start = 0; start < numSamples; start += kControlInterval)
    {
        const int n = std::min(kControlInterval, numSamples - start);
        for (int i = start; i < start + n; ++i)
            follower_.process(std::max(std::abs(channels[0][i]), std::abs(channels[1][i])));
        const double envelope = dsp::EnvelopeFollower::normalise(follower_.getLevel());
        const double ppqAtEnd = transport.ppq + (start + n) / sampleRate_ * transport.bpm / 60.0;
        lfo_.advance(n, sampleRate_, lfoRate_->get(), lfoSync_->get(), dsp::syncDivisionBeats(lfoDivision_->getIndex()), transport, ppqAtEnd);

        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            const double lfo = lfo_.value(shape, ch == 1 ? stereo : 0.0);
            const double side = ch == 0 ? 1.0 : -1.0;
            double* x = channels[ch];
            if (mode == Mode::pitch)
            {
                const double semitones = pitch_->get() + fine_->get() / 100.0 + (lfo * lfoDepth + envelope * envDepth) * 12.0 + side * spread * 0.5;
                const double drift = 1.0 - std::exp2(semitones / 12.0); // read-head delay change per sample
                if (ch == 0)
                    currentShift_.store(static_cast<float>(semitones), std::memory_order_relaxed);
                for (int i = start; i < start + n; ++i)
                {
                    const double in = x[i] + (feedback > 0.0 ? dsp::softSaturate(feedbackLine_[ch].read(delay - 1.0) * feedback) : 0.0);
                    grains_[ch].push(in);
                    // Two read heads half a window apart, each faded out where it wraps.
                    const double a = head_[ch];
                    double b = a + window * 0.5;
                    if (b >= window)
                        b -= window;
                    const double ga = std::sin(dsp::kPi * a / window);
                    const double gb = std::sin(dsp::kPi * b / window);
                    const double y = ga * ga * grains_[ch].read(a) + gb * gb * grains_[ch].read(b);
                    head_[ch] += drift;
                    if (head_[ch] < 0.0)
                        head_[ch] += window;
                    else if (head_[ch] >= window)
                        head_[ch] -= window;
                    feedbackLine_[ch].push(y);
                    x[i] = x[i] * (1.0 - wet) + y * wet;
                }
            }
            else
            {
                const double hz = shift_->get() + (lfo * lfoDepth + envelope * envDepth) * 500.0 + side * spread * 20.0;
                const double increment = dsp::kTwoPi * hz / sampleRate_;
                if (ch == 0)
                    currentShift_.store(static_cast<float>(hz), std::memory_order_relaxed);
                for (int i = start; i < start + n; ++i)
                {
                    const double in = x[i] + (feedback > 0.0 ? dsp::softSaturate(feedbackLine_[ch].read(delay - 1.0) * feedback) : 0.0);
                    double y = 0.0;
                    if (mode == Mode::frequency)
                    {
                        double inPhase = 0.0, quadrature = 0.0;
                        hilbert_[ch].process(in, inPhase, quadrature);
                        // (this splitter's quadrature leads: the upper sideband is I cos + Q sin)
                        y = inPhase * std::cos(oscillator_[ch]) + quadrature * std::sin(oscillator_[ch]);
                    }
                    else
                    {
                        y = in * std::cos(oscillator_[ch]);
                    }
                    oscillator_[ch] += increment;
                    if (oscillator_[ch] > dsp::kTwoPi)
                        oscillator_[ch] -= dsp::kTwoPi;
                    else if (oscillator_[ch] < 0.0)
                        oscillator_[ch] += dsp::kTwoPi;
                    feedbackLine_[ch].push(y);
                    x[i] = x[i] * (1.0 - wet) + y * wet;
                }
            }
        }
    }
}

//==================================================================================================
// Chorus-Ensemble

ChorusEnsemble::ChorusEnsemble()
    : BuiltinProcessor(kTypeId, "Chorus-Ensemble",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    mode_ = addChoice("mode", "Mode", {"Classic", "Ensemble", "Vibrato"}, 0);
    rate_ = addFloat("rate", "Rate", skewedRange(0.01f, 20.0f, 1.0f), 0.8f, "Hz");
    amount_ = addFloat("amount", "Amount", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    feedback_ = addFloat("feedback", "Feedback", {-95.0f, 95.0f, 0.1f}, 0.0f, "%");
    width_ = addFloat("width", "Width", {0.0f, 200.0f, 0.1f}, 100.0f, "%");
    warmth_ = addFloat("warmth", "Warmth", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    highPass_ = addFloat("highpass", "High Pass", skewedRange(20.0f, 2000.0f, 200.0f), 20.0f, "Hz");
    shape_ = addFloat("shape", "Vibrato Shape", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    output_ = addFloat("output", "Output", {-24.0f, 12.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
}

double ChorusEnsemble::voiceDelayMs(Mode mode, double lfo, double amount) noexcept
{
    switch (mode)
    {
    case Mode::classic: return 7.0 + lfo * amount * 4.0;
    case Mode::ensemble: return 10.0 + lfo * amount * 3.0;
    case Mode::vibrato: return 6.0 + lfo * amount * 5.0;
    }
    return 7.0;
}

void ChorusEnsemble::prepareDsp(double sampleRate, int)
{
    for (auto& l : lines_)
        l.setMaximum(static_cast<int>(0.05 * sampleRate));
    phase_ = shimmerPhase_ = 0.0;
    feedbackSample_.fill(0.0);
    for (auto& f : warmthFilter_)
        f.reset();
    for (auto& channel : highPassFilter_)
        for (auto& f : channel)
            f.reset();
}

void ChorusEnsemble::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const auto mode = static_cast<Mode>(std::clamp(mode_->getIndex(), 0, 2));
    const double rate = rate_->get() / sampleRate_;
    const double shimmerRate = 6.8 / sampleRate_;
    const double amount = amount_->get() / 100.0;
    const double feedback = feedback_->get() / 100.0;
    const double width = width_->get() / 100.0;
    const double warmth = warmth_->get() / 100.0;
    const double drive = 1.0 + 2.0 * warmth;
    const bool highPass = highPass_->get() > 20.5f;
    const double vibratoShape = shape_->get() / 100.0;
    const double outputGain = dsp::dbToGain(output_->get());
    const double wet = mix_->get() / 100.0;
    const double msToSamples = sampleRate_ / 1000.0;
    for (auto& f : warmthFilter_)
        f.setFrequency(18000.0 * std::exp2(-warmth * 2.2), sampleRate_);
    for (auto& channel : highPassFilter_)
        for (auto& f : channel)
            f.setFrequency(highPass_->get(), sampleRate_);

    for (int i = 0; i < numSamples; ++i)
    {
        phase_ += rate;
        if (phase_ >= 1.0)
            phase_ -= 1.0;
        shimmerPhase_ += shimmerRate;
        if (shimmerPhase_ >= 1.0)
            shimmerPhase_ -= 1.0;

        std::array<double, 2> voice{};
        switch (mode)
        {
        case Mode::classic:
            for (std::size_t ch = 0; ch < 2; ++ch)
            {
                const double lfo = std::sin(dsp::kTwoPi * (phase_ + (ch == 1 ? 0.5 : 0.0)));
                lines_[ch].push((ch == 0 ? left[i] : right[i]) + feedback * feedbackSample_[ch]);
                voice[ch] = lines_[ch].read(voiceDelayMs(mode, lfo, amount) * msToSamples);
                feedbackSample_[ch] = voice[ch];
            }
            break;
        case Mode::ensemble:
        {
            lines_[0].push(0.5 * (left[i] + right[i]) + feedback * feedbackSample_[0]);
            std::array<double, 3> v{};
            for (std::size_t k = 0; k < v.size(); ++k)
            {
                const double offset = static_cast<double>(k) / 3.0;
                const double lfo = std::sin(dsp::kTwoPi * (phase_ + offset));
                const double shimmer = 0.3 * amount * std::sin(dsp::kTwoPi * (shimmerPhase_ + offset));
                v[k] = lines_[0].read((voiceDelayMs(mode, lfo, amount) + shimmer) * msToSamples);
            }
            voice[0] = (v[0] + 0.6 * v[1]) / 1.3;
            voice[1] = (v[2] + 0.6 * v[1]) / 1.3;
            feedbackSample_[0] = (v[0] + v[1] + v[2]) / 3.0;
            break;
        }
        case Mode::vibrato:
            for (std::size_t ch = 0; ch < 2; ++ch)
            {
                const double sine = std::sin(dsp::kTwoPi * phase_);
                const double triangle = dsp::waveformAt(dsp::Waveform::triangle, phase_);
                lines_[ch].push((ch == 0 ? left[i] : right[i]) + feedback * feedbackSample_[ch]);
                voice[ch] = lines_[ch].read(voiceDelayMs(mode, sine + (triangle - sine) * vibratoShape, amount) * msToSamples);
                feedbackSample_[ch] = voice[ch];
            }
            break;
        }

        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            double v = voice[ch];
            if (highPass)
                v = highPassFilter_[ch][1].highPass(highPassFilter_[ch][0].highPass(v));
            if (warmth > 0.0)
                v = warmthFilter_[ch].lowPass(dsp::softSaturate(v * drive) / drive);
            voice[ch] = v;
        }
        const double mid = 0.5 * (voice[0] + voice[1]);
        const double side = 0.5 * (voice[0] - voice[1]) * width;
        left[i] = (left[i] * (1.0 - wet) + (mid + side) * wet) * outputGain;
        right[i] = (right[i] * (1.0 - wet) + (mid - side) * wet) * outputGain;
    }
}

//==================================================================================================
// Auto Pan

AutoPan::AutoPan()
    : BuiltinProcessor(kTypeId, "Auto Pan",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    amount_ = addFloat("amount", "Amount", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    rate_ = addFloat("rate", "Rate", skewedRange(0.01f, 40.0f, 2.0f), 1.0f, "Hz");
    sync_ = addBool("sync", "Sync", false);
    division_ = addChoice("division", "Division", dsp::syncDivisionNames(), 9);
    shape_ = addChoice("shape", "Shape", dsp::waveformNames(), 0);
    phase_ = addFloat("phase", "Phase", {0.0f, 360.0f, 1.0f}, 180.0f, "deg");
    offset_ = addFloat("offset", "Offset", {0.0f, 360.0f, 1.0f}, 0.0f, "deg");
    invert_ = addBool("invert", "Invert", false);
    square_ = addFloat("square", "Square", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
}

double AutoPan::gainFor(double lfo) const noexcept
{
    const double square = square_->get() / 100.0;
    double v = lfo;
    if (square > 0.0)
    {
        const double k = 1.0 + square * 12.0;
        v = std::tanh(v * k) / std::tanh(k);
    }
    if (invert_->get())
        v = -v;
    const double up = 0.5 * (v + 1.0);
    return 1.0 - amount_->get() / 100.0 * (1.0 - up);
}

void AutoPan::prepareDsp(double, int)
{
    lfo_.reset();
    current_.fill(1.0);
}

void AutoPan::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    const auto transport = dsp::readTransport(getPlayHead());
    const auto shape = static_cast<dsp::Waveform>(std::clamp(shape_->getIndex(), 0, 7));
    const double stereo = phase_->get() / 360.0;
    const double offset = offset_->get() / 360.0;
    const double smoothing = 1.0 - std::exp(-1.0 / (0.001 * sampleRate_)); // 1 ms: square shapes without clicks
    for (int start = 0; start < numSamples; start += kControlInterval)
    {
        const int n = std::min(kControlInterval, numSamples - start);
        const double ppqAtEnd = transport.ppq + (start + n) / sampleRate_ * transport.bpm / 60.0;
        lfo_.advance(n, sampleRate_, rate_->get(), sync_->get(), dsp::syncDivisionBeats(division_->getIndex()), transport, ppqAtEnd);
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            const double target = gainFor(lfo_.value(shape, offset + (ch == 1 ? stereo : 0.0)));
            double* x = channels[ch];
            for (int i = start; i < start + n; ++i)
            {
                current_[ch] += (target - current_[ch]) * smoothing;
                x[i] *= current_[ch];
            }
        }
    }
    for (std::size_t ch = 0; ch < 2; ++ch)
        gains_[ch].store(static_cast<float>(current_[ch]), std::memory_order_relaxed);
}

} // namespace dojo::devices

#include "devices/AutoFilter.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace dojo::devices
{

AutoFilter::AutoFilter()
    : BuiltinProcessor(kTypeId, "Auto Filter",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput("Sidechain", juce::AudioChannelSet::stereo(), true),
                       false)
{
    type_ = addChoice("type", "Type", {"Low Pass", "High Pass", "Band Pass", "Notch", "Morph"}, 0);
    slope_ = addChoice("slope", "Slope", {"12 dB", "24 dB"}, 1);
    circuit_ = addChoice("circuit", "Circuit", {"Clean", "Warm", "Ladder", "Diode"}, 1);
    frequency_ = addFloat("freq", "Frequency", skewedRange(20.0f, 20000.0f, 1000.0f), 1500.0f, "Hz");
    resonance_ = addFloat("res", "Resonance", {0.0f, 100.0f, 0.1f}, 25.0f, "%");
    morph_ = addFloat("morph", "Morph", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    drive_ = addFloat("drive", "Drive", {0.0f, 24.0f, 0.1f}, 0.0f, "dB");
    lfoAmount_ = addFloat("lfoamt", "LFO Amount", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    lfoRate_ = addFloat("lforate", "LFO Rate", skewedRange(0.01f, 40.0f, 2.0f), 1.0f, "Hz");
    lfoSync_ = addBool("lfosync", "LFO Sync", false);
    lfoDivision_ = addChoice("lfodiv", "LFO Division", dsp::syncDivisionNames(), 9); // 1/4
    lfoShape_ = addChoice("lfoshape", "LFO Shape", dsp::waveformNames(), 0);
    lfoStereo_ = addFloat("lfostereo", "LFO Stereo", {0.0f, 360.0f, 1.0f}, 0.0f, "deg");
    lfoOffset_ = addFloat("lfooffset", "LFO Offset", {0.0f, 360.0f, 1.0f}, 0.0f, "deg");
    envAmount_ = addFloat("envamt", "Env Amount", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    envAttack_ = addFloat("envattack", "Env Attack", skewedRange(0.1f, 100.0f, 10.0f), 5.0f, "ms");
    envRelease_ = addFloat("envrelease", "Env Release", skewedRange(10.0f, 2000.0f, 200.0f), 200.0f, "ms");
    external_ = addBool("sidechain", "External Sidechain", false);
    output_ = addFloat("output", "Output", {-24.0f, 24.0f, 0.1f}, 0.0f, "dB");
    mix_ = addFloat("mix", "Dry/Wet", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
}

double AutoFilter::getResponseDb(double frequency, double cutoff) const noexcept
{
    // The analog prototype of the state-variable filter: s / wc, damping k.
    const double resonance = resonance_->get() / 100.0;
    const auto stage = [&](double k)
    {
        const std::complex<double> s(0.0, frequency / std::max(1.0, cutoff));
        const auto d = s * s + k * s + 1.0;
        struct R
        {
            std::complex<double> lp, bp, hp, notch;
        } r{1.0 / d, k * s / d, s * s / d, (s * s + 1.0) / d};
        return r;
    };
    const int type = type_->getIndex();
    const double morph = morph_->get() / 100.0;
    const auto pick = [&](const auto& r)
    {
        if (type == 4)
        {
            const double p = morph * 3.0;
            if (p < 1.0)
                return r.lp + (r.bp - r.lp) * p;
            if (p < 2.0)
                return r.bp + (r.hp - r.bp) * (p - 1.0);
            return r.hp + (r.notch - r.hp) * (p - 2.0);
        }
        switch (type)
        {
        case 1: return r.hp;
        case 2: return r.bp;
        case 3: return r.notch;
        default: return r.lp;
        }
    };
    auto h = pick(stage(2.0 - 1.97 * resonance));
    if (slope_->getIndex() == 1)
        h *= pick(stage(2.0 - 1.97 * resonance * 0.2));
    return dsp::gainToDb(std::abs(h));
}

void AutoFilter::prepareDsp(double, int)
{
    for (auto& f : filters_)
        f.reset();
    lfo_.reset();
    follower_.reset();
    cutoff_.fill(frequency_->get());
}

void AutoFilter::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer&) noexcept
{
    const int numSamples = buffer.getNumSamples();
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const bool external = external_->get() && buffer.getNumChannels() >= 4;
    const double* keyLeft = external ? buffer.getReadPointer(2) : left;
    const double* keyRight = external ? buffer.getReadPointer(3) : right;

    const auto transport = dsp::readTransport(getPlayHead());
    const int type = type_->getIndex();
    const auto mode = static_cast<dsp::FilterMode>(std::clamp(type, 0, 3));
    const double morph = type == 4 ? morph_->get() / 100.0 : -1.0;
    const auto circuit = static_cast<dsp::FilterCircuit>(std::clamp(circuit_->getIndex(), 0, 3));
    const bool steep = slope_->getIndex() == 1;
    const double resonance = resonance_->get() / 100.0;
    const double driveGain = dsp::dbToGain(drive_->get());
    const bool drive = drive_->get() > 0.05f;
    const double outputGain = dsp::dbToGain(output_->get()) / std::sqrt(driveGain);
    const double wet = mix_->get() / 100.0;
    const auto shape = static_cast<dsp::Waveform>(std::clamp(lfoShape_->getIndex(), 0, 7));
    const double lfoDepth = lfoAmount_->get() / 100.0 * kLfoOctaves;
    const double envDepth = envAmount_->get() / 100.0 * kEnvelopeOctaves;
    const double stereo = lfoStereo_->get() / 360.0;
    const double offset = lfoOffset_->get() / 360.0;
    const double beats = dsp::syncDivisionBeats(lfoDivision_->getIndex());
    const double maxCutoff = std::min(20000.0, sampleRate_ * 0.45);
    const double smoothing = 1.0 - std::exp(-static_cast<double>(kControlInterval) / (0.004 * sampleRate_)); // ~4 ms
    follower_.setTimes(envAttack_->get() / 1000.0, envRelease_->get() / 1000.0, sampleRate_);

    for (int start = 0; start < numSamples; start += kControlInterval)
    {
        const int n = std::min(kControlInterval, numSamples - start);

        // Envelope follower over this chunk.
        for (int i = start; i < start + n; ++i)
            follower_.process(std::max(std::abs(keyLeft[i]), std::abs(keyRight[i])));
        const double envelope = dsp::EnvelopeFollower::normalise(follower_.getLevel());

        const double ppqAtEnd = transport.ppq + (start + n) / sampleRate_ * transport.bpm / 60.0;
        lfo_.advance(n, sampleRate_, lfoRate_->get(), lfoSync_->get(), beats, transport, ppqAtEnd);
        for (std::size_t ch = 0; ch < 2; ++ch)
        {
            const double lfo = lfo_.value(shape, offset + (ch == 1 ? stereo : 0.0));
            const double target = std::clamp(frequency_->get() * std::exp2(lfo * lfoDepth + envelope * envDepth), 20.0, maxCutoff);
            cutoff_[ch] += (target - cutoff_[ch]) * smoothing;
            filters_[ch].set(cutoff_[ch], resonance, sampleRate_, circuit, steep);
        }

        for (int i = start; i < start + n; ++i)
        {
            double l = left[i] * driveGain;
            double r = right[i] * driveGain;
            if (drive && circuit == dsp::FilterCircuit::clean)
            {
                l = dsp::softSaturate(l);
                r = dsp::softSaturate(r);
            }
            const double fl = filters_[0].process(l, mode, morph) * outputGain;
            const double fr = filters_[1].process(r, mode, morph) * outputGain;
            left[i] = left[i] * (1.0 - wet) + fl * wet;
            right[i] = right[i] * (1.0 - wet) + fr * wet;
        }
    }
    modulatedCutoff_.store(static_cast<float>(cutoff_[0]), std::memory_order_relaxed);
}

} // namespace dojo::devices

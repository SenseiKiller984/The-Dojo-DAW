#include "devices/LfoDevice.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{

LfoDevice::LfoDevice()
    : BuiltinProcessor(kTypeId, "LFO",
                       BusesProperties()
                           .withInput("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput("Output", juce::AudioChannelSet::stereo(), true),
                       false)
{
    shape_ = addChoice("shape", "Shape", dsp::waveformNames(), 0);
    rate_ = addFloat("rate", "Rate", skewedRange(0.01f, 40.0f, 2.0f), 1.0f, "Hz");
    sync_ = addBool("sync", "Sync", false);
    division_ = addChoice("division", "Division", dsp::syncDivisionNames(), 12); // 1/2
    depth_ = addFloat("depth", "Depth", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    offset_ = addFloat("offset", "Offset", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
    phase_ = addFloat("phase", "Phase", {0.0f, 360.0f, 1.0f}, 0.0f, "deg");
    jitter_ = addFloat("jitter", "Jitter", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    smooth_ = addFloat("smooth", "Smooth", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
    hold_ = addBool("hold", "Hold", false);
    retrigger_ = addBool("retrigger", "Retrigger", false);
    invert_ = addBool("invert", "Invert", false);
}

double LfoDevice::shapeAt(double phase) const noexcept
{
    const auto shape = static_cast<dsp::Waveform>(std::clamp(shape_->getIndex(), 0, 7));
    double wave = dsp::waveformAt(shape, phase + phase_->get() / 360.0, 17u);
    if (invert_->get())
        wave = -wave;
    return std::clamp(0.5 + 0.5 * wave * depth_->get() / 100.0 + 0.5 * offset_->get() / 100.0, 0.0, 1.0);
}

double LfoDevice::advanceModulation(int numSamples, double sampleRate, double bpm, double ppq, bool playing) noexcept
{
    const double beats = dsp::syncDivisionBeats(division_->getIndex());
    const double cyclesPerSecond = sync_->get() ? bpm / 60.0 / beats : static_cast<double>(rate_->get());
    const double seconds = numSamples / std::max(1.0, sampleRate);

    if (!hold_->get())
    {
        if (sync_->get() && playing)
            cycles_ = ppq / beats; // locked to the song position
        else if (playing && !wasPlaying_ && retrigger_->get())
            cycles_ = 0.0;
        else
            cycles_ += seconds * cyclesPerSecond;
    }
    wasPlaying_ = playing;

    double value = shapeAt(cycles_);
    if (const double jitter = jitter_->get() / 100.0; jitter > 0.0)
        value = std::clamp(value + 0.5 * jitter * dsp::hashRandom(static_cast<long long>(std::floor(cycles_)), 99u), 0.0, 1.0);

    // Smooth: a lag of up to half a cycle.
    const double smooth = smooth_->get() / 100.0;
    if (!started_ || smooth <= 0.0)
        smoothed_ = value;
    else
    {
        const double lag = smooth * 0.5 / std::max(0.01, cyclesPerSecond);
        smoothed_ = value + (smoothed_ - value) * std::exp(-seconds / lag);
    }
    started_ = true;
    if (hold_->get())
        smoothed_ = static_cast<double>(output_.load(std::memory_order_relaxed));

    output_.store(static_cast<float>(smoothed_), std::memory_order_relaxed);
    phaseDisplay_.store(static_cast<float>(cycles_ - std::floor(cycles_)), std::memory_order_relaxed);
    return smoothed_;
}

void LfoDevice::prepareDsp(double, int)
{
    cycles_ = 0.0;
    started_ = false;
    wasPlaying_ = false;
}

void LfoDevice::processDsp(juce::AudioBuffer<double>&, juce::MidiBuffer&) noexcept
{
    // The audio passes through: the LFO only moves other controls (see advanceModulation()).
}

} // namespace dojo::devices

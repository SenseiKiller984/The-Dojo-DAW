#include "devices/Impulse.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
void storeMax(std::atomic<float>& target, float value) noexcept
{
    float current = target.load(std::memory_order_relaxed);
    while (value > current && !target.compare_exchange_weak(current, value, std::memory_order_acq_rel))
    {
    }
}
} // namespace

Impulse::Impulse()
    : BuiltinProcessor(kTypeId, "Impulse", BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    for (std::size_t s = 0; s < slots_.size(); ++s)
    {
        const juce::String id = "s" + juce::String(s + 1);
        const juce::String name = "Slot " + juce::String(s + 1) + " ";
        auto& slot = slots_[s];
        slot.start = addFloat(id + "start", name + "Start", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
        slot.transpose = addFloat(id + "transpose", name + "Transpose", {-48.0f, 48.0f, 0.01f}, 0.0f, "st");
        slot.drive = addFloat(id + "drive", name + "Drive", {0.0f, 100.0f, 0.1f}, 0.0f, "%");
        slot.transient = addFloat(id + "transient", name + "Transient", {-100.0f, 100.0f, 0.1f}, 0.0f, "%");
        slot.decay = addFloat(id + "decay", name + "Decay", skewedRange(10.0f, kMaxDecayMs, 600.0f), kMaxDecayMs, "ms");
        slot.filter = addChoice(id + "filter", name + "Filter", {"Off", "Low Pass", "High Pass", "Band Pass"}, 0);
        slot.cutoff = addFloat(id + "cutoff", name + "Cutoff", skewedRange(20.0f, 20000.0f, 1500.0f), 5000.0f, "Hz");
        slot.resonance = addFloat(id + "res", name + "Resonance", {0.0f, 100.0f, 0.1f}, 10.0f, "%");
        slot.pan = addFloat(id + "pan", name + "Pan", {-100.0f, 100.0f, 0.1f}, 0.0f);
        slot.volume = addFloat(id + "volume", name + "Volume", {-48.0f, 12.0f, 0.1f}, 0.0f, "dB");
        slot.velocity = addFloat(id + "vel", name + "Velocity", {0.0f, 100.0f, 0.1f}, 50.0f, "%");
    }
    link_ = addBool("link78", "Link 7 & 8", true);
    volume_ = addFloat("volume", "Volume", {-36.0f, 12.0f, 0.1f}, 0.0f, "dB");
    transpose_ = addFloat("transpose", "Transpose", {-24.0f, 24.0f, 1.0f}, 0.0f, "st");
}

juce::String Impulse::getSlotNoteName(int slot)
{
    return juce::MidiMessage::getMidiNoteName(kFirstNote + slot, true, true, 3);
}

bool Impulse::loadSlotSample(int slot, const juce::File& file, juce::String& error)
{
    if (slot < 0 || slot >= kSlots)
        return false;
    auto sample = loadSample(file, error);
    if (sample == nullptr)
        return false;
    setSlotSample(slot, std::move(sample));
    return true;
}

void Impulse::setSlotSample(int slot, std::shared_ptr<const SampleData> sample)
{
    if (slot < 0 || slot >= kSlots)
        return;
    samples_[static_cast<std::size_t>(slot)].set(std::move(sample));
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

std::shared_ptr<const SampleData> Impulse::getSlotSample(int slot) const
{
    return slot >= 0 && slot < kSlots ? samples_[static_cast<std::size_t>(slot)].get() : nullptr;
}

void Impulse::triggerSlot(int slot, float velocity)
{
    (void) auditions_.tryPush({slot, velocity});
}

float Impulse::takeSlotActivity(int slot) noexcept
{
    return slot >= 0 && slot < kSlots ? activity_[static_cast<std::size_t>(slot)].exchange(0.0f, std::memory_order_acq_rel) : 0.0f;
}

void Impulse::prepareDsp(double, int)
{
    for (auto& voice : voices_)
        voice.active = false;
}

void Impulse::writeExtraState(juce::XmlElement& xml) const
{
    for (int slot = 0; slot < kSlots; ++slot)
        if (const auto sample = getSlotSample(slot))
        {
            auto* element = xml.createNewChildElement("SLOT");
            element->setAttribute("index", slot);
            element->setAttribute("sample", sample->file.getFullPathName());
        }
}

void Impulse::readExtraState(const juce::XmlElement& xml)
{
    std::array<bool, kSlots> loaded{};
    for (const auto* element : xml.getChildWithTagNameIterator("SLOT"))
    {
        const int slot = element->getIntAttribute("index", -1);
        if (slot < 0 || slot >= kSlots)
            continue;
        juce::String error;
        loaded[static_cast<std::size_t>(slot)] = loadSlotSample(slot, juce::File(element->getStringAttribute("sample")), error);
    }
    for (int slot = 0; slot < kSlots; ++slot)
        if (!loaded[static_cast<std::size_t>(slot)])
            samples_[static_cast<std::size_t>(slot)].set(nullptr);
}

void Impulse::trigger(int slot, float velocity) noexcept
{
    if (slot < 0 || slot >= kSlots || blockSamples_[static_cast<std::size_t>(slot)] == nullptr)
        return;

    // Slots 7 and 8 (open/closed hi-hat) choke each other when linked.
    if (link_->get() && (slot == 6 || slot == 7))
        for (auto& voice : voices_)
            if (voice.active && voice.slot == (slot == 6 ? 7 : 6))
                voice.fadeStep = 1.0 / (0.005 * sampleRate_);

    Voice* target = nullptr;
    const auto first = static_cast<std::size_t>(slot * kVoicesPerSlot);
    for (std::size_t v = first; v < first + kVoicesPerSlot; ++v)
        if (!voices_[v].active)
        {
            target = &voices_[v];
            break;
        }
    if (target == nullptr)
    {
        target = &voices_[first];
        for (std::size_t v = first; v < first + kVoicesPerSlot; ++v)
            if (voices_[v].age < target->age)
                target = &voices_[v];
    }

    const auto& p = slots_[static_cast<std::size_t>(slot)];
    const auto* sample = blockSamples_[static_cast<std::size_t>(slot)];
    target->active = true;
    target->slot = slot;
    target->position = p.start->get() / 100.0 * std::max(0, sample->getNumFrames() - 1);
    target->velocity = velocity;
    target->elapsed = 0.0;
    target->decay = 1.0;
    target->fade = 1.0;
    target->fadeStep = 0.0;
    target->age = ++ageCounter_;
    for (auto& f : target->filters)
        f.reset();
}

void Impulse::render(juce::AudioBuffer<double>& buffer, int start, int end) noexcept
{
    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    const double globalTranspose = transpose_->get();

    for (auto& voice : voices_)
    {
        if (!voice.active)
            continue;
        const auto slotIndex = static_cast<std::size_t>(voice.slot);
        const auto* sample = blockSamples_[slotIndex];
        if (sample == nullptr)
        {
            voice.active = false;
            continue;
        }

        const auto& p = slots_[slotIndex];
        const int frames = sample->getNumFrames();
        const float* sampleLeft = sample->audio.getReadPointer(0);
        const float* sampleRight = sample->audio.getReadPointer(sample->audio.getNumChannels() > 1 ? 1 : 0);
        const double rate = sample->sampleRate / sampleRate_ * std::exp2((p.transpose->get() + globalTranspose) / 12.0);

        const double drive = p.drive->get() / 100.0;
        const double driveGain = 1.0 + 9.0 * drive;
        const double driveNorm = 1.0 / std::tanh(driveGain);
        const double transient = p.transient->get() / 100.0;
        const double boostTime = 0.008 * sampleRate_;
        const double softenSamples = std::max(1.0, -transient * 0.04 * sampleRate_);
        const bool decays = p.decay->get() < kMaxDecayMs - 1.0f;
        const double decayCoefficient = decays ? std::exp(-4.6 / std::max(1.0, p.decay->get() / 1000.0 * sampleRate_)) : 1.0;
        const int filterType = p.filter->getIndex();
        if (filterType > 0)
            for (auto& f : voice.filters)
                f.setParameters(p.cutoff->get(), p.resonance->get() / 100.0 * 0.95, sampleRate_);
        const auto mode = filterType == 1 ? dsp::StateVariableFilter::Mode::lowPass
                          : filterType == 2 ? dsp::StateVariableFilter::Mode::highPass
                                            : dsp::StateVariableFilter::Mode::bandPass;
        const double velocityGain = 1.0 - p.velocity->get() / 100.0 * (1.0 - voice.velocity);
        const double gain = juce::Decibels::decibelsToGain(static_cast<double>(p.volume->get()), -100.0) * velocityGain;
        const double pan = p.pan->get() / 100.0;
        const double panLeft = std::min(1.0, 1.0 - pan);
        const double panRight = std::min(1.0, 1.0 + pan);

        double peak = 0.0;
        for (int i = start; i < end; ++i)
        {
            const int index = static_cast<int>(voice.position);
            if (index >= frames - 1)
            {
                voice.active = false;
                break;
            }
            const double fraction = voice.position - index;
            double l = sampleLeft[index] + (sampleLeft[index + 1] - sampleLeft[index]) * fraction;
            double r = sampleRight[index] + (sampleRight[index + 1] - sampleRight[index]) * fraction;

            double shape = 1.0;
            if (transient > 0.0)
                shape = 1.0 + transient * 3.0 * std::exp(-voice.elapsed / boostTime);
            else if (transient < 0.0)
                shape = std::min(1.0, voice.elapsed / softenSamples);

            if (drive > 0.0)
            {
                l = std::tanh(l * driveGain) * driveNorm;
                r = std::tanh(r * driveGain) * driveNorm;
            }
            if (filterType > 0)
            {
                l = voice.filters[0].process(l, mode);
                r = voice.filters[1].process(r, mode);
            }

            if (voice.fadeStep > 0.0)
            {
                voice.fade -= voice.fadeStep;
                if (voice.fade <= 0.0)
                {
                    voice.active = false;
                    break;
                }
            }

            const double amp = gain * shape * voice.decay * voice.fade;
            left[i] += l * amp * panLeft;
            right[i] += r * amp * panRight;
            peak = std::max(peak, std::abs(l * amp));

            voice.decay *= decayCoefficient;
            if (decays && voice.decay < 1.0e-4)
            {
                voice.active = false;
                break;
            }
            voice.position += rate;
            voice.elapsed += 1.0;
        }
        storeMax(activity_[slotIndex], static_cast<float>(peak));
    }
}

void Impulse::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();
    for (std::size_t s = 0; s < samples_.size(); ++s)
        blockSamples_[s] = samples_[s].acquire();

    DrumPadTrigger audition;
    while (auditions_.tryPop(audition))
        trigger(audition.pad, audition.velocity);

    int position = 0;
    for (const auto metadata : midi)
    {
        const int at = std::clamp(metadata.samplePosition, 0, numSamples);
        if (at > position)
            render(buffer, position, at);
        position = std::max(position, at);

        if (metadata.numBytes < 3)
            continue;
        const auto status = metadata.data[0] & 0xF0;
        const int note = metadata.data[1];
        if (status == 0x90 && metadata.data[2] > 0 && note >= kFirstNote && note < kFirstNote + kSlots)
            trigger(note - kFirstNote, metadata.data[2] / 127.0f);
        else if (status == 0xB0 && (note == 120 || note == 123))
            for (auto& voice : voices_)
                voice.fadeStep = 1.0 / (0.005 * sampleRate_);
    }
    if (position < numSamples)
        render(buffer, position, numSamples);

    const double gain = juce::Decibels::decibelsToGain(static_cast<double>(volume_->get()), -100.0);
    buffer.applyGain(0, 0, numSamples, gain);
    buffer.applyGain(1, 0, numSamples, gain);
}

} // namespace dojo::devices

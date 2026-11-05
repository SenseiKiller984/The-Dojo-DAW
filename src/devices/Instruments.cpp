#include "devices/Instruments.h"

#include "devices/dsp/Biquad.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices
{
namespace
{
/** Calls `fn(sampleOffset, data, size)` for every MIDI event, without constructing MidiMessages. */
template <typename Fn>
void forEachMidiEvent(const juce::MidiBuffer& midi, int numSamples, Fn&& fn) noexcept
{
    for (const auto metadata : midi)
        fn(std::clamp(metadata.samplePosition, 0, std::max(0, numSamples - 1)), metadata.data, metadata.numBytes);
}

void storeMax(std::atomic<float>& target, float value) noexcept
{
    float previous = target.load(std::memory_order_relaxed);
    while (value > previous && !target.compare_exchange_weak(previous, value, std::memory_order_acq_rel))
    {
    }
}
} // namespace

//==================================================================================================
// Sampler

Sampler::Sampler()
    : BuiltinProcessor(kTypeId, "Sampler",
                       BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true), true)
{
    root_ = addFloat("root", "Root Note", {0.0f, 127.0f, 1.0f}, 60.0f);
    tune_ = addFloat("tune", "Tune", {-24.0f, 24.0f, 1.0f}, 0.0f, "st");
    fine_ = addFloat("fine", "Fine", {-100.0f, 100.0f, 1.0f}, 0.0f, "ct");
    attack_ = addFloat("attack", "Attack", skewedRange(0.0f, 5000.0f, 200.0f), 2.0f, "ms");
    decay_ = addFloat("decay", "Decay", skewedRange(1.0f, 10000.0f, 400.0f), 300.0f, "ms");
    sustain_ = addFloat("sustain", "Sustain", {0.0f, 100.0f, 0.1f}, 100.0f, "%");
    release_ = addFloat("release", "Release", skewedRange(1.0f, 10000.0f, 400.0f), 150.0f, "ms");
    gain_ = addFloat("gain", "Gain", {-36.0f, 12.0f, 0.1f}, 0.0f, "dB");
    velocity_ = addFloat("velocity", "Velocity", {0.0f, 100.0f, 0.1f}, 70.0f, "%");
    start_ = addFloat("start", "Start", {0.0f, 1.0f, 0.0001f}, 0.0f);
    end_ = addFloat("end", "End", {0.0f, 1.0f, 0.0001f}, 1.0f);
    loop_ = addBool("loop", "Loop", false);
}

bool Sampler::loadSample(const juce::File& file, juce::String& error)
{
    auto sample = devices::loadSample(file, error);
    if (sample == nullptr)
        return false;
    setSample(std::move(sample));
    return true;
}

void Sampler::setSample(std::shared_ptr<const SampleData> sample)
{
    sample_.set(std::move(sample));
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

void Sampler::prepareDsp(double sampleRate, int)
{
    for (auto& voice : voices_)
    {
        voice.active = false;
        voice.envelope.setSampleRate(sampleRate);
        voice.envelope.reset();
    }
    sustainPedal_ = false;
    pitchBend_ = 0.0;
}

void Sampler::writeExtraState(juce::XmlElement& xml) const
{
    if (const auto sample = sample_.get())
        xml.setAttribute("sample", sample->file.getFullPathName());
}

void Sampler::readExtraState(const juce::XmlElement& xml)
{
    const auto path = xml.getStringAttribute("sample");
    if (path.isEmpty())
    {
        setSample(nullptr);
        return;
    }

    juce::String error;
    if (!loadSample(juce::File(path), error))
        juce::Logger::writeToLog("Sampler: " + error);
}

void Sampler::noteOn(int note, float velocity) noexcept
{
    Voice* target = nullptr;
    for (auto& voice : voices_)
        if (!voice.active)
        {
            target = &voice;
            break;
        }

    if (target == nullptr) // steal the oldest
    {
        target = &voices_[0];
        for (auto& voice : voices_)
            if (voice.age < target->age)
                target = &voice;
    }

    juce::ADSR::Parameters parameters;
    parameters.attack = attack_->get() / 1000.0f;
    parameters.decay = decay_->get() / 1000.0f;
    parameters.sustain = sustain_->get() / 100.0f;
    parameters.release = release_->get() / 1000.0f;

    const double sensitivity = velocity_->get() / 100.0;
    target->active = true;
    target->note = note;
    target->position = -1.0; // set from the start point at render time
    target->velocityGain = (1.0 - sensitivity) + sensitivity * velocity;
    target->age = ++ageCounter_;
    target->keyDown = true;
    target->sustained = false;
    target->envelope.setParameters(parameters);
    target->envelope.reset();
    target->envelope.noteOn();
}

void Sampler::noteOff(int note) noexcept
{
    for (auto& voice : voices_)
    {
        if (!voice.active || !voice.keyDown || voice.note != note)
            continue;
        voice.keyDown = false;
        if (sustainPedal_)
            voice.sustained = true;
        else
            voice.envelope.noteOff();
    }
}

void Sampler::handleMidi(const std::uint8_t* data, int size) noexcept
{
    if (size < 1)
        return;

    const int status = data[0] & 0xf0;
    const int d1 = size > 1 ? data[1] : 0;
    const int d2 = size > 2 ? data[2] : 0;

    if (status == 0x90 && d2 > 0)
    {
        noteOn(d1, static_cast<float>(d2) / 127.0f);
    }
    else if (status == 0x80 || (status == 0x90 && d2 == 0))
    {
        noteOff(d1);
    }
    else if (status == 0xb0)
    {
        if (d1 == 64)
        {
            sustainPedal_ = d2 >= 64;
            if (!sustainPedal_)
                for (auto& voice : voices_)
                    if (voice.active && voice.sustained)
                    {
                        voice.sustained = false;
                        voice.envelope.noteOff();
                    }
        }
        else if (d1 == 120) // all sound off
        {
            for (auto& voice : voices_)
                voice.active = false;
        }
        else if (d1 == 123) // all notes off
        {
            for (auto& voice : voices_)
                if (voice.active)
                {
                    voice.keyDown = voice.sustained = false;
                    voice.envelope.noteOff();
                }
        }
    }
    else if (status == 0xe0)
    {
        const int bend = (d2 << 7) | d1;
        pitchBend_ = (bend - 8192) / 8192.0 * 2.0;
    }
}

void Sampler::render(double* left, double* right, int start, int end, const SampleData& sample) noexcept
{
    const int frames = sample.getNumFrames();
    const float* sourceLeft = sample.audio.getReadPointer(0);
    const float* sourceRight = sample.audio.getNumChannels() > 1 ? sample.audio.getReadPointer(1) : sourceLeft;
    const double regionStart = std::clamp(static_cast<double>(start_->get()), 0.0, 1.0) * frames;
    const double regionEnd = std::max(regionStart + 1.0, std::clamp(static_cast<double>(end_->get()), 0.0, 1.0) * frames);
    const bool loop = loop_->get();
    const double gain = dsp::dbToGain(gain_->get());
    const double root = root_->get();
    const double tune = tune_->get() + fine_->get() / 100.0 + pitchBend_;
    const double rateRatio = sample.sampleRate / sampleRate_;

    for (auto& voice : voices_)
    {
        if (!voice.active)
            continue;

        if (voice.position < 0.0)
            voice.position = regionStart;

        const double increment = std::pow(2.0, (voice.note - root + tune) / 12.0) * rateRatio;
        const double voiceGain = gain * voice.velocityGain;

        for (int i = start; i < end; ++i)
        {
            if (voice.position >= regionEnd)
            {
                if (loop && regionEnd - regionStart > 1.0)
                {
                    voice.position = regionStart + std::fmod(voice.position - regionStart, regionEnd - regionStart);
                }
                else
                {
                    voice.active = false;
                    break;
                }
            }

            const double envelope = voice.envelope.getNextSample();
            if (!voice.envelope.isActive())
            {
                voice.active = false;
                break;
            }

            const double l = dsp::hermite(sourceLeft, frames, voice.position);
            const double r = sourceRight == sourceLeft ? l : dsp::hermite(sourceRight, frames, voice.position);
            left[i] += l * voiceGain * envelope;
            right[i] += r * voiceGain * envelope;
            voice.position += increment;
        }
    }
}

void Sampler::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    const SampleData* sample = sample_.acquire();
    if (sample != lastSample_)
    {
        for (auto& voice : voices_)
            voice.active = false; // positions refer to the old sample
        lastSample_ = sample;
    }

    double* left = buffer.getWritePointer(0);
    double* right = buffer.getWritePointer(1);
    int rendered = 0;

    forEachMidiEvent(midi, numSamples,
                     [&](int offset, const std::uint8_t* data, int size)
                     {
                         if (sample != nullptr && offset > rendered)
                             render(left, right, rendered, offset, *sample);
                         rendered = std::max(rendered, offset);
                         handleMidi(data, size);
                     });

    if (sample != nullptr && rendered < numSamples)
        render(left, right, rendered, numSamples, *sample);
    if (sample == nullptr)
        for (auto& voice : voices_)
            voice.active = false;

    int active = 0;
    for (const auto& voice : voices_)
        active += voice.active ? 1 : 0;
    activeVoices_.store(active, std::memory_order_relaxed);
}

//==================================================================================================
// DrumRack

juce::AudioProcessor::BusesProperties DrumRack::makeBuses()
{
    auto buses = BusesProperties().withOutput("Main", juce::AudioChannelSet::stereo(), true);
    for (int pad = 0; pad < kPads; ++pad)
        buses = buses.withOutput("Out " + juce::String(pad + 1), juce::AudioChannelSet::stereo(), true);
    return buses;
}

namespace
{
juce::StringArray outputNames()
{
    juce::StringArray names{"Main"};
    for (int pad = 0; pad < DrumRack::kPads; ++pad)
        names.add("Out " + juce::String(pad + 1));
    return names;
}
} // namespace

DrumRack::DrumRack()
    : BuiltinProcessor(kTypeId, "Drum Rack", makeBuses(), true)
{
    for (int pad = 0; pad < kPads; ++pad)
    {
        const auto prefix = "pad" + juce::String(pad + 1);
        const auto label = "Pad " + juce::String(pad + 1) + " ";
        auto& p = pads_[static_cast<std::size_t>(pad)];
        p.volume = addFloat(prefix + "Volume", label + "Volume", {-60.0f, 6.0f, 0.1f}, 0.0f, "dB");
        p.pan = addFloat(prefix + "Pan", label + "Pan", {-1.0f, 1.0f, 0.01f}, 0.0f);
        p.pitch = addFloat(prefix + "Pitch", label + "Pitch", {-24.0f, 24.0f, 0.1f}, 0.0f, "st");
        p.output = addChoice(prefix + "Output", label + "Output", outputNames(), 0);
        p.choke = addChoice(prefix + "Choke", label + "Choke", {"None", "Group 1", "Group 2", "Group 3", "Group 4"}, 0);
        activity_[static_cast<std::size_t>(pad)].store(0.0f);
    }
    master_ = addFloat("master", "Master", {-60.0f, 6.0f, 0.1f}, 0.0f, "dB");
}

juce::String DrumRack::getPadNoteName(int pad)
{
    return juce::MidiMessage::getMidiNoteName(kFirstNote + pad, true, true, 3);
}

bool DrumRack::loadPadSample(int pad, const juce::File& file, juce::String& error)
{
    if (pad < 0 || pad >= kPads)
        return false;
    auto sample = loadSample(file, error);
    if (sample == nullptr)
        return false;
    setPadSample(pad, std::move(sample));
    return true;
}

void DrumRack::setPadSample(int pad, std::shared_ptr<const SampleData> sample)
{
    if (pad < 0 || pad >= kPads)
        return;
    samples_[static_cast<std::size_t>(pad)].set(std::move(sample));
    updateHostDisplay(juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged(true));
}

std::shared_ptr<const SampleData> DrumRack::getPadSample(int pad) const
{
    return pad >= 0 && pad < kPads ? samples_[static_cast<std::size_t>(pad)].get() : nullptr;
}

void DrumRack::triggerPad(int pad, float velocity)
{
    (void)auditions_.tryPush({pad, velocity});
}

float DrumRack::takePadActivity(int pad) noexcept
{
    return pad >= 0 && pad < kPads ? activity_[static_cast<std::size_t>(pad)].exchange(0.0f, std::memory_order_acq_rel) : 0.0f;
}

void DrumRack::prepareDsp(double, int)
{
    for (auto& voice : voices_)
        voice.active = false;
}

void DrumRack::writeExtraState(juce::XmlElement& xml) const
{
    for (int pad = 0; pad < kPads; ++pad)
        if (const auto sample = getPadSample(pad))
        {
            auto* element = xml.createNewChildElement("PAD");
            element->setAttribute("index", pad);
            element->setAttribute("sample", sample->file.getFullPathName());
        }
}

void DrumRack::readExtraState(const juce::XmlElement& xml)
{
    std::array<bool, kPads> loaded{};
    for (const auto* element : xml.getChildWithTagNameIterator("PAD"))
    {
        const int pad = element->getIntAttribute("index", -1);
        if (pad < 0 || pad >= kPads)
            continue;
        juce::String error;
        loaded[static_cast<std::size_t>(pad)] = loadPadSample(pad, juce::File(element->getStringAttribute("sample")), error);
    }
    for (int pad = 0; pad < kPads; ++pad)
        if (!loaded[static_cast<std::size_t>(pad)])
            samples_[static_cast<std::size_t>(pad)].set(nullptr);
}

void DrumRack::trigger(int pad, float velocity) noexcept
{
    if (pad < 0 || pad >= kPads || blockSamples_[static_cast<std::size_t>(pad)] == nullptr)
        return;

    // Choke: a hit silences (quickly fades) the other pads of its group.
    const int group = pads_[static_cast<std::size_t>(pad)].choke->getIndex();
    if (group > 0)
        for (auto& voice : voices_)
            if (voice.active && voice.pad != pad && pads_[static_cast<std::size_t>(voice.pad)].choke->getIndex() == group)
                voice.fadeStep = 1.0 / (0.005 * sampleRate_);

    // A free voice of this pad, or its oldest one.
    Voice* target = nullptr;
    const auto first = static_cast<std::size_t>(pad * kVoicesPerPad);
    for (std::size_t v = first; v < first + kVoicesPerPad; ++v)
        if (!voices_[v].active)
        {
            target = &voices_[v];
            break;
        }
    if (target == nullptr)
    {
        target = &voices_[first];
        for (std::size_t v = first; v < first + kVoicesPerPad; ++v)
            if (voices_[v].age < target->age)
                target = &voices_[v];
    }

    target->active = true;
    target->pad = pad;
    target->position = 0.0;
    target->velocity = 0.2 + 0.8 * velocity;
    target->fade = 1.0;
    target->fadeStep = 0.0;
    target->age = ++ageCounter_;
}

void DrumRack::render(juce::AudioBuffer<double>& buffer, int start, int end) noexcept
{
    const double master = dsp::dbToGain(master_->get());
    const int numChannels = buffer.getNumChannels();

    for (auto& voice : voices_)
    {
        if (!voice.active)
            continue;

        const auto padIndex = static_cast<std::size_t>(voice.pad);
        const SampleData* sample = blockSamples_[padIndex];
        if (sample == nullptr)
        {
            voice.active = false;
            continue;
        }

        const auto& p = pads_[padIndex];
        const auto [panLeft, panRight] = [&]
        {
            const double pan = p.pan->get();
            return std::pair<double, double>{pan <= 0.0 ? 1.0 : 1.0 - pan, pan >= 0.0 ? 1.0 : 1.0 + pan};
        }();
        const double gain = dsp::dbToGain(p.volume->get()) * master * voice.velocity;
        const double increment = std::pow(2.0, p.pitch->get() / 12.0) * sample->sampleRate / sampleRate_;

        const int output = p.output->getIndex();
        const int firstChannel = output > 0 && 2 * output + 1 < numChannels ? 2 * output : 0;
        double* left = buffer.getWritePointer(firstChannel);
        double* right = buffer.getWritePointer(firstChannel + 1);

        const int frames = sample->getNumFrames();
        const float* sourceLeft = sample->audio.getReadPointer(0);
        const float* sourceRight = sample->audio.getNumChannels() > 1 ? sample->audio.getReadPointer(1) : sourceLeft;
        float peak = 0.0f;

        for (int i = start; i < end; ++i)
        {
            if (voice.position >= frames || voice.fade <= 0.0)
            {
                voice.active = false;
                break;
            }

            const double l = dsp::hermite(sourceLeft, frames, voice.position);
            const double r = sourceRight == sourceLeft ? l : dsp::hermite(sourceRight, frames, voice.position);
            const double g = gain * voice.fade;
            left[i] += l * g * panLeft;
            right[i] += r * g * panRight;
            peak = std::max(peak, static_cast<float>(std::abs(l * g)));

            voice.position += increment;
            if (voice.fadeStep > 0.0)
                voice.fade -= voice.fadeStep;
        }

        storeMax(activity_[padIndex], peak);
    }
}

void DrumRack::processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int numSamples = buffer.getNumSamples();
    buffer.clear();

    for (int pad = 0; pad < kPads; ++pad)
        blockSamples_[static_cast<std::size_t>(pad)] = samples_[static_cast<std::size_t>(pad)].acquire();

    DrumPadTrigger audition;
    while (auditions_.tryPop(audition))
        trigger(audition.pad, audition.velocity);

    int rendered = 0;
    forEachMidiEvent(midi, numSamples,
                     [&](int offset, const std::uint8_t* data, int size)
                     {
                         if (offset > rendered)
                             render(buffer, rendered, offset);
                         rendered = std::max(rendered, offset);

                         if (size >= 3 && (data[0] & 0xf0) == 0x90 && data[2] > 0)
                             trigger(data[1] - kFirstNote, static_cast<float>(data[2]) / 127.0f);
                         else if (size >= 3 && (data[0] & 0xf0) == 0xb0 && data[1] == 120)
                             for (auto& voice : voices_)
                                 voice.active = false;
                     });

    if (rendered < numSamples)
        render(buffer, rendered, numSamples);
}

} // namespace dojo::devices

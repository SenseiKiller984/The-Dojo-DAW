#include "devices/SampleSlot.h"

#include <algorithm>

namespace dojo::devices
{

std::shared_ptr<const SampleData> loadSample(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile())
    {
        error = "File not found: " + file.getFullPathName();
        return nullptr;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
    {
        error = "Unsupported or unreadable audio file: " + file.getFileName();
        return nullptr;
    }

    constexpr double kMaxSeconds = 10.0 * 60.0;
    if (reader->lengthInSamples <= 0 || reader->sampleRate <= 0.0 ||
        static_cast<double>(reader->lengthInSamples) / reader->sampleRate > kMaxSeconds)
    {
        error = "Samples must be between a few samples and ten minutes long: " + file.getFileName();
        return nullptr;
    }

    auto sample = std::make_shared<SampleData>();
    sample->file = file;
    sample->sampleRate = reader->sampleRate;
    const int channels = reader->numChannels > 1 ? 2 : 1;
    const auto frames = static_cast<int>(reader->lengthInSamples);
    sample->audio.setSize(channels, frames);
    if (!reader->read(&sample->audio, 0, frames, 0, true, channels > 1))
    {
        error = "Could not decode " + file.getFileName();
        return nullptr;
    }

    return sample;
}

void SampleSlot::set(std::shared_ptr<const SampleData> sample)
{
    if (current_ != nullptr)
        retired_.push_back(current_);

    current_ = std::move(sample);
    pending_.store(current_.get(), std::memory_order_release);
    collect();
}

void SampleSlot::collect()
{
    const SampleData* inUse = acknowledged_.load(std::memory_order_acquire);
    const SampleData* pending = pending_.load(std::memory_order_acquire);

    // Anything the audio thread has moved on from (it acknowledged the pending sample) can go.
    if (inUse == pending)
        retired_.erase(std::remove_if(retired_.begin(), retired_.end(),
                                      [&](const auto& s) { return s.get() != inUse && s.get() != pending; }),
                       retired_.end());
}

} // namespace dojo::devices

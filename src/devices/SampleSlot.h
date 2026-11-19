#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <memory>
#include <vector>

namespace dojo::devices
{

/** A sample loaded into an instrument: decoded audio (1 or 2 channels) and where it came from. */
struct SampleData
{
    juce::File file;
    juce::AudioBuffer<float> audio;
    double sampleRate = 48000.0;

    [[nodiscard]] int getNumFrames() const noexcept { return audio.getNumSamples(); }
};

/** Decodes an audio file for an instrument (message thread). Null with `error` set on failure. */
std::shared_ptr<const SampleData> loadSample(const juce::File& file, juce::String& error);

/**
    Real-time-safe hand-over of a sample from the message thread to the audio thread.

    The message thread publishes a new sample with set(); the audio thread picks it up with acquire()
    once per block and acknowledges what it is now using. A replaced sample is freed (on the message
    thread) only after the audio thread has acknowledged a newer one, so it can never be freed while
    a block is still reading it.
*/
class SampleSlot final
{
public:
    SampleSlot() = default;
    ~SampleSlot() = default;
    SampleSlot(const SampleSlot&) = delete;
    SampleSlot& operator=(const SampleSlot&) = delete;

    /** Message thread. Null clears the slot. */
    void set(std::shared_ptr<const SampleData> sample);
    /** Message thread: what the slot holds (the newest sample set). */
    [[nodiscard]] std::shared_ptr<const SampleData> get() const { return current_; }

    /** Audio thread, once per block: the sample to use for this block (may be null). */
    const SampleData* acquire() noexcept
    {
        const SampleData* sample = pending_.load(std::memory_order_acquire);
        acknowledged_.store(sample, std::memory_order_release);
        return sample;
    }

private:
    void collect();

    std::atomic<const SampleData*> pending_{nullptr};
    std::atomic<const SampleData*> acknowledged_{nullptr};
    std::shared_ptr<const SampleData> current_;
    std::vector<std::shared_ptr<const SampleData>> retired_;
};

} // namespace dojo::devices

#pragma once

#include "core/Types.h"
#include "core/rt/SpscQueue.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace dojo::engine
{

class AudioEngine;

/**
    Deferred destruction for objects the audio thread may still be reading (for example a sample a
    Drum Rack pad just replaced). The message thread hands the last owner here; the object is freed
    once the engine has completed two further processing cycles (so no block that could have seen
    it is still running), or immediately when the engine is idle. Message thread only.
*/
class ReleasePool final
{
public:
    explicit ReleasePool(const AudioEngine& engine);

    void release(std::shared_ptr<const void> object);

    /** Frees everything that is now safe to free. Called regularly by EngineController. */
    void collect();

    [[nodiscard]] std::size_t getPendingCount() const noexcept { return pending_.size(); }

private:
    struct Entry
    {
        std::shared_ptr<const void> object;
        std::uint64_t epoch = 0;
    };

    const AudioEngine& engine_;
    std::vector<Entry> pending_;
};

/** A MIDI message from a hardware input, as queued for the audio thread. */
struct LiveMidiEvent
{
    std::uint8_t data[3]{};
    std::uint8_t size = 0;
};

/**
    Hardware MIDI input -> audio thread. MIDI callbacks may arrive on several driver threads, so the
    producer side is serialised by a mutex (never touched by the audio thread); the audio thread
    drains wait-free.
*/
class LiveMidiQueue final
{
public:
    LiveMidiQueue();

    /** Any MIDI driver thread. Short, non-sysex messages only. Returns false if the queue is full. */
    bool push(const juce::MidiMessage& message);

    /** Audio thread. */
    [[nodiscard]] bool pop(LiveMidiEvent& event) noexcept { return queue_.tryPop(event); }

private:
    std::mutex producerMutex_;
    rt::SpscQueue<LiveMidiEvent> queue_{4096};
};

/**
    A lock-free audio capture stream: the audio thread writes blocks (wait-free, no allocation, no
    wake-up syscall), a disk thread polls and writes them to an AudioFormatWriter. Used for recording
    inputs, real-time bounces and stems.
*/
class CaptureStream final
{
public:
    /** `secondsOfBuffer` of audio at `sampleRate` are buffered between the two threads. */
    CaptureStream(std::unique_ptr<juce::AudioFormatWriter> writer, int numChannels, double sampleRate,
                  double secondsOfBuffer = 8.0);
    ~CaptureStream();

    /** Audio thread: appends `numSamples` frames (channel pointers are 64-bit). */
    void write(const Sample* const* channels, int numChannels, int numSamples) noexcept;

    /** Disk thread: writes everything buffered so far to the file. Returns frames written. */
    int drainToDisk();

    /** Frames dropped because the disk thread fell behind (should stay 0). */
    [[nodiscard]] std::uint64_t getOverflowCount() const noexcept { return overflow_.load(); }
    [[nodiscard]] std::uint64_t getFramesWritten() const noexcept { return framesWritten_; }

    /** Flushes and closes the file (after the audio thread has stopped writing). */
    void finish();

private:
    std::unique_ptr<juce::AudioFormatWriter> writer_;
    int numChannels_;
    int capacity_;
    std::vector<float> ring_; // interleaved by channel blocks: [channel][capacity]
    std::atomic<std::uint64_t> writePosition_{0};
    std::atomic<std::uint64_t> readPosition_{0};
    std::atomic<std::uint64_t> overflow_{0};
    std::uint64_t framesWritten_ = 0;
    juce::AudioBuffer<float> chunk_;
};

/**
    Background thread that drains every registered CaptureStream to disk every few milliseconds.
*/
class DiskWriterThread final : private juce::Thread
{
public:
    DiskWriterThread();
    ~DiskWriterThread() override;

    void add(std::shared_ptr<CaptureStream> stream);
    void remove(const std::shared_ptr<CaptureStream>& stream); ///< drains it fully first

private:
    void run() override;

    std::mutex mutex_;
    std::vector<std::shared_ptr<CaptureStream>> streams_;
};

} // namespace dojo::engine

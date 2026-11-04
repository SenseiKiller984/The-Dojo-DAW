#pragma once

#include "core/rt/SpscQueue.h"
#include "core/rt/ThreadOwnershipChecker.h"
#include "core/Types.h"
#include "engine/dsp/TestToneGenerator.h"
#include "engine/EngineCommands.h"
#include "engine/EngineTelemetry.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <chrono>
#include <cstddef>

namespace dojo::engine
{

/**
    The real-time heart of The Dojo.

    AudioEngine owns all state touched by the audio thread and is deliberately independent of how
    it is driven: the device host calls process() from the driver's callback, the test-suite calls
    it directly, and the offline bouncer (export phase) will call it from a worker thread.

    Threading contract
    ------------------
    | Function            | Thread                         | Real-time safe |
    |---------------------|--------------------------------|----------------|
    | prepare / release   | any non-RT thread, never while process() runs | no (allocates) |
    | post                | the message thread (single producer)          | yes            |
    | collectTelemetry    | the message thread (single consumer)          | yes            |
    | process             | the audio thread                              | YES            |

    process() never allocates or frees memory, never takes a lock, never blocks and never calls into
    the UI. Debug builds verify the allocation rule at run time (dojo::rt::ScopedRealtimeSection +
    the allocation trap) and the test-suite asserts it.

    Signal path (Phase 1)
    ---------------------
        [test tone] -> 64-bit master bus -> master gain (smoothed) -> metering -> float device out
    The track/bus processing graph (rendered across a worker pool) slots in ahead of the master bus
    in later phases.
*/
class AudioEngine final
{
public:
    static constexpr std::size_t kCommandQueueCapacity = 4096;

    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;
    AudioEngine(AudioEngine&&) = delete;
    AudioEngine& operator=(AudioEngine&&) = delete;

    //==============================================================================================
    // Lifecycle — non-real-time; must not overlap process().

    /** Allocates every buffer the audio thread will need for blocks of up to `maximumBlockSize`. */
    void prepare(double sampleRate, int maximumBlockSize, int numOutputChannels);

    /** Marks the engine unprepared; process() outputs silence until the next prepare(). */
    void release() noexcept;

    [[nodiscard]] bool isPrepared() const noexcept { return prepared_.load(std::memory_order_acquire); }

    /** Sample rate of the most recent prepare() (0 if never prepared). Any thread. */
    [[nodiscard]] double getSampleRate() const noexcept
    {
        return publishedSampleRate_.load(std::memory_order_relaxed);
    }

    //==============================================================================================
    // Message thread

    /** Enqueues a command for the audio thread. Wait-free. Returns false if the queue is full, in
        which case the caller keeps the command and retries later (see EngineController). */
    [[nodiscard]] bool post(const EngineCommand& command) noexcept;

    /** Copies the latest telemetry and resets the "max since last read" fields. Call once per frame. */
    [[nodiscard]] TelemetrySnapshot collectTelemetry() noexcept;

    //==============================================================================================
    // Audio thread

    /** Renders one device block. `outputs[ch]` may be null for inactive channels. Handles blocks
        larger than the prepared maximum by rendering in sub-blocks. */
    void process(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                 int numSamples) noexcept;

private:
    using Clock = std::chrono::steady_clock;

    void applyPendingCommands() noexcept;
    void handle(const command::SetPlaying& cmd) noexcept;
    void handle(const command::Locate& cmd) noexcept;
    void handle(const command::SetMasterGain& cmd) noexcept;
    void handle(const command::SetTestTone& cmd) noexcept;

    void renderSubBlock(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void applyMasterGain(int numSamples) noexcept;
    void writeOutputsAndMeter(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void publishLoad(Clock::duration elapsed, int numSamples) noexcept;

    // --- Cross-thread channels -------------------------------------------------------------------
    rt::SpscQueue<EngineCommand> commands_{kCommandQueueCapacity};
    EngineTelemetry telemetry_;
    rt::ThreadOwnershipChecker commandProducer_;

    std::atomic<bool> prepared_{false};
    std::atomic<double> publishedSampleRate_{0.0};
    static_assert(std::atomic<double>::is_always_lock_free);

    // --- Configuration: written by prepare(), read-only while processing ---------------------------
    double sampleRate_ = 0.0;
    int maxBlockSize_ = 0;
    int numChannels_ = 0;
    juce::AudioBuffer<Sample> masterBus_; // 64-bit internal mix

    // --- Audio-thread state ----------------------------------------------------------------------
    SampleCount playhead_ = 0;
    bool playing_ = false;
    double masterGainTarget_ = 1.0;
    juce::SmoothedValue<Sample, juce::ValueSmoothingTypes::Linear> masterGain_;
    dsp::TestToneGenerator testTone_;
    double smoothedLoad_ = 0.0;
};

} // namespace dojo::engine

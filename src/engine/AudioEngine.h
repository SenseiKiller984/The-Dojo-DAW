#pragma once

#include "core/rt/SpscQueue.h"
#include "core/rt/ThreadOwnershipChecker.h"
#include "core/Types.h"
#include "engine/dsp/TestToneGenerator.h"
#include "engine/EngineCommands.h"
#include "engine/EngineTelemetry.h"
#include "engine/RenderGraph.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
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

    Signal path
    -----------
        clips -> per-track 64-bit bus -> smoothed L/R gain (volume, pan, mute/solo, phase) -> meter
              -> 64-bit master bus (+ test tone) -> smoothed master gain -> metering -> float out

    What is rendered is described by an immutable RenderGraph swapped in by pointer; see
    RenderGraph.h for the ownership protocol. Parallel rendering across a worker pool slots in at the
    per-track stage in a later phase.
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

    /** Post-fader peaks of the track rendered in `slot` since the previous call (then reset). */
    [[nodiscard]] StereoPeak collectTrackPeak(int slot) noexcept;

    /** Serial of the RenderGraph the audio thread is currently rendering (0 = none yet). Any thread. */
    [[nodiscard]] std::uint64_t getAppliedGraphSerial() const noexcept
    {
        return telemetry_.appliedGraphSerial.load(std::memory_order_acquire);
    }

    /** Destroys every graph the audio thread has swapped out. Call regularly; never allocates on the
        audio thread's behalf, only frees what it handed back. */
    void releaseRetiredGraphs() noexcept;

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
    void handle(const command::SwapGraph& cmd) noexcept;
    void retire(RenderGraph* graph) noexcept;

    void renderTracks(int numSamples) noexcept;

    void renderSubBlock(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void applyMasterGain(int numSamples) noexcept;
    void writeOutputsAndMeter(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void publishLoad(Clock::duration elapsed, int numSamples) noexcept;

    // --- Cross-thread channels -------------------------------------------------------------------
    rt::SpscQueue<EngineCommand> commands_{kCommandQueueCapacity};
    rt::SpscQueue<RenderGraph*> retiredGraphs_{64}; // audio -> message thread, for deletion
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
    juce::AudioBuffer<Sample> trackBus_;  // 64-bit stereo scratch for one track at a time

    // --- Audio-thread state ----------------------------------------------------------------------
    SampleCount playhead_ = 0;
    bool playing_ = false;
    double masterGainTarget_ = 1.0;
    juce::SmoothedValue<Sample, juce::ValueSmoothingTypes::Linear> masterGain_;
    dsp::TestToneGenerator testTone_;
    double smoothedLoad_ = 0.0;

    RenderGraph* graph_ = nullptr; // owned; replaced only via command::SwapGraph

    /** Retired graphs that did not fit in retiredGraphs_ (practically never used). */
    std::array<RenderGraph*, 16> retireBacklog_{};
    std::size_t retireBacklogSize_ = 0;

    /** Per-track state that must persist across graph swaps (indexed by RenderTrack::slot). */
    struct TrackRuntime
    {
        std::uint32_t trackId = 0;
        juce::SmoothedValue<Sample, juce::ValueSmoothingTypes::Linear> gainLeft;
        juce::SmoothedValue<Sample, juce::ValueSmoothingTypes::Linear> gainRight;
    };
    std::array<TrackRuntime, static_cast<std::size_t>(kMaxTracks)> trackRuntime_{};
};

} // namespace dojo::engine

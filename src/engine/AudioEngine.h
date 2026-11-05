#pragma once

#include "core/Types.h"
#include "core/rt/SpscQueue.h"
#include "core/rt/ThreadOwnershipChecker.h"
#include "engine/EngineCommands.h"
#include "engine/EngineTelemetry.h"
#include "engine/RealtimeHandoff.h"
#include "engine/RenderGraph.h"
#include "engine/WorkerPool.h"
#include "engine/dsp/TestToneGenerator.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <vector>

namespace dojo::engine
{

struct CaptureSession;

/**
    The real-time heart of The Dojo.

    AudioEngine owns all state touched by the audio thread and is deliberately independent of how
    it is driven: the device host calls process() from the driver's callback, the test-suite calls
    it directly, and the exporter calls renderOffline() from its own thread while live processing is
    paused.

    Threading contract
    ------------------
    | Function                     | Thread                                      | Real-time safe |
    |------------------------------|---------------------------------------------|----------------|
    | prepare / release            | any non-RT thread, never while rendering    | no (allocates) |
    | post / collect* / release*   | the message thread                          | yes            |
    | process                      | the audio thread (+ its worker pool)        | YES            |
    | begin/endOfflineRender       | the export thread                           | no             |
    | renderOffline                | the export thread, between begin/end        | yes            |

    Nothing reachable from process() allocates or frees memory, takes a lock, blocks or calls into the
    UI. Debug builds verify the allocation rule on the audio thread *and* the worker threads
    (dojo::rt::ScopedRealtimeSection + the allocation trap) and the test-suite asserts it.

    Signal path (per block)
    -----------------------
        for each dependency level of the RenderGraph, in parallel across the WorkerPool:
            track source (clips / MIDI clips -> instrument / bus sum of children, sends, aux outs /
                          monitored input)
            -> device chain (sidechains pulled from earlier levels)
            -> pre-fader copy (for pre-fader sends) -> smoothed volume/pan (automation) -> meter
            -> latency-compensation delay
        master: sum of tracks routed to master -> master devices -> master gain -> meter -> device out
*/
class AudioEngine final
{
public:
    static constexpr std::size_t kCommandQueueCapacity = 4096;

    explicit AudioEngine(int numWorkerThreads = WorkerPool::defaultWorkerCount());
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    //==============================================================================================
    // Lifecycle — non-real-time; never while rendering.

    /** Allocates every buffer the audio thread will need for blocks of up to `maximumBlockSize`,
        then calls onPrepare (to prepare every processor at the same rate/size). */
    void prepare(double sampleRate, int maximumBlockSize, int numOutputChannels, int numInputChannels = 0);

    /** Marks the engine unprepared; process() outputs silence until the next prepare(). */
    void release() noexcept;

    /** Called at the end of prepare() with (sampleRate, maximumBlockSize) — used to prepare devices. */
    std::function<void(double, int)> onPrepare;

    [[nodiscard]] bool isPrepared() const noexcept { return prepared_.load(std::memory_order_acquire); }
    [[nodiscard]] double getSampleRate() const noexcept
    {
        return publishedSampleRate_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int getMaximumBlockSize() const noexcept
    {
        return publishedBlockSize_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int getNumInputChannels() const noexcept { return numInputs_; }

    /** The play head the engine exposes to devices and plug-ins (tempo, position, loop). */
    [[nodiscard]] juce::AudioPlayHead* getPlayHead() noexcept { return &playHead_; }

    //==============================================================================================
    // Message thread

    [[nodiscard]] bool post(const EngineCommand& command) noexcept;
    [[nodiscard]] TelemetrySnapshot collectTelemetry() noexcept;
    [[nodiscard]] StereoPeak collectTrackPeak(int slot) noexcept;
    [[nodiscard]] float collectInputPeak(int channel) noexcept;

    [[nodiscard]] std::uint64_t getAppliedGraphSerial() const noexcept
    {
        return telemetry_.appliedGraphSerial.load(std::memory_order_acquire);
    }

    void releaseRetiredGraphs() noexcept;

    [[nodiscard]] LiveMidiQueue& getLiveMidiQueue() noexcept { return liveMidi_; }

    /** Monotonic count of finished processing cycles (live or offline). For deferred releases. */
    [[nodiscard]] std::uint64_t getProcessEpoch() const noexcept { return epoch_.load(std::memory_order_acquire); }

    /** False while nothing can be rendering: no device running and no offline render. */
    [[nodiscard]] bool isProcessingActive() const noexcept
    {
        return isPrepared() || offlineActive_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isCaptureActive() const noexcept
    {
        return telemetry_.captureActive.load(std::memory_order_acquire);
    }
    [[nodiscard]] double getCaptureStartSeconds() const noexcept { return telemetry_.captureStartSeconds.load(); }
    /** Number of StopCapture commands the audio thread has handled (acknowledgement for the owner of
        a CaptureSession: once this passes the value read before posting StopCapture, the session is
        no longer referenced). */
    [[nodiscard]] std::uint32_t getCaptureStopCount() const noexcept
    {
        return telemetry_.captureStops.load(std::memory_order_acquire);
    }

    //==============================================================================================
    // Offline rendering (export thread)

    /** Pauses live processing (the device then outputs silence) and makes the calling thread the
        renderer. Waits for a live callback that is in progress to finish; returns false (and does
        not pause) if it does not finish within `timeoutMs`. */
    bool beginOfflineRender(int timeoutMs = 2000);

    /** Renders the next block exactly like the live callback would. Real-time safe. */
    void renderOffline(float* const* outputs, int numOutputs, int numSamples) noexcept;

    void endOfflineRender();

    /** While rendering offline: sets the transport directly (the offline renderer owns it). With
        `ignoreLoop`, the loop range is ignored until the next call. */
    void setOfflineTransport(double positionSeconds, bool playing, bool ignoreLoop) noexcept;

    /** While rendering offline: applies posted commands now (e.g. to stop the transport before
        handing back to the live callback). Call from the thread that owns the offline render. */
    void applyCommandsOffline() noexcept;

    /** While rendering offline: a track slot's output (post-fader, latency-compensated) of the last
        rendered block, for stem exports. */
    [[nodiscard]] const juce::AudioBuffer<Sample>& getSlotOutput(int slot) const noexcept
    {
        return slots_[static_cast<std::size_t>(std::clamp(slot, 0, kMaxTracks - 1))].main;
    }

    //==============================================================================================
    // Audio thread

    void process(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                 int numSamples) noexcept;

private:
    using Clock = std::chrono::steady_clock;

    struct SlotState
    {
        std::uint32_t trackId = 0;
        juce::SmoothedValue<Sample, juce::ValueSmoothingTypes::Linear> gainLeft;
        juce::SmoothedValue<Sample, juce::ValueSmoothingTypes::Linear> gainRight;
        juce::AudioBuffer<Sample> main; ///< working buffer; holds the post-fader output after processing
        juce::AudioBuffer<Sample> pre;  ///< pre-fader copy (when a pre-fader send needs it)
        juce::MidiBuffer midi;
        juce::AudioBuffer<Sample> delay; ///< latency-compensation ring
        int delayWrite = 0;
        int currentDelay = 0;
        bool panic = false; ///< send all-notes-off before the next block
    };

    class PlayHead final : public juce::AudioPlayHead
    {
    public:
        juce::Optional<PositionInfo> getPosition() const override { return info; }
        PositionInfo info;
    };

    void renderBlock(const float* const* inputs, int numInputs, float* const* outputs, int numOutputs,
                     int numSamples) noexcept;
    void renderSubBlock(int inputOffset, float* const* outputs, int numOutputs, int offset,
                        int numSamples) noexcept;

    void applyPendingCommands() noexcept;
    void handle(const command::SetPlaying& cmd) noexcept;
    void handle(const command::Locate& cmd) noexcept;
    void handle(const command::SetMasterGain& cmd) noexcept;
    void handle(const command::SetTestTone& cmd) noexcept;
    void handle(const command::SetLoop& cmd) noexcept;
    void handle(const command::SetSpeed& cmd) noexcept;
    void handle(const command::SwapGraph& cmd) noexcept;
    void handle(const command::StartCapture& cmd) noexcept;
    void handle(const command::StopCapture& cmd) noexcept;
    void retire(RenderGraph* graph) noexcept;
    void panicAll() noexcept;

    static void processTrackTask(void* context, int taskIndex) noexcept;
    void processTrack(int graphIndex) noexcept;
    void renderAudioClips(const RenderTrack& track, SlotState& slot) noexcept;
    void renderMidiClips(const RenderTrack& track, SlotState& slot) noexcept;
    void addMonitoredInput(const RenderTrack& track, SlotState& slot) noexcept;
    void sumBusSources(const RenderTrack& track, SlotState& slot) noexcept;
    void applyFader(const RenderTrack& track, SlotState& slot) noexcept;
    void applyCompensationDelay(const RenderTrack& track, SlotState& slot) noexcept;
    void applyParameterAutomation(const std::vector<ParameterLane>& lanes) noexcept;

    void renderMaster(int numSamples) noexcept;
    void writeOutputsAndMeter(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept;
    void captureInputs(int inputOffset, int numSamples) noexcept;
    void publishLoad(Clock::duration elapsed, int numSamples) noexcept;
    void updatePlayHead() noexcept;

    // --- Cross-thread channels -------------------------------------------------------------------
    rt::SpscQueue<EngineCommand> commands_{kCommandQueueCapacity};
    rt::SpscQueue<RenderGraph*> retiredGraphs_{64};
    EngineTelemetry telemetry_;
    rt::ThreadOwnershipChecker commandProducer_;
    LiveMidiQueue liveMidi_;
    WorkerPool workers_;

    std::atomic<bool> prepared_{false};
    std::atomic<double> publishedSampleRate_{0.0};
    std::atomic<int> publishedBlockSize_{0};
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<bool> offlineRequested_{false};
    std::atomic<bool> offlineActive_{false};
    std::atomic<bool> liveCallbackActive_{false};

    // --- Configuration: written by prepare(), read-only while rendering ---------------------------
    double sampleRate_ = 0.0;
    int maxBlockSize_ = 0;
    int numChannels_ = 0;
    int numInputs_ = 0;
    juce::AudioBuffer<Sample> masterBus_;
    juce::AudioBuffer<Sample> inputBus_; ///< device inputs as 64-bit, for monitoring and capture
    juce::MidiBuffer masterMidi_;
    std::vector<SlotState> slots_;

    // --- Audio-thread state ----------------------------------------------------------------------
    double positionSeconds_ = 0.0;
    double speed_ = 1.0;
    bool playing_ = false;
    bool ignoreLoop_ = false; ///< offline exports render straight through the loop range
    bool loopEnabled_ = false;
    double loopStart_ = 0.0;
    double loopEnd_ = 0.0;
    double masterGainTarget_ = 1.0;
    juce::SmoothedValue<Sample, juce::ValueSmoothingTypes::Linear> masterGain_;
    dsp::TestToneGenerator testTone_;
    double smoothedLoad_ = 0.0;
    RenderGraph* graph_ = nullptr; // owned; replaced only via command::SwapGraph
    CaptureSession* capture_ = nullptr;
    juce::MidiBuffer liveBlock_; ///< live MIDI drained this callback (events at offset 0)
    bool liveBlockPending_ = false;
    PlayHead playHead_;

    // Per-sub-block context read by the worker tasks (written before they are released).
    const std::vector<int>* currentLevel_ = nullptr;
    int currentNumSamples_ = 0;
    int currentInputOffset_ = 0;
    double blockStartSeconds_ = 0.0;
    double secondsPerSample_ = 0.0;

    std::array<RenderGraph*, 16> retireBacklog_{};
    std::size_t retireBacklogSize_ = 0;
};

} // namespace dojo::engine

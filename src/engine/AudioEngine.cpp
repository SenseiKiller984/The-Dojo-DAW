#include "engine/AudioEngine.h"

#include "core/rt/AtomicUtils.h"
#include "core/rt/RealtimeGuard.h"
#include "engine/dsp/ClipRenderer.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace dojo::engine
{
namespace
{
constexpr double kMasterGainRampSeconds = 0.050;
constexpr double kTrackGainRampSeconds = 0.030;
constexpr double kLoadSmoothingCoefficient = 0.1; // one-pole, ~10-block time constant

void clearOutputs(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill_n(outputs[ch] + offset, numSamples, 0.0f);
}
} // namespace

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine()
{
    // By contract the audio device has stopped, so this (message) thread may now act as the consumer
    // of the command queue and reclaim every graph the engine still owns.
    EngineCommand command;
    while (commands_.tryPop(command))
        if (const auto* swap = std::get_if<command::SwapGraph>(&command))
            delete swap->graph;

    releaseRetiredGraphs();

    for (std::size_t i = 0; i < retireBacklogSize_; ++i)
        delete retireBacklog_[i];

    delete graph_;
}

//==================================================================================================
// Lifecycle

void AudioEngine::prepare(double sampleRate, int maximumBlockSize, int numOutputChannels)
{
    jassert(sampleRate > 0.0 && maximumBlockSize > 0 && numOutputChannels >= 0);

    prepared_.store(false, std::memory_order_release);

    // Keep the playhead at the same *time* if the device's sample rate changed.
    if (sampleRate_ > 0.0 && !juce::exactlyEqual(sampleRate_, sampleRate))
        playhead_ =
            static_cast<SampleCount>(std::llround(static_cast<double>(playhead_) * sampleRate / sampleRate_));

    sampleRate_ = sampleRate;
    maxBlockSize_ = std::max(1, maximumBlockSize);
    numChannels_ = std::max(0, numOutputChannels);

    // The only allocation in the engine's life cycle — done here, never on the audio thread.
    masterBus_.setSize(std::max(1, numChannels_), maxBlockSize_, false, true, false);
    trackBus_.setSize(2, maxBlockSize_, false, true, false);

    for (auto& track : trackRuntime_)
    {
        track.gainLeft.reset(sampleRate_, kTrackGainRampSeconds);
        track.gainRight.reset(sampleRate_, kTrackGainRampSeconds);
    }

    masterGain_.reset(sampleRate_, kMasterGainRampSeconds);
    masterGain_.setCurrentAndTargetValue(masterGainTarget_);
    testTone_.prepare(sampleRate_);
    smoothedLoad_ = 0.0;

    publishedSampleRate_.store(sampleRate_, std::memory_order_relaxed);
    prepared_.store(true, std::memory_order_release);
}

void AudioEngine::release() noexcept
{
    prepared_.store(false, std::memory_order_release);
    telemetry_.playing.store(false, std::memory_order_relaxed);
}

//==================================================================================================
// Message thread

bool AudioEngine::post(const EngineCommand& command) noexcept
{
    // Single-producer queue: every post must come from the same (message) thread.
    jassert(commandProducer_.isOwnedByCallingThread());
    return commands_.tryPush(command);
}

TelemetrySnapshot AudioEngine::collectTelemetry() noexcept
{
    TelemetrySnapshot snapshot;
    snapshot.masterPeakLeft = telemetry_.masterPeakLeft.exchange(0.0f, std::memory_order_acq_rel);
    snapshot.masterPeakRight = telemetry_.masterPeakRight.exchange(0.0f, std::memory_order_acq_rel);
    snapshot.dspLoad = telemetry_.dspLoad.load(std::memory_order_relaxed);
    snapshot.dspLoadPeak = telemetry_.dspLoadPeak.exchange(0.0f, std::memory_order_acq_rel);
    snapshot.playheadSamples = telemetry_.playheadSamples.load(std::memory_order_relaxed);
    snapshot.playing = telemetry_.playing.load(std::memory_order_relaxed);
    snapshot.processedBlocks = telemetry_.processedBlocks.load(std::memory_order_relaxed);
    snapshot.clippedSamples = telemetry_.clippedSamples.load(std::memory_order_relaxed);
    return snapshot;
}

StereoPeak AudioEngine::collectTrackPeak(int slot) noexcept
{
    if (slot < 0 || slot >= kMaxTracks)
        return {};

    const auto index = static_cast<std::size_t>(slot) * 2;
    return {telemetry_.trackPeaks[index].exchange(0.0f, std::memory_order_acq_rel),
            telemetry_.trackPeaks[index + 1].exchange(0.0f, std::memory_order_acq_rel)};
}

void AudioEngine::releaseRetiredGraphs() noexcept
{
    RenderGraph* graph = nullptr;
    while (retiredGraphs_.tryPop(graph))
        delete graph;
}

//==================================================================================================
// Audio thread — THE AUDIO CALLBACK ENTRY POINT.
//
// Everything reachable from here must be real-time safe: no allocation/free, no locks, no blocking
// I/O, no logging, no UI calls, no unbounded loops.

void AudioEngine::process(const float* const* /*inputs*/, int /*numInputs*/, float* const* outputs,
                          int numOutputs, int numSamples) noexcept
{
    const rt::ScopedRealtimeSection realtimeSection; // arms the allocation trap for this thread
    const juce::ScopedNoDenormals noDenormals;       // FTZ/DAZ: denormals can cost 100x CPU
    const auto callbackStart = Clock::now();

    if (numSamples <= 0)
        return;

    if (!prepared_.load(std::memory_order_acquire))
    {
        clearOutputs(outputs, numOutputs, 0, numSamples);
        return;
    }

    applyPendingCommands();

    // Hand back any retired graphs that did not fit in the return queue last time.
    while (retireBacklogSize_ > 0 && retiredGraphs_.tryPush(retireBacklog_[retireBacklogSize_ - 1]))
        --retireBacklogSize_;

    // Drivers may (rarely) deliver more frames than they announced; never overrun our buffers.
    for (int offset = 0; offset < numSamples;)
    {
        const int subBlock = std::min(numSamples - offset, maxBlockSize_);
        renderSubBlock(outputs, numOutputs, offset, subBlock);
        offset += subBlock;
    }

    telemetry_.playheadSamples.store(playhead_, std::memory_order_relaxed);
    telemetry_.playing.store(playing_, std::memory_order_relaxed);
    telemetry_.processedBlocks.fetch_add(1, std::memory_order_relaxed);

    publishLoad(Clock::now() - callbackStart, numSamples);
}

void AudioEngine::applyPendingCommands() noexcept
{
    // Bounded: at most one queue's worth of commands per block.
    commands_.drain([this](const EngineCommand& command) noexcept
                    { std::visit([this](const auto& cmd) noexcept { handle(cmd); }, command); },
                    commands_.capacity());
}

void AudioEngine::handle(const command::SetPlaying& cmd) noexcept
{
    playing_ = cmd.shouldPlay;
}

void AudioEngine::handle(const command::Locate& cmd) noexcept
{
    playhead_ = std::max<SampleCount>(0, cmd.positionSamples);
}

void AudioEngine::handle(const command::SetMasterGain& cmd) noexcept
{
    masterGainTarget_ = std::clamp(cmd.linearGain, 0.0, 4.0); // hard ceiling: +12 dB
    masterGain_.setTargetValue(masterGainTarget_);
}

void AudioEngine::handle(const command::SetTestTone& cmd) noexcept
{
    testTone_.setParameters(cmd.enabled, cmd.frequencyHz, cmd.linearGain);
}

void AudioEngine::handle(const command::SwapGraph& cmd) noexcept
{
    RenderGraph* const previous = graph_;
    graph_ = cmd.graph;

    if (graph_ != nullptr)
    {
        masterGainTarget_ = std::clamp(graph_->masterGain, 0.0, 4.0);
        masterGain_.setTargetValue(masterGainTarget_);
        telemetry_.appliedGraphSerial.store(graph_->serial, std::memory_order_release);
    }

    retire(previous);
}

void AudioEngine::retire(RenderGraph* graph) noexcept
{
    if (graph == nullptr)
        return;

    if (retiredGraphs_.tryPush(graph))
        return;

    if (retireBacklogSize_ < retireBacklog_.size())
        retireBacklog_[retireBacklogSize_++] = graph;
    // else: unreachable in practice (EngineController keeps at most one graph in flight). Leaking is
    // the only real-time-safe option left, and it is preferable to freeing memory on this thread.
}

void AudioEngine::renderTracks(int numSamples) noexcept
{
    if (graph_ == nullptr || numChannels_ <= 0)
        return;

    const double blockStartSeconds = static_cast<double>(playhead_) / sampleRate_;
    Sample* const trackLeft = trackBus_.getWritePointer(0);
    Sample* const trackRight = trackBus_.getWritePointer(1);
    Sample* const masterLeft = masterBus_.getWritePointer(0);
    Sample* const masterRight = numChannels_ > 1 ? masterBus_.getWritePointer(1) : nullptr;

    for (const auto& track : graph_->tracks)
    {
        if (track.slot < 0 || track.slot >= kMaxTracks)
            continue;

        auto& runtime = trackRuntime_[static_cast<std::size_t>(track.slot)];

        if (runtime.trackId != track.trackId)
        {
            // A different track now owns this slot: start from its current gains, no ramp.
            runtime.trackId = track.trackId;
            runtime.gainLeft.setCurrentAndTargetValue(track.gainLeft);
            runtime.gainRight.setCurrentAndTargetValue(track.gainRight);
        }
        else
        {
            runtime.gainLeft.setTargetValue(track.gainLeft);
            runtime.gainRight.setTargetValue(track.gainRight);
        }

        bool hasAudio = false;

        if (playing_)
        {
            for (const auto& clip : track.clips)
            {
                if (clip.startSeconds * sampleRate_ >= static_cast<double>(playhead_ + numSamples))
                    break; // clips are sorted by start: nothing later can overlap this block

                if (!hasAudio && clip.endSeconds > blockStartSeconds)
                {
                    std::fill_n(trackLeft, numSamples, 0.0);
                    std::fill_n(trackRight, numSamples, 0.0);
                }

                if (clip.endSeconds > blockStartSeconds)
                    hasAudio = dsp::renderClip(clip, blockStartSeconds, sampleRate_, numSamples, trackLeft,
                                               trackRight) ||
                               hasAudio;
            }
        }

        if (!hasAudio)
        {
            runtime.gainLeft.skip(numSamples);
            runtime.gainRight.skip(numSamples);
            continue;
        }

        Sample peakLeft = 0.0;
        Sample peakRight = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const Sample l = trackLeft[i] * runtime.gainLeft.getNextValue();
            const Sample r = trackRight[i] * runtime.gainRight.getNextValue();
            peakLeft = std::max(peakLeft, std::abs(l));
            peakRight = std::max(peakRight, std::abs(r));

            if (masterRight != nullptr)
            {
                masterLeft[i] += l;
                masterRight[i] += r;
            }
            else
            {
                masterLeft[i] += 0.5 * (l + r); // mono device
            }
        }

        const auto meterIndex = static_cast<std::size_t>(track.slot) * 2;
        rt::atomicStoreMax(telemetry_.trackPeaks[meterIndex], static_cast<float>(peakLeft));
        rt::atomicStoreMax(telemetry_.trackPeaks[meterIndex + 1], static_cast<float>(peakRight));
    }
}

void AudioEngine::renderSubBlock(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    // 1. Start from silence on the 64-bit master bus.
    masterBus_.clear(0, numSamples);

    // 2. Sources: every track in the current render graph, then the diagnostic test tone.
    renderTracks(numSamples);
    testTone_.addTo(masterBus_, numSamples);

    // 3. Master section.
    applyMasterGain(numSamples);

    // 4. Down-convert to the driver's 32-bit float buffers, metering on the way out.
    writeOutputsAndMeter(outputs, numOutputs, offset, numSamples);

    if (playing_)
        playhead_ += numSamples;
}

void AudioEngine::applyMasterGain(int numSamples) noexcept
{
    if (!masterGain_.isSmoothing())
    {
        masterBus_.applyGain(0, numSamples, masterGain_.getTargetValue()); // no-op when silent/unity
        return;
    }

    if (masterBus_.hasBeenCleared())
    {
        masterGain_.skip(numSamples);
        return;
    }

    auto* const* channels = masterBus_.getArrayOfWritePointers();
    const int numChannels = masterBus_.getNumChannels();

    for (int i = 0; i < numSamples; ++i)
    {
        const Sample gain = masterGain_.getNextValue();

        for (int ch = 0; ch < numChannels; ++ch)
            channels[ch][i] *= gain;
    }
}

void AudioEngine::writeOutputsAndMeter(float* const* outputs, int numOutputs, int offset,
                                       int numSamples) noexcept
{
    Sample channelPeaks[2] = {0.0, 0.0};
    std::uint64_t clipped = 0;

    for (int ch = 0; ch < numOutputs; ++ch)
    {
        float* const destination = outputs[ch];

        if (destination == nullptr)
            continue;

        if (ch >= numChannels_)
        {
            std::fill_n(destination + offset, numSamples, 0.0f);
            continue;
        }

        const Sample* const source = masterBus_.getReadPointer(ch);
        Sample peak = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const Sample value = source[i];
            const Sample magnitude = std::abs(value);
            peak = std::max(peak, magnitude);
            clipped += magnitude > 1.0 ? 1u : 0u;
            destination[offset + i] = static_cast<IoSample>(value);
        }

        if (ch < 2)
            channelPeaks[ch] = peak;
    }

    if (numChannels_ == 1)
        channelPeaks[1] = channelPeaks[0]; // mono device: show the same level on both meters

    rt::atomicStoreMax(telemetry_.masterPeakLeft, static_cast<float>(channelPeaks[0]));
    rt::atomicStoreMax(telemetry_.masterPeakRight, static_cast<float>(channelPeaks[1]));

    if (clipped > 0)
        telemetry_.clippedSamples.fetch_add(clipped, std::memory_order_relaxed);
}

void AudioEngine::publishLoad(Clock::duration elapsed, int numSamples) noexcept
{
    const double periodSeconds = static_cast<double>(numSamples) / sampleRate_;
    const double elapsedSeconds = std::chrono::duration<double>(elapsed).count();
    const double instantaneousLoad = elapsedSeconds / periodSeconds;

    smoothedLoad_ += kLoadSmoothingCoefficient * (instantaneousLoad - smoothedLoad_);

    telemetry_.dspLoad.store(static_cast<float>(smoothedLoad_), std::memory_order_relaxed);
    rt::atomicStoreMax(telemetry_.dspLoadPeak, static_cast<float>(instantaneousLoad));
}

} // namespace dojo::engine

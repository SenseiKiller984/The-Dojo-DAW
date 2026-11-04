#include "engine/AudioEngine.h"

#include "core/rt/AtomicUtils.h"
#include "core/rt/RealtimeGuard.h"

#include <algorithm>
#include <cmath>
#include <variant>

namespace dojo::engine
{
namespace
{
constexpr double kMasterGainRampSeconds = 0.050;
constexpr double kLoadSmoothingCoefficient = 0.1; // one-pole, ~10-block time constant

void clearOutputs(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    for (int ch = 0; ch < numOutputs; ++ch)
        if (outputs[ch] != nullptr)
            std::fill_n(outputs[ch] + offset, numSamples, 0.0f);
}
} // namespace

AudioEngine::AudioEngine() = default;
AudioEngine::~AudioEngine() = default;

//==================================================================================================
// Lifecycle

void AudioEngine::prepare(double sampleRate, int maximumBlockSize, int numOutputChannels)
{
    jassert(sampleRate > 0.0 && maximumBlockSize > 0 && numOutputChannels >= 0);

    prepared_.store(false, std::memory_order_release);

    sampleRate_ = sampleRate;
    maxBlockSize_ = std::max(1, maximumBlockSize);
    numChannels_ = std::max(0, numOutputChannels);

    // The only allocation in the engine's life cycle — done here, never on the audio thread.
    masterBus_.setSize(std::max(1, numChannels_), maxBlockSize_, false, true, false);

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

void AudioEngine::renderSubBlock(float* const* outputs, int numOutputs, int offset, int numSamples) noexcept
{
    // 1. Start from silence on the 64-bit master bus.
    masterBus_.clear(0, numSamples);

    // 2. Sources. (Later phases: the track/bus graph renders here, fanned out across the worker pool.)
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

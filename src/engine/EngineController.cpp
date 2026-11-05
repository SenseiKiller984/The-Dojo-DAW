#include "engine/EngineController.h"

#include "engine/AudioEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace dojo::engine
{
namespace
{
constexpr int kHousekeepingIntervalMs = 33;
} // namespace

EngineController::EngineController(AudioEngine& engine, AudioDeviceHost& deviceHost)
    : engine_(engine)
    , deviceHost_(deviceHost)
    , releasePool_(engine)
{
    startTimer(kHousekeepingIntervalMs);
}

EngineController::~EngineController()
{
    stopTimer();
}

//==================================================================================================
// Transport

void EngineController::play()
{
    send(command::SetPlaying{true});
}

void EngineController::stop()
{
    send(command::SetPlaying{false});
}

void EngineController::togglePlayback()
{
    // The audio thread's state is the source of truth for what is actually playing.
    send(command::SetPlaying{!lastTelemetry_.playing});
}

void EngineController::returnToStart()
{
    locate(0);
}

void EngineController::locate(SampleCount positionSamples)
{
    send(command::Locate{positionSamples});
}

void EngineController::locateSeconds(double seconds)
{
    const double rate = getSampleRateOr(48000.0);
    locate(static_cast<SampleCount>(std::llround(std::max(0.0, seconds) * rate)));
}

void EngineController::setLoop(bool enabled, double startSeconds, double endSeconds)
{
    send(command::SetLoop{enabled, startSeconds, endSeconds});
}

void EngineController::setSpeed(double rate)
{
    send(command::SetSpeed{rate});
}

void EngineController::startCapture(CaptureSession* session)
{
    command::StartCapture start;
    start.session = session;
    send(start);
}

void EngineController::stopCapture()
{
    send(command::StopCapture{});
}

bool EngineController::isCaptureActive() const noexcept
{
    return engine_.isCaptureActive();
}

//==================================================================================================
// Mixer / diagnostics

void EngineController::setMasterGainDecibels(double decibels)
{
    const double linear = juce::Decibels::decibelsToGain(decibels, kMinusInfinityDb);
    send(command::SetMasterGain{linear});
}

void EngineController::setTestToneEnabled(bool enabled)
{
    testToneEnabled_ = enabled;
    command::SetTestTone tone;
    tone.enabled = enabled;
    send(tone);
}

//==================================================================================================
// Status

const TelemetrySnapshot& EngineController::pollTelemetry() noexcept
{
    lastTelemetry_ = engine_.collectTelemetry();
    return lastTelemetry_;
}

StereoPeak EngineController::collectTrackPeak(int slot) noexcept
{
    return engine_.collectTrackPeak(slot);
}

double EngineController::getSampleRateOr(double fallback) const noexcept
{
    const double rate = engine_.getSampleRate();
    return rate > 0.0 ? rate : fallback;
}

//==================================================================================================
// Command delivery

void EngineController::send(const EngineCommand& command)
{
    JUCE_ASSERT_MESSAGE_THREAD

    // Preserve FIFO order: once anything is parked, everything queues behind it.
    if (outbox_.empty() && engine_.post(command))
        return;

    if (!outbox_.empty() && canCoalesce(outbox_.back(), command))
        outbox_.back() = command;
    else
        outbox_.push_back(command);
}

void EngineController::flushOutbox()
{
    while (!outbox_.empty() && engine_.post(outbox_.front()))
        outbox_.pop_front();
}

void EngineController::publishGraph(std::unique_ptr<RenderGraph> graph)
{
    JUCE_ASSERT_MESSAGE_THREAD

    if (graph == nullptr)
        return;

    graph->serial = nextGraphSerial_++;
    pendingGraph_ = std::move(graph); // any older, never-posted graph is destroyed here
    pumpGraphs();
}

void EngineController::pumpGraphs()
{
    if (pendingGraph_ == nullptr)
        return;

    // Keep at most one graph in the queue: wait until the audio thread has picked up the last one.
    if (inFlightGraphSerial_ != 0 && engine_.getAppliedGraphSerial() < inFlightGraphSerial_)
        return;

    command::SwapGraph swap;
    swap.graph = pendingGraph_.get();

    if (engine_.post(swap))
    {
        inFlightGraphSerial_ = pendingGraph_->serial;
        (void)pendingGraph_.release(); // ownership now travels with the command
    }
}

void EngineController::timerCallback()
{
    engine_.releaseRetiredGraphs();
    releasePool_.collect();
    flushOutbox();
    pumpGraphs();
}

} // namespace dojo::engine

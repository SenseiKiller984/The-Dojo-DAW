#include "engine/EngineController.h"

#include "engine/AudioEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace dojo::engine
{
namespace
{
constexpr int kOutboxRetryIntervalMs = 20;
} // namespace

EngineController::EngineController(AudioEngine& engine, AudioDeviceHost& deviceHost)
    : engine_(engine)
    , deviceHost_(deviceHost)
{
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

    if (!isTimerRunning())
        startTimer(kOutboxRetryIntervalMs);
}

void EngineController::flushOutbox()
{
    while (!outbox_.empty() && engine_.post(outbox_.front()))
        outbox_.pop_front();

    if (outbox_.empty())
        stopTimer();
}

void EngineController::timerCallback()
{
    flushOutbox();
}

} // namespace dojo::engine

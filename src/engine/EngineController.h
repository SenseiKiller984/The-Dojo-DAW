#pragma once

#include "core/Types.h"
#include "engine/EngineCommands.h"
#include "engine/EngineTelemetry.h"
#include "engine/io/AudioDeviceHost.h"

#include <juce_events/juce_events.h>

#include <deque>

namespace dojo::engine
{

class AudioEngine;

/**
    The UI's single, message-thread-only façade onto the engine.

    UI components never touch AudioEngine directly: they call intent-level methods here, which are
    translated into EngineCommands and pushed through the lock-free command queue.

    Back-pressure: the command queue is bounded (it must be, to be allocation free). If it is ever
    full — realistically only while no audio device is running, so nothing drains it — commands are
    parked in a message-thread outbox (where allocation is fine) and retried on a timer in FIFO order.
    Consecutive coalescable commands of the same type are merged there, so the outbox stays tiny and
    the latest value always wins. Nothing is ever silently dropped.
*/
class EngineController final : private juce::Timer
{
public:
    EngineController(AudioEngine& engine, AudioDeviceHost& deviceHost);
    ~EngineController() override;

    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    // --- Transport ---------------------------------------------------------------------------------
    void play();
    void stop();
    void togglePlayback();
    void returnToStart();
    void locate(SampleCount positionSamples);

    // --- Mixer ---------------------------------------------------------------------------------------
    /** Values at or below kMinusInfinityDb mute the output. */
    void setMasterGainDecibels(double decibels);
    static constexpr double kMinusInfinityDb = -60.0;

    // --- Diagnostics -----------------------------------------------------------------------------
    void setTestToneEnabled(bool enabled);
    [[nodiscard]] bool isTestToneEnabled() const noexcept { return testToneEnabled_; }

    // --- Status ------------------------------------------------------------------------------------
    /** Collects fresh telemetry from the engine. Call exactly once per UI frame. */
    const TelemetrySnapshot& pollTelemetry() noexcept;

    /** The snapshot taken by the most recent pollTelemetry(). */
    [[nodiscard]] const TelemetrySnapshot& getLastTelemetry() const noexcept { return lastTelemetry_; }

    [[nodiscard]] AudioDeviceHost::Status getDeviceStatus() const { return deviceHost_.getStatus(); }

    /** The engine's sample rate, or `fallback` while no device is running. */
    [[nodiscard]] double getSampleRateOr(double fallback) const noexcept;

    /** Commands waiting in the outbox because the queue was full (diagnostics). */
    [[nodiscard]] std::size_t getParkedCommandCount() const noexcept { return outbox_.size(); }

private:
    void send(const EngineCommand& command);
    void flushOutbox();
    void timerCallback() override;

    AudioEngine& engine_;
    AudioDeviceHost& deviceHost_;
    std::deque<EngineCommand> outbox_;
    TelemetrySnapshot lastTelemetry_;
    bool testToneEnabled_ = false;
};

} // namespace dojo::engine

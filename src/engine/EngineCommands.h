#pragma once

#include "core/Types.h"
#include "engine/RenderGraph.h"

#include <type_traits>
#include <variant>

namespace dojo::engine
{

struct CaptureSession;

/**
    The UI -> audio protocol.

    Commands are the ONLY way the message thread may change state owned by the audio thread. They
    are small, trivially copyable value types, posted through a lock-free SPSC queue and applied by
    the audio thread at the start of its next processing block.

    Rules for adding a command:
      1. It must be trivially copyable (enforced below) — no std::string, std::vector, shared_ptr…
      2. If it needs to hand the audio thread a large object (a rebuilt RenderGraph, a capture
         session), it carries a raw pointer to an object fully constructed on the message thread.
         Replaced objects are handed back (return queue / acknowledgement) and destroyed on the
         message thread, so the audio thread never frees memory.
      3. Handling it on the audio thread must be bounded-time, allocation free and lock free.
      4. It must declare `static constexpr bool coalescable`: true if it is an absolute "setter" for
         which only the most recent value matters. EngineController uses this to merge consecutive
         commands of the same type if the queue is ever full (e.g. while no audio device is running).
*/
namespace command
{
/** Start or stop the transport. */
struct SetPlaying
{
    static constexpr bool coalescable = true;
    bool shouldPlay = false;
};

/** Move the playhead to an absolute timeline position. */
struct Locate
{
    static constexpr bool coalescable = true;
    SampleCount positionSamples = 0;
};

/** Master output gain (linear), when no graph sets it. Smoothed on the audio thread. */
struct SetMasterGain
{
    static constexpr bool coalescable = true;
    double linearGain = 1.0;
};

/** Diagnostic sine generator that feeds the master bus — used to verify a device/driver setup. */
struct SetTestTone
{
    static constexpr bool coalescable = true;
    bool enabled = false;
    double frequencyHz = 440.0;
    double linearGain = 0.125;
};

/** Loop playback between two timeline positions (seconds). */
struct SetLoop
{
    static constexpr bool coalescable = true;
    bool enabled = false;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
};

/** Stop playback when it reaches the end of the arrangement (the end of the last clip). Ignored
    while a loop will wrap, while capturing (recording, real-time export) and in offline renders. */
struct SetStopAtEnd
{
    static constexpr bool coalescable = true;
    bool enabled = false;
};

/** Global varispeed: the timeline runs at `rate` x real time (0.25 .. 4). */
struct SetSpeed
{
    static constexpr bool coalescable = true;
    double rate = 1.0;
};

/** Replace the render graph. Ownership of `graph` passes to the engine; the graph it replaces comes
    back through AudioEngine::releaseRetiredGraphs(). Never coalesced: EngineController keeps at most
    one graph in flight and merges newer ones itself. */
struct SwapGraph
{
    static constexpr bool coalescable = false;
    RenderGraph* graph = nullptr;
};

/** Start writing inputs / track outputs / the master into the session's capture streams (recording
    or a real-time bounce). The session stays owned by the message thread; the audio thread stops
    touching it after StopCapture is acknowledged (see TelemetrySnapshot::captureActive). */
struct StartCapture
{
    static constexpr bool coalescable = false;
    CaptureSession* session = nullptr;
};

struct StopCapture
{
    static constexpr bool coalescable = false;
};
} // namespace command

using EngineCommand =
    std::variant<command::SetPlaying, command::Locate, command::SetMasterGain, command::SetTestTone,
                 command::SetLoop, command::SetSpeed, command::SwapGraph, command::StartCapture, command::StopCapture,
                 command::SetStopAtEnd>;

static_assert(std::is_trivially_copyable_v<EngineCommand>,
              "EngineCommand must stay trivially copyable to travel through the lock-free command queue");

/** True if `newer` may replace `older` without changing the outcome (same type, absolute setter). */
[[nodiscard]] inline bool canCoalesce(const EngineCommand& older, const EngineCommand& newer) noexcept
{
    if (older.index() != newer.index())
        return false;

    return std::visit([](const auto& c) noexcept { return std::decay_t<decltype(c)>::coalescable; }, newer);
}

} // namespace dojo::engine

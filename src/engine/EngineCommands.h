#pragma once

#include "core/Types.h"

#include <type_traits>
#include <variant>

namespace dojo::engine
{

/**
    The UI -> audio protocol.

    Commands are the ONLY way the message thread may change state owned by the audio thread. They
    are small, trivially copyable value types, posted through a lock-free SPSC queue and applied by
    the audio thread at the start of its next processing block (sample-accurate scheduling will be
    layered on top in later phases).

    Rules for adding a command:
      1. It must be trivially copyable (enforced below) — no std::string, std::vector, shared_ptr…
      2. If it needs to hand the audio thread a large object (e.g. a rebuilt processing graph), it
         carries a raw pointer to an object fully constructed on the message thread. The object it
         replaces is sent back to the message thread through a lock-free return queue and destroyed
         there, so the audio thread never frees memory. (Introduced with the processing graph.)
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

/** Master output gain (linear). Smoothed on the audio thread to avoid zipper noise. */
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
} // namespace command

using EngineCommand =
    std::variant<command::SetPlaying, command::Locate, command::SetMasterGain, command::SetTestTone>;

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

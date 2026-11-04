#pragma once

#include "core/Types.h"

#include <atomic>
#include <cstdint>

namespace dojo::engine
{

/**
    Audio -> UI status channel.

    Continuous status values (levels, load, playhead) are published through individual lock-free
    atomics: the audio thread overwrites them every block and the UI samples them once per frame.
    Only the latest value matters, so a queue would be the wrong tool — it could overflow while the
    UI is busy and would deliver stale data.

    Peak-style values are "max since last read": the audio thread raises them with atomicStoreMax()
    and the UI collects-and-resets them with exchange(), so a one-sample transient is never lost
    between two UI frames.

    All members are written exclusively by the audio thread (single writer); the UI only reads or
    exchange()s them.
*/
struct EngineTelemetry
{
    std::atomic<float> masterPeakLeft{0.0f};  ///< Linear peak, max since last read.
    std::atomic<float> masterPeakRight{0.0f}; ///< Linear peak, max since last read.
    std::atomic<float> dspLoad{0.0f};         ///< Smoothed callback time / buffer period (1.0 == 100%).
    std::atomic<float> dspLoadPeak{0.0f};     ///< Max instantaneous load since last read.

    std::atomic<SampleCount> playheadSamples{0};
    std::atomic<bool> playing{false};

    std::atomic<std::uint64_t> processedBlocks{0};
    std::atomic<std::uint64_t> clippedSamples{0}; ///< Master samples that exceeded 0 dBFS.

    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<SampleCount>::is_always_lock_free);
    static_assert(std::atomic<bool>::is_always_lock_free);
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
};

/** A plain copy of the telemetry, taken on the message thread once per UI frame. */
struct TelemetrySnapshot
{
    float masterPeakLeft = 0.0f;
    float masterPeakRight = 0.0f;
    float dspLoad = 0.0f;
    float dspLoadPeak = 0.0f;
    SampleCount playheadSamples = 0;
    bool playing = false;
    std::uint64_t processedBlocks = 0;
    std::uint64_t clippedSamples = 0;
};

} // namespace dojo::engine

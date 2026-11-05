#pragma once

#include "core/Types.h"
#include "engine/RenderGraph.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace dojo::engine
{

inline constexpr int kMaxInputChannels = 32;

/**
    Audio -> UI status channel.

    Continuous status values (levels, load, playhead) are published through individual lock-free
    atomics: the audio thread overwrites them every block and the UI samples them once per frame.
    Only the latest value matters, so a queue would be the wrong tool — it could overflow while the
    UI is busy and would deliver stale data.

    Peak-style values are "max since last read": the audio thread raises them with atomicStoreMax()
    and the UI collects-and-resets them with exchange(), so a one-sample transient is never lost
    between two UI frames.

    Written by the audio thread (and its workers, for per-track peaks); the UI only reads or
    exchange()s them.
*/
struct EngineTelemetry
{
    std::atomic<float> masterPeakLeft{0.0f};  ///< Linear peak, max since last read.
    std::atomic<float> masterPeakRight{0.0f}; ///< Linear peak, max since last read.
    std::atomic<float> dspLoad{0.0f};         ///< Smoothed callback time / buffer period (1.0 == 100%).
    std::atomic<float> dspLoadPeak{0.0f};     ///< Max instantaneous load since last read.

    std::atomic<SampleCount> playheadSamples{0};
    std::atomic<double> playheadSeconds{0.0};
    std::atomic<bool> playing{false};

    std::atomic<std::uint64_t> processedBlocks{0};
    std::atomic<std::uint64_t> clippedSamples{0}; ///< Master samples that exceeded 0 dBFS.

    std::atomic<std::uint64_t> appliedGraphSerial{0}; ///< Serial of the RenderGraph now in use.

    std::atomic<bool> captureActive{false};      ///< the audio thread is writing a CaptureSession
    std::atomic<double> captureStartSeconds{-1.0}; ///< timeline time of the first captured sample
    std::atomic<std::uint32_t> captureStops{0}; ///< StopCapture commands handled so far

    /** Post-fader track peaks, [slot * 2 + channel], max since last read. */
    std::array<std::atomic<float>, static_cast<std::size_t>(kMaxTracks) * 2> trackPeaks{};

    /** Device input peaks per channel, max since last read. */
    std::array<std::atomic<float>, static_cast<std::size_t>(kMaxInputChannels)> inputPeaks{};

    static_assert(std::atomic<float>::is_always_lock_free);
    static_assert(std::atomic<double>::is_always_lock_free);
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
    double playheadSeconds = 0.0;
    bool playing = false;
    std::uint64_t processedBlocks = 0;
    std::uint64_t clippedSamples = 0;
    bool captureActive = false;
};

/** Linear left/right peaks of one track since the previous read. */
struct StereoPeak
{
    float left = 0.0f;
    float right = 0.0f;
};

} // namespace dojo::engine

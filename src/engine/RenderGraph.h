#pragma once

#include "core/Types.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace dojo::engine
{

/** Upper bound on simultaneously rendered tracks. Per-track engine state (gain smoothing, meters) lives
    in fixed arrays of this size so the audio thread never allocates. */
inline constexpr int kMaxTracks = 256;

/**
    One audio clip as the audio thread sees it: a window onto decoded sample data, placed on the
    timeline. Times are in seconds, so a graph stays valid across device sample-rate changes; the
    engine converts to samples per block and resamples on the fly when the source rate differs.
*/
struct RenderClip
{
    double startSeconds = 0.0;        ///< timeline position of the clip's first sample
    double endSeconds = 0.0;          ///< timeline position just past its last sample
    double sourceOffsetSeconds = 0.0; ///< source time that plays at startSeconds (slip editing)
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    double gain = 1.0;

    const float* const* channels = nullptr; ///< decoded audio, kept alive by RenderGraph::keepAlive
    int numChannels = 0;                    ///< 1 = mono (played on both sides), 2 = stereo
    SampleCount sourceLength = 0;           ///< frames available in `channels`
    double sourceSampleRate = 48000.0;
};

/** One track: its clips (sorted by start) and its final left/right gains, which already include
    volume, pan law, phase inversion and mute/solo, all resolved on the message thread. */
struct RenderTrack
{
    std::uint32_t trackId = 0; ///< stable identity, so smoothing state survives graph swaps
    int slot = 0;              ///< index into the engine's per-track state and meter arrays
    double gainLeft = 1.0;
    double gainRight = 1.0;
    std::vector<RenderClip> clips;
};

/**
    An immutable snapshot of everything the audio thread renders.

    Built on the message thread whenever the session changes, handed to the audio thread by pointer
    (command::SwapGraph) and never modified afterwards. The graph it replaces is returned through a
    lock-free queue and destroyed on the message thread, so the audio thread never frees memory.
*/
struct RenderGraph
{
    std::uint64_t serial = 0; ///< assigned by EngineController; echoed back via telemetry
    double masterGain = 1.0;
    std::vector<RenderTrack> tracks;
    std::vector<std::shared_ptr<const void>> keepAlive; ///< owners of the sample data clips point into
};

} // namespace dojo::engine

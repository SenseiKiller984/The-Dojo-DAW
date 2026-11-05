#pragma once

#include "core/Types.h"
#include "engine/RenderGraph.h"

namespace dojo::engine::dsp
{

/**
    Adds the part of `clip` that falls inside one block into a stereo 64-bit buffer.

    - `blockStartSeconds` is the timeline time of the block's first sample; `secondsPerSample` is the
      timeline time per output sample (1/sampleRate, scaled by the global varispeed).
    - Clips with a ClipStretcher (time-stretch / pitch) are rendered through it. Others are read
      directly: sources at a different rate are resampled with 4-point Hermite interpolation; matching
      rates take an exact copy path.
    - Every clip edge gets at least a 1 ms fade so that starting/stopping mid-waveform never clicks;
      longer user fades and crossfades (equal-power) take precedence.

    Returns true if any sample of the clip was rendered. Real-time safe: no allocation, no locks.
*/
bool renderClip(const RenderClip& clip, double blockStartSeconds, double secondsPerSample, int numSamples,
                Sample* left, Sample* right) noexcept;

/** Minimum fade applied at every clip edge to prevent clicks. */
inline constexpr double kMinimumEdgeFadeSeconds = 0.001;

/** Gain of a clip at `timeInClip` seconds from its start, from its fades. */
double clipEdgeGain(const RenderClip& clip, double timeInClip) noexcept;

} // namespace dojo::engine::dsp

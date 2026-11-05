#pragma once

#include "core/Types.h"
#include "engine/RenderGraph.h"

namespace dojo::engine::dsp
{

/**
    Adds the part of `clip` that falls inside one block into a stereo 64-bit buffer.

    - `blockStartSeconds` is the timeline time of the block's first sample.
    - Sources at a different sample rate than the device are resampled with 4-point Hermite
      interpolation; matching rates take an exact copy path.
    - Every clip edge gets at least a 1 ms fade so that starting/stopping mid-waveform never clicks;
      longer user fades (fadeIn/fadeOutSeconds) take precedence.

    Returns true if any sample of the clip was rendered. Real-time safe: no allocation, no locks.
*/
bool renderClip(const RenderClip& clip, double blockStartSeconds, double sampleRate, int numSamples,
                Sample* left, Sample* right) noexcept;

/** Minimum fade applied at every clip edge to prevent clicks. */
inline constexpr double kMinimumEdgeFadeSeconds = 0.001;

} // namespace dojo::engine::dsp

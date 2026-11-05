#include "engine/dsp/ClipRenderer.h"

#include <algorithm>
#include <cmath>

namespace dojo::engine::dsp
{
namespace
{
inline double sampleAt(const float* data, SampleCount length, SampleCount index) noexcept
{
    return (index >= 0 && index < length) ? static_cast<double>(data[index]) : 0.0;
}

/** 4-point, 3rd-order Hermite interpolation (x-form). */
inline double interpolate(const float* data, SampleCount length, double position) noexcept
{
    const double floorPosition = std::floor(position);
    const auto index = static_cast<SampleCount>(floorPosition);
    const double fraction = position - floorPosition;

    const double xm1 = sampleAt(data, length, index - 1);
    const double x0 = sampleAt(data, length, index);
    const double x1 = sampleAt(data, length, index + 1);
    const double x2 = sampleAt(data, length, index + 2);

    const double c = (x1 - xm1) * 0.5;
    const double v = x0 - x1;
    const double w = c + v;
    const double a = w + v + (x2 - x0) * 0.5;
    const double bNeg = w + a;

    return (((a * fraction) - bNeg) * fraction + c) * fraction + x0;
}

/** Gain from the clip's fades for a sample at `timeInClip` seconds after its start. */
inline double edgeGain(double timeInClip, double clipLength, double fadeIn, double fadeOut) noexcept
{
    double gain = 1.0;

    if (timeInClip < fadeIn)
        gain *= std::max(0.0, timeInClip / fadeIn);

    const double remaining = clipLength - timeInClip;
    if (remaining < fadeOut)
        gain *= std::max(0.0, remaining / fadeOut);

    return gain;
}
} // namespace

bool renderClip(const RenderClip& clip, double blockStartSeconds, double sampleRate, int numSamples,
                Sample* left, Sample* right) noexcept
{
    if (clip.channels == nullptr || clip.numChannels <= 0 || clip.sourceLength <= 0 || sampleRate <= 0.0 ||
        numSamples <= 0)
        return false;

    // Which samples of this block lie inside [start, end)?
    const double firstExact = (clip.startSeconds - blockStartSeconds) * sampleRate;
    const double endExact = (clip.endSeconds - blockStartSeconds) * sampleRate;
    const int first =
        static_cast<int>(std::clamp(std::ceil(firstExact - 1.0e-9), 0.0, static_cast<double>(numSamples)));
    const int last =
        static_cast<int>(std::clamp(std::ceil(endExact - 1.0e-9), 0.0, static_cast<double>(numSamples)));

    if (first >= last)
        return false;

    const double clipLength = clip.endSeconds - clip.startSeconds;
    const double fadeIn = std::max(clip.fadeInSeconds, kMinimumEdgeFadeSeconds);
    const double fadeOut = std::max(clip.fadeOutSeconds, kMinimumEdgeFadeSeconds);
    const double secondsPerSample = 1.0 / sampleRate;
    const double sourceStep = clip.sourceSampleRate / sampleRate;

    const float* const sourceLeft = clip.channels[0];
    const float* const sourceRight = clip.numChannels > 1 ? clip.channels[1] : clip.channels[0];

    const double firstTimeInClip = blockStartSeconds + first * secondsPerSample - clip.startSeconds;
    const double firstSourcePosition = (clip.sourceOffsetSeconds + firstTimeInClip) * clip.sourceSampleRate;

    // Exact path: same rate and the clip is aligned to whole source samples.
    const double roundedPosition = std::round(firstSourcePosition);
    const bool exact =
        std::abs(sourceStep - 1.0) < 1.0e-12 && std::abs(firstSourcePosition - roundedPosition) < 1.0e-6;

    for (int i = first; i < last; ++i)
    {
        const double timeInClip = firstTimeInClip + (i - first) * secondsPerSample;
        const double gain = clip.gain * edgeGain(timeInClip, clipLength, fadeIn, fadeOut);

        double l = 0.0;
        double r = 0.0;

        if (exact)
        {
            const auto index = static_cast<SampleCount>(roundedPosition) + (i - first);
            l = sampleAt(sourceLeft, clip.sourceLength, index);
            r = sampleAt(sourceRight, clip.sourceLength, index);
        }
        else
        {
            const double position = firstSourcePosition + (i - first) * sourceStep;
            l = interpolate(sourceLeft, clip.sourceLength, position);
            r = sourceRight == sourceLeft ? l : interpolate(sourceRight, clip.sourceLength, position);
        }

        left[i] += l * gain;
        right[i] += r * gain;
    }

    return true;
}

} // namespace dojo::engine::dsp

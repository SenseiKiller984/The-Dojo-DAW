#include "engine/dsp/ClipRenderer.h"

#include "engine/dsp/ClipStretcher.h"

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

bool renderStretchedClip(const RenderClip& clip, double blockStartSeconds, double secondsPerSample, int first,
                         int last, Sample* left, Sample* right) noexcept
{
    return clip.stretcher->render(clip, blockStartSeconds, secondsPerSample, first, last, left, right);
}

inline double streamedSampleAt(const StreamedAudio& audio, int channel, SampleCount index) noexcept
{
    return static_cast<double>(audio.sample(channel, index));
}

double interpolateStreamed(const StreamedAudio& audio, int channel, double position) noexcept
{
    const double floorPosition = std::floor(position);
    const auto index = static_cast<SampleCount>(floorPosition);
    const double fraction = position - floorPosition;

    const double xm1 = streamedSampleAt(audio, channel, index - 1);
    const double x0 = streamedSampleAt(audio, channel, index);
    const double x1 = streamedSampleAt(audio, channel, index + 1);
    const double x2 = streamedSampleAt(audio, channel, index + 2);

    const double c = (x1 - xm1) * 0.5;
    const double v = x0 - x1;
    const double w = c + v;
    const double a = w + v + (x2 - x0) * 0.5;
    const double bNeg = w + a;
    return (((a * fraction) - bNeg) * fraction + c) * fraction + x0;
}

bool renderStreamedClip(const RenderClip& clip, double blockStartSeconds, double secondsPerSample, int first,
                        int last, Sample* left, Sample* right) noexcept
{
    const auto& audio = *clip.streamed;
    const double stretch = clip.stretch > 0.0 ? clip.stretch : 1.0;
    const double sourceStep = secondsPerSample / stretch * clip.sourceSampleRate;
    const double firstTimeInClip = blockStartSeconds + first * secondsPerSample - clip.startSeconds;
    const double firstSourcePosition = (clip.sourceOffsetSeconds + firstTimeInClip / stretch) * clip.sourceSampleRate;
    const bool stereo = clip.numChannels > 1;

    for (int i = first; i < last; ++i)
    {
        const double gain = clip.gain * clipEdgeGain(clip, firstTimeInClip + (i - first) * secondsPerSample);
        const double position = firstSourcePosition + (i - first) * sourceStep;
        const double l = interpolateStreamed(audio, 0, position);
        const double r = stereo ? interpolateStreamed(audio, 1, position) : l;
        left[i] += l * gain;
        right[i] += r * gain;
    }

    return true;
}

} // namespace

double clipEdgeGain(const RenderClip& clip, double timeInClip) noexcept
{
    const double clipLength = clip.endSeconds - clip.startSeconds;
    const double fadeIn = std::max(clip.fadeInSeconds, kMinimumEdgeFadeSeconds);
    const double fadeOut = std::max(clip.fadeOutSeconds, kMinimumEdgeFadeSeconds);
    constexpr double halfPi = 1.5707963267948966;
    double gain = 1.0;

    if (timeInClip < fadeIn)
    {
        const double x = std::max(0.0, timeInClip / fadeIn);
        gain *= clip.fadeInEqualPower ? std::sin(halfPi * x) : x;
    }

    const double remaining = clipLength - timeInClip;
    if (remaining < fadeOut)
    {
        const double x = std::max(0.0, remaining / fadeOut);
        gain *= clip.fadeOutEqualPower ? std::sin(halfPi * x) : x;
    }

    return gain;
}

bool renderClip(const RenderClip& clip, double blockStartSeconds, double secondsPerSample, int numSamples,
                Sample* left, Sample* right) noexcept
{
    if (secondsPerSample <= 0.0 || numSamples <= 0 || clip.sourceLength <= 0 || clip.numChannels <= 0)
        return false;

    // Which samples of this block lie inside [start, end)?
    const double firstExact = (clip.startSeconds - blockStartSeconds) / secondsPerSample;
    const double endExact = (clip.endSeconds - blockStartSeconds) / secondsPerSample;
    const int first = static_cast<int>(std::clamp(std::ceil(firstExact - 1.0e-9), 0.0, static_cast<double>(numSamples)));
    const int last = static_cast<int>(std::clamp(std::ceil(endExact - 1.0e-9), 0.0, static_cast<double>(numSamples)));

    if (first >= last)
        return false;

    if (clip.stretcher != nullptr)
        return renderStretchedClip(clip, blockStartSeconds, secondsPerSample, first, last, left, right);

    if (clip.channels == nullptr)
        return clip.streamed != nullptr
               && renderStreamedClip(clip, blockStartSeconds, secondsPerSample, first, last, left, right);

    const double stretch = clip.stretch > 0.0 ? clip.stretch : 1.0;
    const double sourceStep = secondsPerSample / stretch * clip.sourceSampleRate;

    const float* const sourceLeft = clip.channels[0];
    const float* const sourceRight = clip.numChannels > 1 ? clip.channels[1] : clip.channels[0];

    const double firstTimeInClip = blockStartSeconds + first * secondsPerSample - clip.startSeconds;
    const double firstSourcePosition = (clip.sourceOffsetSeconds + firstTimeInClip / stretch) * clip.sourceSampleRate;

    // Exact path: same rate and the clip is aligned to whole source samples.
    const double roundedPosition = std::round(firstSourcePosition);
    const bool exact = std::abs(sourceStep - 1.0) < 1.0e-12 && std::abs(firstSourcePosition - roundedPosition) < 1.0e-6;

    for (int i = first; i < last; ++i)
    {
        const double timeInClip = firstTimeInClip + (i - first) * secondsPerSample;
        const double gain = clip.gain * clipEdgeGain(clip, timeInClip);

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

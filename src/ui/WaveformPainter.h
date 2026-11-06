#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace dojo::ui::waveform
{

/** Below this height per channel, a stereo waveform is drawn as one merged lane. */
inline constexpr float kMinLaneHeight = 10.0f;
/** Space between the left and right lanes (a faint divider is drawn in it). */
inline constexpr float kLaneGap = 1.0f;

/**
    The lanes a waveform is drawn in: one for mono; for stereo, left above right (like Ableton's
    arrangement) when each channel gets at least kMinLaneHeight, otherwise one merged lane.
*/
[[nodiscard]] inline std::vector<juce::Rectangle<float>> lanes(juce::Rectangle<float> area, int numChannels)
{
    if (numChannels < 2 || area.getHeight() < 2.0f * kMinLaneHeight + kLaneGap)
        return {area};
    const float laneHeight = (area.getHeight() - kLaneGap) * 0.5f;
    return {area.withHeight(laneHeight), area.withTrimmedTop(laneHeight + kLaneGap)};
}

/**
    Draws a min/max waveform for pixel columns [firstX, lastX), one lane per channel (see lanes()).
    `range(channel, x)` returns the {minimum, maximum} sample value (-1..1) of `channel` under
    pixel column `x`. A merged lane shows the extremes of every channel. Silence draws a 1 px line
    at the lane's centre.
*/
template <typename Range>
void paintChannels(juce::Graphics& g, juce::Rectangle<float> area, int numChannels, int firstX, int lastX, juce::Colour colour,
                   Range&& range)
{
    if (numChannels <= 0 || lastX <= firstX)
        return;

    const auto laneAreas = lanes(area, numChannels);
    const bool merged = laneAreas.size() < static_cast<std::size_t>(numChannels);

    if (laneAreas.size() > 1)
    {
        g.setColour(colour.withMultipliedAlpha(0.3f));
        g.fillRect(static_cast<float>(firstX), laneAreas[0].getBottom(), static_cast<float>(lastX - firstX), kLaneGap);
    }

    g.setColour(colour);
    for (std::size_t lane = 0; lane < laneAreas.size(); ++lane)
    {
        const auto& r = laneAreas[lane];
        const float mid = r.getCentreY();
        const float half = r.getHeight() * 0.5f;
        const int firstChannel = merged ? 0 : static_cast<int>(lane);
        const int endChannel = merged ? numChannels : firstChannel + 1;

        for (int x = firstX; x < lastX; ++x)
        {
            float lo = 0.0f;
            float hi = 0.0f;
            for (int ch = firstChannel; ch < endChannel; ++ch)
            {
                const auto [minimum, maximum] = range(ch, x);
                lo = std::min(lo, minimum);
                hi = std::max(hi, maximum);
            }
            const float top = mid - std::clamp(hi, 0.0f, 1.0f) * half;
            const float bottom = mid - std::clamp(lo, -1.0f, 0.0f) * half;
            g.fillRect(static_cast<float>(x), top, 1.0f, std::max(1.0f, bottom - top));
        }
    }
}

} // namespace dojo::ui::waveform

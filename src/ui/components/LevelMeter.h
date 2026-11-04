#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace dojo::ui
{

/**
    Stereo peak meter with peak-hold and a latching clip indicator.

    Fed once per UI frame with the "max since last frame" peaks published by the engine, so no
    transient is ever missed. Ballistics (instant attack, 24 dB/s fall, 1.2 s hold) are applied here
    on the message thread, never on the audio thread. Click to reset the clip indicators.
*/
class LevelMeter final : public juce::Component, public juce::SettableTooltipClient
{
public:
    static constexpr float kMinDb = -60.0f;
    static constexpr float kMaxDb = 6.0f;

    LevelMeter();

    /** `peakLeft` / `peakRight` are linear; `elapsedSeconds` is the time since the previous update. */
    void update(float peakLeft, float peakRight, double elapsedSeconds);

    void resetClipIndicators();

    /** Maps a level in dBFS onto [0, 1] meter travel. Shared with the scale drawn beside the meter. */
    [[nodiscard]] static float decibelsToProportion(float decibels) noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& event) override;

private:
    struct Channel
    {
        float displayDb = kMinDb;
        float holdDb = kMinDb;
        double holdAgeSeconds = 0.0;
        bool clipped = false;
    };

    void paintChannel(juce::Graphics& g, juce::Rectangle<float> area, const Channel& channel) const;

    std::array<Channel, 2> channels_;
};

} // namespace dojo::ui

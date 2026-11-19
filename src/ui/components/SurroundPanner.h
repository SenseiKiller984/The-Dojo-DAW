#pragma once

#include "engine/ChannelLayout.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

namespace dojo::ui
{

/**
    A surround panner pad: the room seen from above (front at the top), with the destination's
    speakers around it and a puck for where the signal sits. Drag the puck (or click anywhere) to
    move it: left/right is the track's pan, top/bottom its front/rear position. The centre amount
    (how much of a centred front signal plays from the centre speaker) is shown as a ring around the
    centre speaker; right-click for presets. Double-click puts it back at the front centre.
*/
class SurroundPanner final : public juce::Component, public juce::SettableTooltipClient
{
public:
    SurroundPanner();

    void setLayout(const engine::ChannelLayout& layout);
    /** `pan` -1 (left) .. +1 (right); `frontRear` -1 (front) .. +1 (rear); `centre` 0..1. */
    void setPosition(double pan, double frontRear, double centre);
    [[nodiscard]] double getPan() const noexcept { return pan_; }
    [[nodiscard]] double getFrontRear() const noexcept { return frontRear_; }
    [[nodiscard]] double getCentre() const noexcept { return centre_; }

    /** Where a speaker is drawn, in component coordinates. */
    [[nodiscard]] juce::Point<float> getSpeakerPosition(engine::Speaker speaker) const;
    /** The pad position for a pan / front-rear pair, and back. */
    [[nodiscard]] juce::Point<float> toPoint(double pan, double frontRear) const;
    [[nodiscard]] std::pair<double, double> fromPoint(juce::Point<float> point) const;

    std::function<void()> onDragStart;
    std::function<void(double pan, double frontRear)> onMove;
    std::function<void()> onDragEnd;
    std::function<void(double centre)> onCentreChange;
    /** Double-click: back to the front centre. */
    std::function<void()> onReset;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    [[nodiscard]] juce::Rectangle<float> padArea() const;
    void moveTo(juce::Point<float> point);

    engine::ChannelLayout layout_ = engine::ChannelLayout::surround51();
    double pan_ = 0.0;
    double frontRear_ = -1.0;
    double centre_ = 0.0;
    bool dragging_ = false;
};

} // namespace dojo::ui

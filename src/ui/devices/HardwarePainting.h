#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

/**
    Painting of studio hardware, for the editors that show a machine or a console as it is: needle
    meters, lamps, LED digits, screws, painted and hammertone metal, wood, leather and faders. The
    colours are the hardware's own, the same in every theme. Textures (metal speckle, wood grain,
    leather) are rendered once per size and kept, so meters can repaint 30 times a second over them.
*/
namespace dojo::ui::hardware
{

/** A moving-coil meter's face. Positions run 0 .. 1 along the scale. */
struct MeterFace
{
    struct Mark
    {
        double position = 0.0;
        juce::String text; ///< empty: a tick only
        bool red = false;
    };

    juce::Colour face{0xfff1e4bf}; ///< cream (VU) or white
    juce::Colour ink{0xff1f1c17};
    juce::Colour red{0xffc23b22};
    juce::Colour needle{0xff16130f};
    juce::Colour bezel{0xff141414};
    juce::String legend{"VU"};
    juce::String caption; ///< small text under the legend (e.g. "L")
    bool backlit = true;  ///< a warm lamp behind the face
    double redFrom = 2.0; ///< where the red arc starts (> 1: none)
    std::vector<Mark> marks;
};

/** Where a VU meter's needle sits for `vu`: the scale is linear in voltage, as on the meters
    (0 VU at 71 % of the swing, -20 VU at 7 %, +3 VU at the end). */
[[nodiscard]] double vuPosition(double vu) noexcept;
/** A VU meter's face: -20, -10, -7, -5, -3, -2, -1, 0, +1, +2, +3 VU, red from 0 VU. */
[[nodiscard]] MeterFace vuFace(juce::Colour face = juce::Colour(0xfff1e4bf), const juce::String& legend = "VU");

/** A moving-coil meter, its needle at `position` (pinned just past either end). */
void paintNeedleMeter(juce::Graphics& g, juce::Rectangle<float> area, double position, const MeterFace& face);

/** An illuminated push button or legend: lit in `colour` when on, its dark lens when off. */
void paintLamp(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour colour, bool on, const juce::String& text = {},
               float corner = 2.0f, float textHeight = 8.0f);
/** A round indicator LED in its bezel. */
void paintLed(juce::Graphics& g, juce::Point<float> centre, float radius, juce::Colour colour, bool on);
/** Text in a LED readout: glowing digits, the unlit segments ghosted behind them. */
void paintLedText(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour colour, float height,
                  juce::Justification justification = juce::Justification::centred);
/** A slotted screw head. */
void paintScrew(juce::Graphics& g, juce::Point<float> centre, float radius, float slotAngle = 0.6f);
/** Painted sheet metal lit from above; `speckle` adds a hammertone finish. */
void paintMetal(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base, float corner = 0.0f, bool speckle = false, int seed = 1);
/** Varnished wood, its grain along the longer side. */
void paintWood(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base, int seed = 1, float corner = 0.0f);
/** A padded leather armrest with its stitched seam. */
void paintLeather(juce::Graphics& g, juce::Rectangle<float> area, juce::Colour base);
/** Text engraved into (or printed on) a panel: a light edge under dark ink on light panels. */
void paintEngraved(juce::Graphics& g, juce::Rectangle<float> area, const juce::String& text, juce::Colour ink, const juce::Font& font,
                   juce::Justification justification = juce::Justification::centred);

/** A fader's slot and cap. `position` 0 (bottom) .. 1 (top). */
struct FaderStyle
{
    juce::Colour slot{0xff060606};
    juce::Colour cap{0xffe9e6dd};
    juce::Colour capLine{0xff111111};
    juce::Colour scale{0xff1c1c1c};
    float capHeight = 18.0f;
};
/** Where a fader's cap sits for `position` (its centre line). */
[[nodiscard]] float faderCapCentre(juce::Rectangle<float> area, float position, float capHeight) noexcept;
void paintFader(juce::Graphics& g, juce::Rectangle<float> area, float position, const FaderStyle& style);

/**
    A fader bound to a device parameter: drag the cap (or anywhere on the slot) to move it, with a
    proper change gesture; double-click returns it to its default. The scale shows `marks` (dB).
*/
class ParameterFader final : public juce::Component, public juce::SettableTooltipClient
{
public:
    ParameterFader(juce::RangedAudioParameter& parameter, FaderStyle style, std::vector<float> marks = {10.0f, 5.0f, 0.0f, -5.0f, -10.0f, -20.0f, -30.0f, -50.0f});
    ~ParameterFader() override;

    /** Follows the parameter (call from a timer; repaints when it moved). */
    void refresh();
    [[nodiscard]] juce::RangedAudioParameter& getParameter() noexcept { return parameter_; }
    /** The slot (cap travel) inside the bounds. */
    [[nodiscard]] juce::Rectangle<float> getSlot() const noexcept;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    [[nodiscard]] float positionAt(float y) const noexcept;

    juce::RangedAudioParameter& parameter_;
    FaderStyle style_;
    std::vector<float> marks_;
    float shown_ = -1.0f;
    float dragValue_ = 0.0f, lastY_ = 0.0f;
    bool dragging_ = false;
};

} // namespace dojo::ui::hardware

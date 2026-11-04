#include "ui/components/MixerPanel.h"

#include "ui/PlaceholderSession.h"
#include "ui/theme/DojoTheme.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>
#include <tuple>

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 26;
constexpr int kMasterStripWidth = 132;
constexpr float kFaderMinDb = static_cast<float>(engine::EngineController::kMinusInfinityDb);
constexpr float kFaderMaxDb = 6.0f;
constexpr double kPeakReadoutHoldSeconds = 2.0;

juce::String formatDecibels(double decibels)
{
    if (decibels <= engine::EngineController::kMinusInfinityDb + 0.001)
        return "-inf";

    return (decibels > 0.0 ? "+" : "") + juce::String(decibels, 1);
}

/** Fader travel proportion for a dB value, matching the master fader's skew (mid-point at -12 dB). */
float faderProportion(float decibels)
{
    juce::NormalisableRange<float> range(kFaderMinDb, kFaderMaxDb);
    range.setSkewForCentre(-12.0f);
    return range.convertTo0to1(juce::jlimit(kFaderMinDb, kFaderMaxDb, decibels));
}
} // namespace

//==================================================================================================
// MasterStrip

MasterStrip::MasterStrip(engine::EngineController& controller)
    : controller_(controller)
{
    fader_.setSliderStyle(juce::Slider::LinearVertical);
    fader_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    fader_.setRange(kFaderMinDb, kFaderMaxDb, 0.01);
    fader_.setSkewFactorFromMidPoint(-12.0);
    fader_.setValue(0.0, juce::dontSendNotification);
    fader_.setDoubleClickReturnValue(true, 0.0);
    fader_.setWantsKeyboardFocus(false);
    fader_.setTooltip("Master gain. Double-click to reset to 0 dB.");
    fader_.onValueChange = [this]
    {
        controller_.setMasterGainDecibels(fader_.getValue());
        updateGainReadout();
    };

    addAndMakeVisible(fader_);
    addAndMakeVisible(meter_);
}

void MasterStrip::pushLevels(float peakLeft, float peakRight, double elapsedSeconds)
{
    meter_.update(peakLeft, peakRight, elapsedSeconds);

    const float peakDb = juce::Decibels::gainToDecibels(juce::jmax(peakLeft, peakRight), LevelMeter::kMinDb);
    peakHoldAge_ += elapsedSeconds;

    if (peakDb >= heldPeakDb_ || peakHoldAge_ > kPeakReadoutHoldSeconds)
    {
        if (std::abs(peakDb - heldPeakDb_) > 0.05f)
            repaint(readoutArea_);

        heldPeakDb_ = peakDb;
        peakHoldAge_ = 0.0;
    }
}

void MasterStrip::updateGainReadout()
{
    repaint(readoutArea_);
}

void MasterStrip::resized()
{
    auto area = getLocalBounds().reduced(8, 0);
    area.removeFromTop(28); // title
    readoutArea_ = area.removeFromBottom(40);
    area.removeFromBottom(6);

    auto meterArea = area.removeFromRight(22);
    meter_.setBounds(meterArea);
    scaleArea_ = area.removeFromRight(26);
    fader_.setBounds(area);
}

void MasterStrip::paint(juce::Graphics& g)
{
    using namespace theme;

    const auto bounds = getLocalBounds();
    g.setColour(colour::panelRaised);
    g.fillRect(bounds);

    g.setColour(colour::accent);
    g.fillRect(bounds.withHeight(3));

    g.setColour(colour::textPrimary);
    g.setFont(label(11.0f));
    g.drawText("MASTER", bounds.withTrimmedTop(3).withHeight(24), juce::Justification::centred, false);

    // dB scale aligned with the meter (the meter's clip LED occupies its top 7 px).
    const auto meterBody = meter_.getBounds().withTrimmedTop(7).toFloat();
    g.setFont(mono(9.0f));
    for (const float db : {6.0f, 0.0f, -6.0f, -12.0f, -18.0f, -24.0f, -36.0f, -48.0f, -60.0f})
    {
        const float y = meterBody.getBottom() - meterBody.getHeight() * LevelMeter::decibelsToProportion(db);
        g.setColour(juce::approximatelyEqual(db, 0.0f) ? colour::textSecondary : colour::textMuted);
        g.drawText(juce::String(juce::roundToInt(db)),
                   juce::Rectangle<float>(static_cast<float>(scaleArea_.getX()), y - 6.0f,
                                          static_cast<float>(scaleArea_.getWidth()) - 3.0f, 12.0f),
                   juce::Justification::centredRight, false);
    }

    // Readouts: fader gain (left) and held output peak (right).
    auto readout = readoutArea_;
    auto gainBox = readout.removeFromLeft(readout.getWidth() / 2).reduced(2, 4);
    auto peakBox = readout.reduced(2, 4);

    for (const auto& [box, text, caption, textColour] :
         {std::tuple{gainBox, formatDecibels(fader_.getValue()), "GAIN", colour::textPrimary},
          std::tuple{peakBox, formatDecibels(heldPeakDb_), "PEAK",
                     heldPeakDb_ > 0.0f ? colour::meterClip : colour::textPrimary}})
    {
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(box.toFloat(), 3.0f);

        auto inner = box;
        g.setColour(colour::textMuted);
        g.setFont(label(8.0f));
        g.drawText(caption, inner.removeFromBottom(12), juce::Justification::centred, false);
        g.setColour(textColour);
        g.setFont(mono(11.0f));
        g.drawText(text, inner, juce::Justification::centred, false);
    }

    g.setColour(colour::outline);
    g.fillRect(bounds.withWidth(1));
}

//==================================================================================================
// MixerPanel

MixerPanel::MixerPanel(engine::EngineController& controller)
    : master_(controller)
{
    setOpaque(true);
    addAndMakeVisible(master_);
}

void MixerPanel::pushMasterLevels(float peakLeft, float peakRight, double elapsedSeconds)
{
    master_.pushLevels(peakLeft, peakRight, elapsedSeconds);
}

void MixerPanel::resized()
{
    auto area = getLocalBounds().withTrimmedTop(kHeaderHeight);
    master_.setBounds(area.removeFromRight(kMasterStripWidth));
    stripsArea_ = area;
}

void MixerPanel::paint(juce::Graphics& g)
{
    using namespace theme;

    g.fillAll(colour::panel);

    auto header = getLocalBounds().removeFromTop(kHeaderHeight);
    g.setColour(colour::panelHeader);
    g.fillRect(header);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    g.drawText("MIXER", header.reduced(14, 0), juce::Justification::centredLeft, false);
    g.setColour(colour::textMuted);
    g.setFont(sans(11.0f));
    g.drawText("Channel strips: layout preview  |  Master: live", header.reduced(14, 0),
               juce::Justification::centredRight, false);
    g.setColour(colour::outline);
    g.fillRect(header.removeFromBottom(1));
    g.fillRect(getLocalBounds().removeFromTop(1));

    auto strips = stripsArea_;
    const auto& tracks = placeholder::getTracks();

    for (std::size_t i = 0; i < tracks.size() && strips.getWidth() >= metrics::channelStripWidth; ++i)
        paintPlaceholderStrip(g, strips.removeFromLeft(metrics::channelStripWidth), i);
}

void MixerPanel::paintPlaceholderStrip(juce::Graphics& g, juce::Rectangle<int> area,
                                       std::size_t trackIndex) const
{
    using namespace theme;

    const auto& track = placeholder::getTracks()[trackIndex];
    const float dim = 0.7f;

    g.setColour(trackIndex % 2 == 0 ? colour::panel : colour::panelRaised.withAlpha(0.6f));
    g.fillRect(area);
    g.setColour(colour::outline);
    g.fillRect(area.removeFromRight(1));

    g.setColour(track.colour.withMultipliedAlpha(dim));
    g.fillRect(area.removeFromTop(3));

    auto content = area.reduced(6, 4);

    g.setColour(colour::textPrimary.withMultipliedAlpha(dim));
    g.setFont(sansBold(11.5f));
    g.drawText(track.name, content.removeFromTop(16), juce::Justification::centred, true);
    g.setColour(colour::textMuted);
    g.setFont(label(8.0f));
    g.drawText(placeholder::getKindLabel(track.kind), content.removeFromTop(12), juce::Justification::centred,
               false);

    // Pan knob.
    const auto knob = content.removeFromTop(34).withSizeKeepingCentre(26, 26).toFloat();
    g.setColour(colour::control);
    g.fillEllipse(knob);
    g.setColour(colour::outlineStrong);
    g.drawEllipse(knob, 1.0f);
    const float angle = track.pan * juce::MathConstants<float>::pi * 0.75f;
    const auto centre = knob.getCentre();
    g.setColour(colour::textSecondary.withMultipliedAlpha(dim));
    g.drawLine(centre.x, centre.y, centre.x + std::sin(angle) * 10.0f, centre.y - std::cos(angle) * 10.0f,
               2.0f);

    // Mute / solo / phase.
    auto buttons = content.removeFromTop(22).withSizeKeepingCentre(3 * 19 + 2 * 3, 17);
    for (const auto* text : {"M", "S", "\xc3\x98"})
    {
        const auto box = buttons.removeFromLeft(19).toFloat();
        buttons.removeFromLeft(3);
        g.setColour(colour::control);
        g.fillRoundedRectangle(box, 3.0f);
        g.setColour(colour::textMuted);
        g.setFont(sansBold(9.5f));
        g.drawText(juce::String::fromUTF8(text), box, juce::Justification::centred, false);
    }

    // Fader + meter grooves.
    auto readout = content.removeFromBottom(16);
    content.removeFromBottom(4);
    content.removeFromTop(4);

    auto meterGroove = content.removeFromRight(10).toFloat();
    g.setColour(colour::meterBackground);
    g.fillRoundedRectangle(meterGroove, 2.0f);

    const auto groove = juce::Rectangle<float>(4.0f, static_cast<float>(content.getHeight()))
                            .withCentre(content.toFloat().getCentre());
    g.fillRoundedRectangle(groove, 2.0f);

    const float capY = groove.getBottom() - groove.getHeight() * faderProportion(track.faderDb);
    const auto cap = juce::Rectangle<float>(26.0f, 14.0f).withCentre({groove.getCentreX(), capY});
    g.setColour(colour::control.brighter(0.1f));
    g.fillRoundedRectangle(cap, 2.5f);
    g.setColour(colour::textMuted);
    g.fillRect(cap.withSizeKeepingCentre(18.0f, 1.5f));

    g.setColour(colour::textMuted);
    g.setFont(mono(10.0f));
    g.drawText(formatDecibels(track.faderDb), readout, juce::Justification::centred, false);
}

} // namespace dojo::ui

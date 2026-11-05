#include "ui/components/MixerPanel.h"

#include "ui/theme/DojoTheme.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 26;
constexpr int kMasterStripWidth = 132;
constexpr int kStripWidth = 86;
constexpr float kFaderMinDb = static_cast<float>(session::kMinusInfinityDb);
constexpr float kFaderMaxDb = static_cast<float>(session::kMaxVolumeDb);
constexpr double kPeakReadoutHoldSeconds = 2.0;

juce::String formatDecibels(double decibels)
{
    if (decibels <= session::kMinusInfinityDb + 0.001)
        return "-inf";

    return (decibels > 0.0 ? "+" : "") + juce::String(decibels, 1);
}

juce::String formatPan(double pan)
{
    const int percent = juce::roundToInt(std::abs(pan) * 100.0);
    return percent == 0 ? juce::String("C") : (pan < 0.0 ? "L" : "R") + juce::String(percent);
}

void configureFader(juce::Slider& fader)
{
    fader.setSliderStyle(juce::Slider::LinearVertical);
    fader.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    fader.setRange(kFaderMinDb, kFaderMaxDb, 0.01);
    fader.setSkewFactorFromMidPoint(-12.0);
    fader.setDoubleClickReturnValue(true, 0.0);
    fader.setWantsKeyboardFocus(false);
}

void configureToggle(juce::TextButton& button, const juce::String& text, juce::Colour onColour,
                     const juce::String& tip)
{
    button.setButtonText(text);
    button.setClickingTogglesState(true);
    button.setWantsKeyboardFocus(false);
    button.setTooltip(tip);
    button.setColour(juce::TextButton::buttonOnColourId, onColour);
    button.setColour(juce::TextButton::textColourOnId, theme::colour::windowBackground);
}
} // namespace

//==================================================================================================
// ChannelStrip

ChannelStrip::ChannelStrip(session::Session& session, EditorState& editor, juce::ValueTree track)
    : session_(session)
    , editor_(editor)
    , track_(std::move(track))
{
    pan_.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
    pan_.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    pan_.setRange(-1.0, 1.0, 0.01);
    pan_.setDoubleClickReturnValue(true, 0.0);
    pan_.setWantsKeyboardFocus(false);
    pan_.setTooltip("Pan. Double-click to centre.");
    pan_.onDragStart = [this]
    {
        panDragging_ = true;
        session_.beginTransaction("Change Pan");
    };
    pan_.onDragEnd = [this]
    {
        panDragging_ = false;
    };
    pan_.onValueChange = [this]
    {
        if (!panDragging_) // e.g. double-click reset: its own undo step
            session_.beginTransaction("Change Pan");
        session::track::setPan(track_, pan_.getValue(), &session_.getUndoManager());
    };

    configureFader(fader_);
    fader_.setTooltip("Track volume. Double-click to reset to 0 dB.");
    fader_.onDragStart = [this]
    {
        faderDragging_ = true;
        session_.beginTransaction("Change Volume");
    };
    fader_.onDragEnd = [this]
    {
        faderDragging_ = false;
    };
    fader_.onValueChange = [this]
    {
        if (!faderDragging_)
            session_.beginTransaction("Change Volume");
        session::track::setVolumeDb(track_, fader_.getValue(), &session_.getUndoManager());
    };

    configureToggle(mute_, "M", theme::colour::gold, "Mute");
    configureToggle(solo_, "S", theme::colour::play, "Solo");
    configureToggle(phase_, juce::String::fromUTF8("\xc3\x98"), juce::Colour(0xff4f8fd9),
                    "Invert phase (polarity)");

    mute_.onClick = [this]
    {
        session_.beginTransaction("Mute");
        session::track::setMuted(track_, mute_.getToggleState(), &session_.getUndoManager());
    };
    solo_.onClick = [this]
    {
        session_.beginTransaction("Solo");
        session::track::setSoloed(track_, solo_.getToggleState(), &session_.getUndoManager());
    };
    phase_.onClick = [this]
    {
        session_.beginTransaction("Invert Phase");
        session::track::setPhaseInverted(track_, phase_.getToggleState(), &session_.getUndoManager());
    };

    for (auto* child :
         std::initializer_list<juce::Component*>{&pan_, &mute_, &solo_, &phase_, &fader_, &meter_})
        addAndMakeVisible(child);

    syncFromModel();
    track_.addListener(this);
    editor_.addListener(this);
}

ChannelStrip::~ChannelStrip()
{
    editor_.removeListener(this);
    track_.removeListener(this);
}

void ChannelStrip::syncFromModel()
{
    pan_.setValue(session::track::getPan(track_), juce::dontSendNotification);
    fader_.setValue(session::track::getVolumeDb(track_), juce::dontSendNotification);
    mute_.setToggleState(session::track::isMuted(track_), juce::dontSendNotification);
    solo_.setToggleState(session::track::isSoloed(track_), juce::dontSendNotification);
    phase_.setToggleState(session::track::isPhaseInverted(track_), juce::dontSendNotification);
    repaint();
}

void ChannelStrip::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree == track_)
        syncFromModel();
}

void ChannelStrip::pushLevels(float peakLeft, float peakRight, double elapsedSeconds)
{
    meter_.update(peakLeft, peakRight, elapsedSeconds);
}

void ChannelStrip::mouseDown(const juce::MouseEvent&)
{
    editor_.selectTrack(session::track::getId(track_));
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced(6, 4);
    area.removeFromTop(3);
    nameArea_ = area.removeFromTop(18);
    pan_.setBounds(area.removeFromTop(38).withSizeKeepingCentre(34, 34));
    area.removeFromTop(2);

    auto buttons = area.removeFromTop(20).withSizeKeepingCentre(3 * 22 + 2 * 3, 20);
    for (auto* button : {&mute_, &solo_, &phase_})
    {
        button->setBounds(buttons.removeFromLeft(22));
        buttons.removeFromLeft(3);
    }

    area.removeFromTop(6);
    readoutArea_ = area.removeFromBottom(16);
    area.removeFromBottom(4);
    meter_.setBounds(area.removeFromRight(14));
    area.removeFromRight(2);
    fader_.setBounds(area);
}

void ChannelStrip::paint(juce::Graphics& g)
{
    using namespace theme;

    const bool selected = session::track::getId(track_) == editor_.getSelectedTrack();
    auto bounds = getLocalBounds();

    g.setColour(selected ? colour::panelHeader.brighter(0.05f) : colour::panel);
    g.fillRect(bounds);
    g.setColour(colour::outline);
    g.fillRect(bounds.removeFromRight(1));
    g.setColour(session::track::getColour(track_));
    g.fillRect(bounds.removeFromTop(selected ? 4 : 3));

    g.setColour(colour::textPrimary);
    g.setFont(sansBold(11.5f));
    g.drawText(session::track::getName(track_), nameArea_, juce::Justification::centred, true);

    g.setColour(colour::textMuted);
    g.setFont(mono(10.0f));
    g.drawText(formatDecibels(session::track::getVolumeDb(track_)) + "  " +
                   formatPan(session::track::getPan(track_)),
               readoutArea_, juce::Justification::centred, false);
}

//==================================================================================================
// MasterStrip

MasterStrip::MasterStrip(session::Session& session)
    : session_(session)
{
    configureFader(fader_);
    fader_.setTooltip("Master volume. Double-click to reset to 0 dB.");
    fader_.setValue(session_.getMasterVolumeDb(), juce::dontSendNotification);
    fader_.onDragStart = [this]
    {
        dragging_ = true;
        session_.beginTransaction("Change Master Volume");
    };
    fader_.onDragEnd = [this]
    {
        dragging_ = false;
    };
    fader_.onValueChange = [this]
    {
        if (!dragging_)
            session_.beginTransaction("Change Master Volume");
        session_.setMasterVolumeDb(fader_.getValue());
        repaint(readoutArea_);
    };

    addAndMakeVisible(fader_);
    addAndMakeVisible(meter_);
    session_.getState().addListener(this);
}

MasterStrip::~MasterStrip()
{
    session_.getState().removeListener(this);
}

void MasterStrip::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == session_.getState() && property == session::ids::masterVolume)
    {
        fader_.setValue(session_.getMasterVolumeDb(), juce::dontSendNotification);
        repaint(readoutArea_);
    }
}

void MasterStrip::valueTreeRedirected(juce::ValueTree&)
{
    fader_.setValue(session_.getMasterVolumeDb(), juce::dontSendNotification);
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

void MasterStrip::resized()
{
    auto area = getLocalBounds().reduced(8, 0);
    area.removeFromTop(28);
    readoutArea_ = area.removeFromBottom(40);
    area.removeFromBottom(6);

    meter_.setBounds(area.removeFromRight(22));
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

    auto readout = readoutArea_;
    const auto paintReadout =
        [&g](juce::Rectangle<int> box, const juce::String& text, const char* caption, juce::Colour textColour)
    {
        g.setColour(colour::meterBackground);
        g.fillRoundedRectangle(box.toFloat(), 3.0f);
        g.setColour(colour::textMuted);
        g.setFont(label(8.0f));
        g.drawText(caption, box.removeFromBottom(12), juce::Justification::centred, false);
        g.setColour(textColour);
        g.setFont(mono(11.0f));
        g.drawText(text, box, juce::Justification::centred, false);
    };

    paintReadout(readout.removeFromLeft(readout.getWidth() / 2).reduced(2, 4),
                 formatDecibels(fader_.getValue()), "GAIN", colour::textPrimary);
    paintReadout(readout.reduced(2, 4), formatDecibels(heldPeakDb_), "PEAK",
                 heldPeakDb_ > 0.0f ? colour::meterClip : colour::textPrimary);

    g.setColour(colour::outline);
    g.fillRect(bounds.withWidth(1));
}

//==================================================================================================
// MixerPanel

MixerPanel::MixerPanel(session::Project& project, engine::EngineController& controller, EditorState& editor)
    : project_(project)
    , controller_(controller)
    , editor_(editor)
    , master_(project.getSession())
{
    setOpaque(true);
    viewport_.setViewedComponent(&stripHolder_, false);
    viewport_.setScrollBarsShown(false, true);
    viewport_.setScrollBarThickness(10);
    addAndMakeVisible(viewport_);
    addAndMakeVisible(master_);

    project_.getSession().getState().addListener(this);
    rebuildStrips();
}

MixerPanel::~MixerPanel()
{
    project_.getSession().getState().removeListener(this);
}

void MixerPanel::rebuildStrips()
{
    strips_.clear();
    auto& session = project_.getSession();

    for (int i = 0; i < session.getNumTracks(); ++i)
    {
        auto strip = std::make_unique<ChannelStrip>(session, editor_, session.getTrack(i));
        stripHolder_.addAndMakeVisible(*strip);
        strips_.push_back(std::move(strip));
    }

    layoutStrips();
    repaint();
}

void MixerPanel::layoutStrips()
{
    const int height = viewport_.getMaximumVisibleHeight();
    stripHolder_.setSize(static_cast<int>(strips_.size()) * kStripWidth, juce::jmax(0, height));

    int x = 0;
    for (auto& strip : strips_)
    {
        strip->setBounds(x, 0, kStripWidth, height);
        x += kStripWidth;
    }
}

void MixerPanel::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent == project_.getSession().getState())
        rebuildStrips();
}

void MixerPanel::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent == project_.getSession().getState())
        rebuildStrips();
}

void MixerPanel::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)
{
    if (parent == project_.getSession().getState())
        rebuildStrips();
}

void MixerPanel::updateMeters(const engine::TelemetrySnapshot& telemetry, double elapsedSeconds)
{
    master_.pushLevels(telemetry.masterPeakLeft, telemetry.masterPeakRight, elapsedSeconds);

    for (auto& strip : strips_)
    {
        const int slot = project_.getCompiler().getSlot(session::track::getId(strip->getTrack()));
        const auto peak = slot >= 0 ? controller_.collectTrackPeak(slot) : engine::StereoPeak{};
        strip->pushLevels(peak.left, peak.right, elapsedSeconds);
    }
}

void MixerPanel::resized()
{
    auto area = getLocalBounds().withTrimmedTop(kHeaderHeight);
    master_.setBounds(area.removeFromRight(kMasterStripWidth));
    viewport_.setBounds(area);
    layoutStrips();
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
    g.drawText("Fader / pan: drag, double-click to reset  |  M mute  S solo  " +
                   juce::String::fromUTF8("\xc3\x98") + " phase",
               header.reduced(14, 0), juce::Justification::centredRight, false);
    g.setColour(colour::outline);
    g.fillRect(header.removeFromBottom(1));
    g.fillRect(getLocalBounds().removeFromTop(1));

    if (strips_.empty())
    {
        auto empty = viewport_.getBounds();
        g.setColour(colour::textMuted);
        g.setFont(sans(12.5f));
        g.drawText("Channel strips appear here as you add tracks.", empty, juce::Justification::centred,
                   false);
    }
}

} // namespace dojo::ui

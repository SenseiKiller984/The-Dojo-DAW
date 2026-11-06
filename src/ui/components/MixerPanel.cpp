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
constexpr int kStripWidth = 104;
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
    button.setColour(juce::TextButton::textColourOnId, theme::textOn(onColour));
}
} // namespace

//==================================================================================================
// ChannelStrip

ChannelStrip::ChannelStrip(session::Session& session, EditorState& editor, juce::ValueTree track,
                           std::function<juce::StringArray()> inputNames, std::function<void()> showDevices)
    : session_(session)
    , editor_(editor)
    , track_(std::move(track))
    , inputNames_(std::move(inputNames))
    , showDevices_(std::move(showDevices))
{
    const auto kind = session::track::getKind(track_);

    input_.setTooltip("Input (recording and monitoring)");
    input_.setWantsKeyboardFocus(false);
    input_.onChange = [this]
    {
        const int id = input_.getSelectedId();
        if (id <= 0)
            return;
        session_.beginTransaction("Track Input");
        if (id == 1)
            session::track::setInput(track_, -1, true, &session_.getUndoManager());
        else if (id < 1000)
            session::track::setInput(track_, id - 2, false, &session_.getUndoManager());
        else
            session::track::setInput(track_, id - 1000, true, &session_.getUndoManager());
    };
    input_.setVisible(kind == session::TrackKind::audio);
    addChildComponent(input_);

    output_.setTooltip("Output: where this track's signal goes");
    output_.setWantsKeyboardFocus(false);
    output_.onChange = [this]
    {
        const int index = output_.getSelectedId() - 1;
        if (index < 0 || index >= static_cast<int>(outputIds_.size()))
            return;
        session_.beginTransaction("Route Track");
        session::track::setOutput(track_, outputIds_[static_cast<std::size_t>(index)], &session_.getUndoManager());
    };
    addAndMakeVisible(output_);

    configureToggle(arm_, "R", theme::colour::record, "Record arm");
    arm_.onClick = [this]
    {
        session_.beginTransaction("Arm Track");
        session::track::setArmed(track_, arm_.getToggleState(), &session_.getUndoManager());
    };
    arm_.setVisible(kind != session::TrackKind::bus);
    addChildComponent(arm_);

    monitor_.setWantsKeyboardFocus(false);
    monitor_.setTooltip("Input monitoring: AUTO = while armed, IN = always, OFF = never");
    monitor_.onClick = [this]
    {
        const auto next = static_cast<engine::MonitorMode>((static_cast<int>(session::track::getMonitor(track_)) + 1) % 3);
        session_.beginTransaction("Monitoring");
        session::track::setMonitor(track_, next, &session_.getUndoManager());
    };
    monitor_.setVisible(kind != session::TrackKind::bus);
    addChildComponent(monitor_);

    devices_.setWantsKeyboardFocus(false);
    devices_.setTooltip("Devices on this track (opens the Devices panel)");
    devices_.onClick = [this]
    {
        editor_.selectTrack(session::track::getId(track_));
        if (showDevices_)
            showDevices_();
    };
    addAndMakeVisible(devices_);

    addSend_.setButtonText("+ SEND");
    addSend_.setWantsKeyboardFocus(false);
    addSend_.setTooltip("Send this track to a bus (e.g. a reverb return)");
    addSend_.onClick = [this] { showSendMenu(); };
    addAndMakeVisible(addSend_);

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

    rebuildRouting();
    rebuildSends();
    syncFromModel();
    track_.addListener(this);
    session_.getState().addListener(this);
    editor_.addListener(this);
}

ChannelStrip::~ChannelStrip()
{
    editor_.removeListener(this);
    session_.getState().removeListener(this);
    track_.removeListener(this);
}

void ChannelStrip::rebuildRouting()
{
    // Inputs: none, each channel (mono), each pair (stereo).
    input_.clear(juce::dontSendNotification);
    input_.addItem("No input", 1);
    const auto names = inputNames_ ? inputNames_() : juce::StringArray();
    for (int ch = 0; ch < names.size(); ++ch)
        input_.addItem("In " + juce::String(ch + 1) + " (" + names[ch] + ")", ch + 2);
    for (int ch = 0; ch + 1 < names.size(); ch += 2)
        input_.addItem("In " + juce::String(ch + 1) + "/" + juce::String(ch + 2), 1000 + ch);
    if (names.isEmpty())
        input_.addItem("In 1/2 (no device inputs)", 1000);

    const int in = session::track::getInput(track_);
    input_.setSelectedId(in < 0 ? 1 : (session::track::isInputStereo(track_) ? 1000 + in : in + 2), juce::dontSendNotification);

    // Outputs: the default (folder or master) and every bus that would not create a loop.
    output_.clear(juce::dontSendNotification);
    outputIds_.clear();
    const auto folder = session::Session::getParentFolder(track_);
    outputIds_.push_back(0);
    output_.addItem(folder.isValid() ? "> " + session::track::getName(folder) : juce::String("> Master"), 1);
    for (const auto& bus : session_.getBusses())
    {
        if (bus == track_ || bus == folder || !session_.canFeed(track_, bus))
            continue;
        outputIds_.push_back(session::track::getId(bus));
        output_.addItem("> " + session::track::getName(bus), static_cast<int>(outputIds_.size()));
    }
    const auto current = session::track::getOutput(track_);
    const auto it = std::find(outputIds_.begin(), outputIds_.end(), current);
    output_.setSelectedId(it != outputIds_.end() ? static_cast<int>(it - outputIds_.begin()) + 1 : 1, juce::dontSendNotification);
}

void ChannelStrip::rebuildSends()
{
    sends_.clear();
    for (const auto& s : session::Session::getSends(track_))
    {
        auto control = std::make_unique<SendControl>();
        control->send = s;
        auto& slider = control->level;
        slider.setSliderStyle(juce::Slider::LinearBar);
        slider.setRange(kFaderMinDb, kFaderMaxDb, 0.1);
        slider.setSkewFactorFromMidPoint(-12.0);
        slider.setDoubleClickReturnValue(true, -6.0);
        slider.setWantsKeyboardFocus(false);
        const auto bus = session_.findTrack(session::send::getTarget(s));
        const auto busName = bus.isValid() ? session::track::getName(bus) : juce::String("?");
        slider.setTextValueSuffix(" " + busName + (session::send::isPreFader(s) ? " (pre)" : ""));
        slider.setTooltip("Send to " + busName + ". Right-click for pre/post fader or to remove.");
        slider.setValue(session::send::getGainDb(s), juce::dontSendNotification);
        auto sendTree = s;
        slider.onDragStart = [this] { session_.beginTransaction("Send Level"); };
        slider.onValueChange = [this, sendTree, &slider]() mutable
        {
            if (slider.getThumbBeingDragged() < 0)
                session_.beginTransaction("Send Level");
            session::send::setGainDb(sendTree, slider.getValue(), &session_.getUndoManager());
        };
        slider.addMouseListener(this, false);
        addAndMakeVisible(slider);
        sends_.push_back(std::move(control));
    }
    resized();
}

void ChannelStrip::showSendMenu()
{
    juce::Component::SafePointer<ChannelStrip> safe(this);
    juce::PopupMenu menu;
    for (const auto& bus : session_.getBusses())
    {
        if (bus == track_ || !session_.canFeed(track_, bus))
            continue;
        const auto busId = session::track::getId(bus);
        menu.addItem("Send to " + session::track::getName(bus), [safe, busId]
                     {
                         if (safe == nullptr)
                             return;
                         safe->session_.beginTransaction("Add Send");
                         safe->session_.addSend(safe->track_, safe->session_.findTrack(busId), -6.0, false);
                     });
    }
    menu.addSeparator();
    menu.addItem("New Return Bus + Send", [safe]
                 {
                     if (safe == nullptr)
                         return;
                     safe->session_.beginTransaction("Add Return Bus");
                     auto bus = safe->session_.addTrack(session::TrackKind::bus, "Return");
                     safe->session_.addSend(safe->track_, bus, -6.0, false);
                 });
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&addSend_));
}

void ChannelStrip::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == track_ && child.hasType(session::ids::send))
        rebuildSends();
    else if (child.hasType(session::ids::track))
        rebuildRouting();
    if (parent == track_ && child.hasType(session::ids::device))
        syncFromModel();
}

void ChannelStrip::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int)
{
    if (parent == track_ && child.hasType(session::ids::send))
        rebuildSends();
    else if (child.hasType(session::ids::track))
        rebuildRouting();
    if (parent == track_ && child.hasType(session::ids::device))
        syncFromModel();
}

void ChannelStrip::syncFromModel()
{
    arm_.setToggleState(session::track::isArmed(track_), juce::dontSendNotification);
    const auto monitor = session::track::getMonitor(track_);
    monitor_.setButtonText(monitor == engine::MonitorMode::off ? "OFF" : monitor == engine::MonitorMode::always ? "IN" : "AUTO");
    const auto deviceCount = session::Session::getDevices(track_).size();
    devices_.setButtonText(deviceCount == 0 ? juce::String("FX") : "FX " + juce::String(static_cast<int>(deviceCount)));
    for (auto& control : sends_)
        control->level.setValue(session::send::getGainDb(control->send), juce::dontSendNotification);
    pan_.setValue(session::track::getPan(track_), juce::dontSendNotification);
    fader_.setValue(session::track::getVolumeDb(track_), juce::dontSendNotification);
    mute_.setToggleState(session::track::isMuted(track_), juce::dontSendNotification);
    solo_.setToggleState(session::track::isSoloed(track_), juce::dontSendNotification);
    phase_.setToggleState(session::track::isPhaseInverted(track_), juce::dontSendNotification);
    repaint();
}

void ChannelStrip::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == track_ && (property == session::ids::output || property == session::ids::input || property == session::ids::inputStereo))
        rebuildRouting();
    if (tree == track_ || tree.getParent() == track_)
        syncFromModel();
    else if (tree.hasType(session::ids::track) && property == session::ids::name)
        rebuildRouting();
}

void ChannelStrip::pushLevels(float peakLeft, float peakRight, double elapsedSeconds)
{
    meter_.update(peakLeft, peakRight, elapsedSeconds);
}

void ChannelStrip::mouseDown(const juce::MouseEvent& e)
{
    editor_.selectTrack(session::track::getId(track_));

    // Right-click on a send: pre/post fader, remove.
    for (auto& control : sends_)
    {
        if (e.eventComponent != &control->level || !e.mods.isPopupMenu())
            continue;
        juce::Component::SafePointer<ChannelStrip> safe(this);
        auto sendTree = control->send;
        juce::PopupMenu menu;
        menu.addItem("Pre-fader", true, session::send::isPreFader(sendTree), [safe, sendTree]() mutable
                     {
                         if (safe == nullptr)
                             return;
                         safe->session_.beginTransaction("Send Pre/Post");
                         session::send::setPreFader(sendTree, !session::send::isPreFader(sendTree), &safe->session_.getUndoManager());
                         safe->rebuildSends();
                     });
        menu.addItem("Remove Send", [safe, sendTree]() mutable
                     {
                         if (safe == nullptr)
                             return;
                         safe->session_.beginTransaction("Remove Send");
                         safe->session_.removeSend(sendTree);
                     });
        menu.showMenuAsync(juce::PopupMenu::Options().withMousePosition());
    }
}

void ChannelStrip::resized()
{
    auto area = getLocalBounds().reduced(5, 4);
    area.removeFromTop(3);
    nameArea_ = area.removeFromTop(16);

    if (input_.isVisible())
        input_.setBounds(area.removeFromTop(20));
    else
        area.removeFromTop(20);
    area.removeFromTop(2);
    output_.setBounds(area.removeFromTop(20));
    area.removeFromTop(3);

    for (auto& control : sends_)
    {
        control->level.setBounds(area.removeFromTop(16));
        area.removeFromTop(2);
    }
    auto sendRow = area.removeFromTop(18);
    addSend_.setBounds(sendRow.removeFromLeft(sendRow.getWidth() / 2 - 1));
    sendRow.removeFromLeft(2);
    devices_.setBounds(sendRow);
    area.removeFromTop(2);

    pan_.setBounds(area.removeFromTop(34).withSizeKeepingCentre(30, 30));

    auto buttons = area.removeFromTop(20);
    const int buttonWidth = (buttons.getWidth() - 3 * 2) / (arm_.isVisible() ? 4 : 3);
    for (auto* button : {&mute_, &solo_, &phase_, &arm_})
    {
        if (!button->isVisible())
            continue;
        button->setBounds(buttons.removeFromLeft(buttonWidth));
        buttons.removeFromLeft(2);
    }
    if (monitor_.isVisible())
    {
        area.removeFromTop(2);
        monitor_.setBounds(area.removeFromTop(18));
    }

    area.removeFromTop(4);
    readoutArea_ = area.removeFromBottom(14);
    area.removeFromBottom(2);
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
    if (const int depth = session::Session::getDepth(track_); depth > 0)
    {
        g.setColour(session::track::getColour(session::Session::getParentFolder(track_)).withAlpha(0.5f));
        g.fillRect(getLocalBounds().removeFromBottom(3));
    }

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

MasterStrip::MasterStrip(session::Session& session, EditorState& editor, std::function<void()> showDevices)
    : session_(session)
    , editor_(editor)
    , showDevices_(std::move(showDevices))
{
    devices_.setButtonText("MASTER FX");
    devices_.setWantsKeyboardFocus(false);
    devices_.setTooltip("Devices on the master bus (opens the Devices panel)");
    devices_.onClick = [this]
    {
        editor_.selectMaster();
        if (showDevices_)
            showDevices_();
    };
    addAndMakeVisible(devices_);

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
    area.removeFromTop(26);
    devices_.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);
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
    , master_(project.getSession(), editor, [this] { if (onShowDevices) onShowDevices(); })
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

    auto* host = &controller_.getDeviceHost();
    for (const auto& t : session.getAllTracks())
    {
        auto strip = std::make_unique<ChannelStrip>(session, editor_, t, [host] { return host->getActiveInputChannelNames(); },
                                                    [this]
                                                    {
                                                        if (onShowDevices)
                                                            onShowDevices();
                                                    });
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

void MixerPanel::valueTreeChildAdded(juce::ValueTree&, juce::ValueTree& child)
{
    if (child.hasType(session::ids::track))
        triggerAsyncUpdate();
}

void MixerPanel::valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree& child, int)
{
    if (child.hasType(session::ids::track))
        triggerAsyncUpdate();
}

void MixerPanel::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)
{
    if (parent == project_.getSession().getState() || parent.hasType(session::ids::track))
        triggerAsyncUpdate();
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
    g.drawText("Double-click to reset  |  M mute  S solo  " + juce::String::fromUTF8("\xc3\x98") +
                   " phase  R arm  |  right-click a send for pre/post",
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

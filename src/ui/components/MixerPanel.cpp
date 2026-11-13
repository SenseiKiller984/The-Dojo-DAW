#include "ui/components/MixerPanel.h"

#include "engine/AudioEngine.h"

#include "ui/MixerValues.h"
#include "ui/theme/DojoTheme.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr int kHeaderHeight = 26;
constexpr int kMasterStripWidth = 132;
constexpr float kFaderMinDb = static_cast<float>(session::kMinusInfinityDb);
constexpr float kFaderMaxDb = static_cast<float>(session::kMaxVolumeDb);
constexpr double kPeakReadoutHoldSeconds = 2.0;

using mixer::formatDecibels;
using mixer::formatPan;

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
        else if (id < 2000)
            session::track::setInput(track_, id - 1000, true, &session_.getUndoManager());
        else // a multichannel track: as many inputs as it has channels, from this one
            session::track::setInput(track_, id - 2000, true, &session_.getUndoManager());
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
    arm_.setVisible(kind == session::TrackKind::audio || session::holdsMidi(kind));
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

    // Surround: left/right is the pan, front/rear and the centre amount are the track's surround
    // position. One drag is one undo step.
    surround_.onDragStart = [this] { session_.beginTransaction("Surround Pan"); };
    surround_.onMove = [this](double pan, double frontRear)
    {
        session::track::setPan(track_, pan, &session_.getUndoManager());
        session::track::setPanFrontRear(track_, frontRear, &session_.getUndoManager());
    };
    surround_.onCentreChange = [this](double centre)
    {
        session_.beginTransaction("Surround Centre");
        session::track::setPanCentre(track_, centre, &session_.getUndoManager());
    };
    surround_.onReset = [this]
    {
        session_.beginTransaction("Reset Surround Pan");
        session::track::setPan(track_, 0.0, &session_.getUndoManager());
        session::track::setPanFrontRear(track_, -1.0, &session_.getUndoManager());
    };
    addChildComponent(surround_);

    rebuildRouting();
    rebuildSends();

    // Control tracks show only what they have: a MIDI track has no audio (mute, solo, arm, monitoring);
    // a VCA Master is a fader with mute and solo; a Master Fader is a fader with mute and its devices.
    if (kind == session::TrackKind::midi || kind == session::TrackKind::vca || kind == session::TrackKind::masterFader)
    {
        for (auto* c : std::initializer_list<juce::Component*>{&output_, &addSend_, &pan_, &phase_, &meter_})
            c->setVisible(false);
        panAllowed_ = false;
        for (auto& control : sends_)
            control->level.setVisible(false);
        if (kind != session::TrackKind::masterFader)
            devices_.setVisible(false);
        if (kind == session::TrackKind::midi)
            fader_.setVisible(false);
        else
        {
            arm_.setVisible(false);
            monitor_.setVisible(false);
        }
        if (kind == session::TrackKind::masterFader)
            solo_.setVisible(false);
    }

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
    // Inputs: none, each channel (mono), each pair (stereo); a multichannel track takes as many
    // consecutive inputs as it has channels.
    input_.clear(juce::dontSendNotification);
    input_.addItem("No input", 1);
    const auto names = inputNames_ ? inputNames_() : juce::StringArray();
    const auto format = session::track::getFormat(track_);
    const int in = session::track::getInput(track_);
    if (session::isMultichannel(format))
    {
        const int n = session::getNumChannels(format);
        for (int ch = 0; ch + n <= std::max(names.size(), n); ch += n)
            input_.addItem("In " + juce::String(ch + 1) + "-" + juce::String(ch + n) + " (" + session::getDisplayName(format) + ")"
                               + (names.size() < ch + n ? " (not on this device)" : ""),
                           2000 + ch);
        input_.setSelectedId(in < 0 ? 1 : 2000 + in, juce::dontSendNotification);
        if (in >= 0 && input_.getSelectedId() != 2000 + in)
        {
            input_.addItem("In " + juce::String(in + 1) + "-" + juce::String(in + n), 2000 + in);
            input_.setSelectedId(2000 + in, juce::dontSendNotification);
        }
        rebuildOutputs();
        return;
    }
    for (int ch = 0; ch < names.size(); ++ch)
        input_.addItem("In " + juce::String(ch + 1) + " (" + names[ch] + ")", ch + 2);
    for (int ch = 0; ch + 1 < names.size(); ch += 2)
        input_.addItem("In " + juce::String(ch + 1) + "/" + juce::String(ch + 2), 1000 + ch);
    if (names.isEmpty())
        input_.addItem("In 1/2 (no device inputs)", 1000);

    input_.setSelectedId(in < 0 ? 1 : (session::track::isInputStereo(track_) ? 1000 + in : in + 2), juce::dontSendNotification);
    rebuildOutputs();
}

void ChannelStrip::rebuildOutputs()
{

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

int ChannelStrip::getDestinationChannels() const
{
    const auto destination = session_.getDestination(track_);
    return destination.isValid() ? session::getNumChannels(session::track::getFormat(destination))
                                 : session::getNumChannels(session_.getMasterFormat());
}

void ChannelStrip::updateSurroundMode()
{
    // A mono or stereo signal into a surround destination is placed with the surround panner; a
    // multichannel track keeps the pan knob (as a balance).
    const bool surround = panAllowed_ && getDestinationChannels() > 2 && !session::isMultichannel(session::track::getFormat(track_));
    if (surround)
    {
        const auto destination = session_.getDestination(track_);
        surround_.setLayout(session::getLayout(destination.isValid() ? session::track::getFormat(destination) : session_.getMasterFormat()));
    }
    if (surround == surroundMode_ && surround_.isVisible() == surround)
        return;
    surroundMode_ = surround;
    surround_.setVisible(surround);
    pan_.setVisible(panAllowed_ && !surround);
    resized();
}

void ChannelStrip::syncFromModel()
{
    updateSurroundMode();
    surround_.setPosition(session::track::getPan(track_), session::track::getPanFrontRear(track_), session::track::getPanCentre(track_));
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
    // An inactive track's controls do nothing; the strip stays selectable (to make it active again).
    const bool active = session::Session::isEffectivelyActive(track_);
    for (auto* child : getChildren())
        child->setEnabled(active);
    repaint();
}

void ChannelStrip::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree == track_ && (property == session::ids::output || property == session::ids::input || property == session::ids::inputStereo
                           || property == session::ids::format))
        rebuildRouting();
    if (property == session::ids::masterFormat || property == session::ids::format || property == session::ids::output)
        updateSurroundMode(); // (this track's destination, or its format, may have changed)
    if (tree == track_ || tree.getParent() == track_ || property == session::ids::active) // (a folder around it)
        syncFromModel();
    else if (tree.hasType(session::ids::track) && property == session::ids::name)
        rebuildRouting();
}

void ChannelStrip::showMovingValues(float volumeGain, float pan)
{
    const bool volumeMoves = !std::isnan(volumeGain);
    const bool panMoves = !std::isnan(pan);
    if (volumeMoves && !faderDragging_)
        fader_.setValue(juce::jlimit(static_cast<double>(kFaderMinDb), static_cast<double>(kFaderMaxDb), juce::Decibels::gainToDecibels(static_cast<double>(volumeGain), static_cast<double>(kFaderMinDb))),
                        juce::dontSendNotification);
    else if (!volumeMoves && movingVolume_)
        fader_.setValue(session::track::getVolumeDb(track_), juce::dontSendNotification); // back to its own setting
    if (panMoves && !panDragging_)
        pan_.setValue(juce::jlimit(-1.0, 1.0, static_cast<double>(pan)), juce::dontSendNotification);
    else if (!panMoves && movingPan_)
        pan_.setValue(session::track::getPan(track_), juce::dontSendNotification);
    if (volumeMoves != movingVolume_ || panMoves != movingPan_ || volumeMoves || panMoves)
    {
        movingVolume_ = volumeMoves;
        movingPan_ = panMoves;
        repaint(readoutArea_);
    }
}

void ChannelStrip::pushLevels(float peakLeft, float peakRight, double elapsedSeconds)
{
    meter_.update(peakLeft, peakRight, elapsedSeconds);
}

void ChannelStrip::pushLevels(const float* peaks, int numChannels, double elapsedSeconds)
{
    meter_.update(peaks, numChannels, elapsedSeconds);
}

void ChannelStrip::showVolumeEditor()
{
    valueEditor_.open(volumeArea_.withSizeKeepingCentre(volumeArea_.getWidth() - 4, 17), formatDecibels(fader_.getValue()),
                      [this](const juce::String& text)
                      {
                          if (const auto decibels = mixer::parseVolume(text))
                              fader_.setValue(*decibels, juce::sendNotificationSync); // one undo step
                          repaint();
                      });
}

void ChannelStrip::showPanEditor()
{
    valueEditor_.open(panArea_.withSizeKeepingCentre(panArea_.getWidth() - 4, 17), formatPan(pan_.getValue()),
                      [this](const juce::String& text)
                      {
                          if (const auto pan = mixer::parsePan(text))
                              pan_.setValue(*pan, juce::sendNotificationSync);
                          repaint();
                      });
}

void ChannelStrip::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (fader_.isVisible() && volumeArea_.contains(e.getPosition()))
        showVolumeEditor();
    else if (pan_.isVisible() && panArea_.contains(e.getPosition()))
        showPanEditor();
}

void ChannelStrip::mouseDown(const juce::MouseEvent& e)
{
    // Ctrl / Shift on the strip itself select like in the arranger's track headers.
    const auto trackId = session::track::getId(track_);
    if (e.eventComponent == this && (e.mods.isCommandDown() || e.mods.isShiftDown()) && !e.mods.isPopupMenu())
    {
        std::vector<std::uint32_t> order;
        for (const auto& t : session_.getAllTracks())
            order.push_back(session::track::getId(t));
        editor_.selectTrackWithModifiers(trackId, e.mods.isCommandDown(), e.mods.isShiftDown(), order);
        return;
    }
    editor_.selectTrack(trackId);

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

    if (surroundMode_)
        surround_.setBounds(area.removeFromTop(58).reduced(2, 0));
    else
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
    volumeArea_ = readoutArea_.withWidth(readoutArea_.getWidth() / 2);
    panArea_ = readoutArea_.withTrimmedLeft(volumeArea_.getWidth());
    area.removeFromBottom(2);
    meter_.setBounds(area.removeFromRight(getDestinationChannels() > 2 ? 28 : 14)); // a bar per channel
    area.removeFromRight(2);
    fader_.setBounds(area);
}

void ChannelStrip::paint(juce::Graphics& g)
{
    using namespace theme;

    const bool selected = editor_.isTrackSelected(session::track::getId(track_));
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
    // Volume and pan, each double-clickable to type a value (in the accent colour while an LFO or
    // automation moves them).
    if (fader_.isVisible())
    {
        g.setColour(movingVolume_ ? colour::accentBright : colour::textMuted);
        g.drawText(formatDecibels(movingVolume_ ? fader_.getValue() : session::track::getVolumeDb(track_)), volumeArea_.withTrimmedRight(5),
                   juce::Justification::centredRight, false);
    }
    if (pan_.isVisible())
    {
        g.setColour(movingPan_ ? colour::accentBright : colour::textMuted);
        g.drawText(formatPan(movingPan_ ? pan_.getValue() : session::track::getPan(track_)), panArea_.withTrimmedLeft(5),
                   juce::Justification::centredLeft, false);
    }
}

void ChannelStrip::paintOverChildren(juce::Graphics& g)
{
    if (session::Session::isEffectivelyActive(track_))
        return;
    using namespace theme;
    g.setColour(colour::windowBackground.withAlpha(0.55f));
    g.fillRect(getLocalBounds().withTrimmedTop(nameArea_.getBottom()));
    const auto badge = juce::Rectangle<int>(0, nameArea_.getBottom() + 4, getWidth(), 16).reduced(10, 0);
    g.setColour(colour::control);
    g.fillRoundedRectangle(badge.toFloat(), 3.0f);
    g.setColour(colour::textSecondary);
    g.setFont(label(9.5f));
    g.drawText("INACTIVE", badge, juce::Justification::centred, false);
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

    // The master's format: stereo, or a surround format (tracks are panned and converted into it;
    // the audio device gets one output per channel, or a stereo fold-down).
    const auto formats = session::getAllTrackFormats();
    for (std::size_t i = 1; i < formats.size(); ++i) // (never mono)
        format_.addItem(session::getDisplayName(formats[i]), static_cast<int>(i) + 1);
    format_.setSelectedId(static_cast<int>(session_.getMasterFormat()) + 1, juce::dontSendNotification);
    format_.setTooltip("Master format: stereo or surround. With fewer device outputs than channels you hear a stereo fold-down.");
    format_.setWantsKeyboardFocus(false);
    format_.onChange = [this]
    {
        const auto all = session::getAllTrackFormats();
        const int index = format_.getSelectedId() - 1;
        if (index < 1 || index >= static_cast<int>(all.size()) || all[static_cast<std::size_t>(index)] == session_.getMasterFormat())
            return;
        session_.beginTransaction("Master Format");
        session_.setMasterFormat(all[static_cast<std::size_t>(index)]);
    };
    addAndMakeVisible(format_);

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
    if (tree == session_.getState() && property == session::ids::masterFormat)
    {
        format_.setSelectedId(static_cast<int>(session_.getMasterFormat()) + 1, juce::dontSendNotification);
        resized();
    }
}

void MasterStrip::valueTreeRedirected(juce::ValueTree&)
{
    fader_.setValue(session_.getMasterVolumeDb(), juce::dontSendNotification);
    format_.setSelectedId(static_cast<int>(session_.getMasterFormat()) + 1, juce::dontSendNotification);
    resized();
}

void MasterStrip::showMovingVolume(float gain)
{
    const bool moves = !std::isnan(gain);
    if (moves && !dragging_)
        fader_.setValue(juce::jlimit(static_cast<double>(kFaderMinDb), static_cast<double>(kFaderMaxDb), juce::Decibels::gainToDecibels(static_cast<double>(gain), static_cast<double>(kFaderMinDb))),
                        juce::dontSendNotification);
    else if (!moves && moving_)
        fader_.setValue(session_.getMasterVolumeDb(), juce::dontSendNotification);
    if (moves || moving_)
        repaint(readoutArea_);
    moving_ = moves;
}

void MasterStrip::pushLevels(float peakLeft, float peakRight, double elapsedSeconds)
{
    const std::array<float, 2> peaks{peakLeft, peakRight};
    pushLevels(peaks.data(), 2, elapsedSeconds);
}

void MasterStrip::pushLevels(const float* peaks, int numChannels, double elapsedSeconds)
{
    meter_.update(peaks, numChannels, elapsedSeconds);

    float loudest = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch)
        loudest = juce::jmax(loudest, peaks[ch]);
    const float peakDb = juce::Decibels::gainToDecibels(loudest, LevelMeter::kMinDb);
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
    format_.setBounds(area.removeFromTop(22));
    area.removeFromTop(4);
    readoutArea_ = area.removeFromBottom(40);
    gainArea_ = readoutArea_.withWidth(readoutArea_.getWidth() / 2).reduced(2, 4);
    area.removeFromBottom(6);

    // A surround master meters every channel: a wider meter.
    meter_.setBounds(area.removeFromRight(session::isMultichannel(session_.getMasterFormat()) ? 44 : 22));
    scaleArea_ = area.removeFromRight(26);
    fader_.setBounds(area);
}

void MasterStrip::showGainEditor()
{
    valueEditor_.open(gainArea_.withTrimmedBottom(12).withSizeKeepingCentre(gainArea_.getWidth(), 18), formatDecibels(fader_.getValue()),
                      [this](const juce::String& text)
                      {
                          if (const auto decibels = mixer::parseVolume(text))
                              fader_.setValue(*decibels, juce::sendNotificationSync);
                          repaint();
                      });
}

void MasterStrip::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (gainArea_.contains(e.getPosition()))
        showGainEditor();
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

    readout.removeFromLeft(readout.getWidth() / 2);
    paintReadout(gainArea_, formatDecibels(fader_.getValue()), "GAIN", colour::textPrimary);
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

void MixerPanel::scrollToTrack(const juce::ValueTree& track)
{
    if (strips_.size() != static_cast<std::size_t>(project_.getSession().getNumTracks()))
        rebuildStrips(); // (a rebuild may still be pending)
    for (std::size_t i = 0; i < strips_.size(); ++i)
        if (strips_[i]->getTrack() == track)
            viewport_.setViewPosition(static_cast<int>(i) * kStripWidth, 0);
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
    master_.showMovingVolume(controller_.getEngine().getMovingMasterVolume());
    if (telemetry.masterChannels > 2)
        master_.pushLevels(telemetry.masterPeaks.data(), telemetry.masterChannels, elapsedSeconds);
    else
        master_.pushLevels(telemetry.masterPeakLeft, telemetry.masterPeakRight, elapsedSeconds);

    // A strip meters what the track sends on: its destination's channels (a bus or the master).
    const auto& session = project_.getSession();
    const auto masterChannels = session::getNumChannels(session.getMasterFormat());
    for (auto& strip : strips_)
    {
        const auto& t = strip->getTrack();
        const int slot = project_.getCompiler().getSlot(session::track::getId(t));
        const auto destination = session.getDestination(t);
        const int channels = destination.isValid() ? std::max(2, session::getNumChannels(session::track::getFormat(destination))) : masterChannels;
        const auto peaks = slot >= 0 ? controller_.collectTrackPeaks(slot) : engine::ChannelPeaks{};
        // Faders and pans follow an LFO or automation as the engine applies them.
        strip->showMovingValues(controller_.getEngine().getMovingVolume(slot), controller_.getEngine().getMovingPan(slot));
        if (channels > 2)
            strip->pushLevels(peaks.peak.data(), channels, elapsedSeconds);
        else
            strip->pushLevels(peaks.peak[0], peaks.peak[1], elapsedSeconds);
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
    g.drawText("Double-click to reset, double-click a value to type it  |  M mute  S solo  " + juce::String::fromUTF8("\xc3\x98") +
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

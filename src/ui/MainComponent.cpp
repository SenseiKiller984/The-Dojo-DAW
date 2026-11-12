#include "ui/MainComponent.h"

#include "core/MusicalTime.h"
#include "core/rt/RealtimeGuard.h"
#include "ui/Commands.h"
#include "ui/EditActions.h"
#include "engine/AudioEngine.h"
#include "ui/DemoProject.h"
#include "ui/components/ExportDialog.h"
#include "ui/components/LevelMeter.h"
#include "ui/components/TrackDialogs.h"
#include "ui/theme/DojoTheme.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <iterator>

namespace dojo::ui
{
namespace
{
constexpr int kBottomTabHeight = 26;
constexpr int kDividerHeight = 5;
constexpr double kStatusRefreshIntervalSeconds = 0.25;
constexpr double kMaxFrameDeltaSeconds = 0.1; // clamp after stalls so meters don't jump

juce::File defaultSessionFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}
} // namespace

//==================================================================================================
/** The bar between the arranger and the bottom panel: drag to resize. */
class MainComponent::Divider final : public juce::Component
{
public:
    explicit Divider(std::function<void(int)> onDrag)
        : onDrag_(std::move(onDrag))
    {
        setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(theme::colour::windowBackground);
        g.setColour(theme::colour::outlineStrong);
        g.fillRect(getLocalBounds().withSizeKeepingCentre(40, 2));
    }

    void mouseDown(const juce::MouseEvent& e) override { startY_ = e.getScreenY(); }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        onDrag_(startY_ - e.getScreenY());
        startY_ = e.getScreenY();
    }

private:
    std::function<void(int)> onDrag_;
    int startY_ = 0;
};

MainComponent::MainComponent(AppServices& services)
    : services_(services)
    , project_(services.project)
    , controller_(services.controller)
    , commandManager_(services.commands)
    , transport_(services.controller, services.project.getSession(), editor_)
    , browser_(services.project.getAssets().getWildcard(), services.plugins, services.preview, *services.settings.getUserSettings())
    , arranger_(services.project, services.controller, editor_)
    , mixer_(services.project, services.controller, editor_)
    , devicePanel_(services.project, services.plugins, editor_)
    , clipEditor_(services.project, editor_)
    , clipList_(services.project.getSession(), editor_)
    , vblank_(this, [this](double timestampSeconds) { onDisplayRefresh(timestampSeconds); })
{
    setOpaque(true);
    setWantsKeyboardFocus(true);

    addAndMakeVisible(transport_);
    addAndMakeVisible(browser_);
    addAndMakeVisible(arranger_);
    addAndMakeVisible(mixer_);
    addChildComponent(devicePanel_);
    addChildComponent(clipEditor_);
    addChildComponent(clipList_);
    addAndMakeVisible(statusBar_);

    divider_ = std::make_unique<Divider>([this](int delta)
                                         {
                                             bottomHeight_ = juce::jlimit(160, std::max(160, getHeight() - 260), bottomHeight_ + delta);
                                             resized();
                                         });
    addAndMakeVisible(*divider_);

    mixerTab_.setButtonText("MIXER");
    devicesTab_.setButtonText("DEVICES");
    editorTab_.setButtonText("EDITOR");
    clipsTab_.setButtonText("CLIPS");
    mixerTab_.setTooltip("Mixer (F3)");
    devicesTab_.setTooltip("Devices of the selected track (F4)");
    editorTab_.setTooltip("Piano roll / step sequencer / clip properties (F5)");
    clipsTab_.setTooltip("Clip List: captured and user-defined clips, drag them onto tracks (F6)");
    for (auto* b : {&mixerTab_, &devicesTab_, &editorTab_, &clipsTab_})
    {
        b->setRadioGroupId(9191);
        b->setClickingTogglesState(true);
        b->setWantsKeyboardFocus(false);
        b->setColour(juce::TextButton::buttonOnColourId, theme::colour::control);
        addAndMakeVisible(b);
    }
    mixerTab_.onClick = [this] { showBottomTab(BottomTab::mixer); };
    devicesTab_.onClick = [this] { showBottomTab(BottomTab::devices); };
    editorTab_.onClick = [this] { showBottomTab(BottomTab::editor); };
    clipsTab_.onClick = [this] { showBottomTab(BottomTab::clips); };
    mixerTab_.setToggleState(true, juce::dontSendNotification);

    transport_.onRecordClicked = [this] { toggleRecording(); };
    arranger_.onOpenClip = [this](std::uint32_t clipId)
    {
        editor_.setEditedClip(clipId);
        showBottomTab(BottomTab::editor);
    };
    arranger_.onDeviceDropped = [this](const juce::ValueTree& target, const juce::String& description)
    {
        if (const auto choice = deviceActions::fromDragDescription(description, services_.plugins))
            addDevice(*choice, target);
    };
    browser_.onDeviceActivated = [this](const deviceActions::DeviceChoice& choice)
    {
        auto& session = project_.getSession();
        addDevice(choice, editor_.isMasterSelected() ? session.getMaster() : session.findTrack(editor_.getSelectedTrack()));
    };
    mixer_.onShowDevices = [this] { showBottomTab(BottomTab::devices); };
    services_.recorder.onStateChanged = [this]
    {
        transport_.setRecording(services_.recorder.isRecording());
        commandManager_.commandStatusChanged();
    };

    transport_.onReturnToStart = [this] { goToStart(); };
    transport_.onTogglePlayback = [this] { togglePlayback(); };
    transport_.onAudioSettingsRequested = [this]
    {
        showAudioSettings();
    };
    arranger_.onImportRequested = [this]
    {
        showImportChooser();
    };
    arranger_.onCommand = [this](int commandId) { commandManager_.invokeDirectly(commandId, true); };
    arranger_.setVideoFrames(&videoFrames_);
    clipList_.setSettings(services_.settings.getUserSettings());
    clipEditor_.onPreviewMidi = [this](std::uint32_t trackId, const juce::MidiMessage& message)
    { (void)controller_.getEngine().getLiveMidiQueue().push(message, trackId); };
    clipList_.getPlayheadSeconds = [this] { return arranger_.getPlayheadSeconds(); };
    arranger_.onVideoImported = [this] { showVideoWindow(true); };
    browser_.onFileActivated = [this](const juce::File& file)
    {
        importAudioFiles({file.getFullPathName()});
    };

    project_.onImportProblem = [this](const juce::String& problem)
    {
        showMessage("Some audio could not be loaded", problem);
    };

    project_.getSession().getUndoManager().addChangeListener(this);

    refreshStatusBar();
}

MainComponent::~MainComponent()
{
    // The video decoder outlives nothing that watches it.
    videoWindow_.reset();
    arranger_.setVideoFrames(nullptr);
    services_.recorder.onStateChanged = nullptr;
    project_.getSession().getUndoManager().removeChangeListener(this);
    project_.onImportProblem = nullptr;

    if (settingsWindow_ != nullptr)
        delete settingsWindow_.getComponent();
}

void MainComponent::togglePlayback()
{
    const auto& telemetry = controller_.getLastTelemetry();
    if (!telemetry.playing && telemetry.stoppedAtEnd && services_.preferences.getReturnViewToStart())
        arranger_.scrollToTime(0.0);
    controller_.togglePlayback();
}

void MainComponent::goToStart()
{
    controller_.returnToStart();
    if (services_.preferences.getReturnViewToStart())
        arranger_.scrollToTime(0.0);
}

void MainComponent::focusGained(FocusChangeType)
{
    arranger_.grabKeyboardFocus();
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(theme::colour::windowBackground);
}

void MainComponent::resized()
{
    using namespace theme::metrics;

    auto area = getLocalBounds();

    transport_.setBounds(area.removeFromTop(transportHeight));
    statusBar_.setBounds(area.removeFromBottom(statusBarHeight));
    browser_.setBounds(area.removeFromLeft(browserWidth));

    const int bottom = juce::jlimit(160, juce::jmax(160, area.getHeight() - 220), bottomHeight_);
    auto bottomArea = area.removeFromBottom(bottom);
    auto tabs = bottomArea.removeFromTop(kBottomTabHeight).reduced(6, 2);
    for (auto* b : {&mixerTab_, &devicesTab_, &editorTab_, &clipsTab_})
        b->setBounds(tabs.removeFromLeft(86));
    divider_->setBounds(area.removeFromBottom(kDividerHeight));
    for (auto* panel : std::initializer_list<juce::Component*>{&mixer_, &devicePanel_, &clipEditor_, &clipList_})
        panel->setBounds(bottomArea);
    arranger_.setBounds(area);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    commandManager_.commandStatusChanged(); // refresh Undo/Redo menu items
}

//==================================================================================================
// Frame loop

void MainComponent::onDisplayRefresh(double timestampSeconds)
{
    const double elapsed = lastFrameTimestamp_ > 0.0 ? juce::jlimit(0.0, kMaxFrameDeltaSeconds,
                                                                    timestampSeconds - lastFrameTimestamp_)
                                                     : 1.0 / 60.0;
    lastFrameTimestamp_ = timestampSeconds;
    ++frameCount_;

    const auto& telemetry = controller_.pollTelemetry();
    const double sampleRate = controller_.getSampleRateOr(48000.0);

    transport_.update(telemetry, sampleRate);
    arranger_.setPlayhead(telemetry.playheadSeconds, telemetry.playing);
    arranger_.setRecording(services_.recorder.isRecording() && controller_.getEngine().getCaptureStartSeconds() >= 0.0,
                           controller_.getEngine().getCaptureStartSeconds());
    if (clipEditor_.isVisible())
        clipEditor_.setPlayhead(telemetry.playheadSeconds);
    mixer_.updateMeters(telemetry, elapsed);
    loadPeakSinceStatusRefresh_ = juce::jmax(loadPeakSinceStatusRefresh_, telemetry.dspLoadPeak);

    if (timestampSeconds - lastStatusRefresh_ >= kStatusRefreshIntervalSeconds)
    {
        lastStatusRefresh_ = timestampSeconds;
        refreshStatusBar();
    }
}

void MainComponent::refreshStatusBar()
{
    StatusBar::Info info;
    info.device = controller_.getDeviceStatus();
    info.dspLoadPeak = loadPeakSinceStatusRefresh_;
    info.realtimeViolations = rt::getRealtimeViolationCount();
    info.realtimeTrapInstalled = rt::isAllocationTrapInstalled();
    info.clippedSamples = controller_.getLastTelemetry().clippedSamples;
    info.parkedCommands = controller_.getParkedCommandCount();
    info.pendingImports = project_.getPendingImportCount();
    info.recording = services_.recorder.isRecording();
    info.scanningPlugins = services_.plugins.isScanning();
    info.midiInputs = controller_.getDeviceHost().getMidiInputNames().size();
    info.latencySamples = project_.getCompiler().getTotalLatency();
    statusBar_.setInfo(info);
    loadPeakSinceStatusRefresh_ = 0.0f;
}

//==================================================================================================
// Commands

void MainComponent::getAllCommands(juce::Array<juce::CommandID>& c)
{
    using namespace commands;
    const juce::CommandID ids[] = {newSession,
                                   openSession,
                                   saveSession,
                                   saveSessionAs,
                                   importAudio,
                                   juce::StandardApplicationCommandIDs::quit,
                                   undo,
                                   redo,
                                   deleteSelection,
                                   duplicateSelection,
                                   splitAtPlayhead,
                                   selectAllClips,
                                   addTrack,
                                   deleteTrack,
                                   playStop,
                                   returnToStart,
                                   toggleSnap,
                                   zoomIn,
                                   zoomOut,
                                   zoomToFit,
                                   audioSettings,
                                   preferences,
                                   toggleTestTone,
                                   showShortcuts,
                                   record,
                                   toggleLoop,
                                   exportAudio,
                                   addInstrumentTrack,
                                   addBusTrack,
                                   addFolder,
                                   groupIntoFolder,
                                   showMixer,
                                   showDevices,
                                   showEditor,
                                   scanPlugins,
                                   openDemo,
                                   cutClips,
                                   copyClips,
                                   pasteClips,
                                   newTracks,
                                   duplicateTracks,
                                   splitIntoMono,
                                   toggleTrackActive,
                                   scrollToTrack,
                                   togglePunchIn,
                                   togglePunchOut,
                                   toggleRecordMonitoring,
                                   clearClipIndicators,
                                   createClickTrack,
                                   toggleEditLock,
                                   toggleTimeLock,
                                   importVideo,
                                   showVideo,
                                   clipRatingNone,
                                   clipRating1,
                                   clipRating2,
                                   clipRating3,
                                   clipRating4,
                                   clipRating5,
                                   toggleClipRatings,
                                   groupClips,
                                   ungroupClips,
                                   ungroupAllClips,
                                   regroupClips,
                                   loopClips,
                                   unloopClips,
                                   captureClip,
                                   renameClip,
                                   identifySyncPoint,
                                   removeSyncPoint,
                                   quantizeClipsToGrid,
                                   bypassClipGain,
                                   renderClipGain,
                                   showClipList,
                                   toggleFullScreen};
    c.addArray(ids, static_cast<int>(std::size(ids)));
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using namespace commands;
    constexpr auto cmd = juce::ModifierKeys::commandModifier;
    constexpr auto shift = juce::ModifierKeys::shiftModifier;
    constexpr auto alt = juce::ModifierKeys::altModifier;
    auto& session = project_.getSession();
    auto& undoManager = session.getUndoManager();
    const bool hasClipSelection = !editor_.getSelectedClips().empty();

    switch (id)
    {
    case newSession:
        info.setInfo("New Session", "Start an empty session", categories::file, 0);
        info.addDefaultKeypress('n', cmd);
        break;
    case openSession:
        info.setInfo("Open Session...", "Open a .dojo session file", categories::file, 0);
        info.addDefaultKeypress('o', cmd);
        break;
    case saveSession:
        info.setInfo("Save", "Save the session", categories::file, 0);
        info.addDefaultKeypress('s', cmd);
        break;
    case saveSessionAs:
        info.setInfo("Save As...", "Save the session under a new name", categories::file, 0);
        info.addDefaultKeypress('s', cmd | shift);
        break;
    case importAudio:
        info.setInfo("Import Audio...", "Import audio files as clips", categories::file, 0);
        info.addDefaultKeypress('i', cmd);
        break;
    case importVideo:
        info.setInfo("Import Video...", "Import a video file onto a video track (MP4, MOV, AVI, MKV, ...)", categories::file, 0);
        info.addDefaultKeypress('i', cmd | shift);
        break;
    case showVideo:
        info.setInfo("Video", "Show the Video window: the picture of the video track at the playhead", categories::view, 0);
        info.setTicked(videoWindow_ != nullptr && videoWindow_->isVisible());
        info.addDefaultKeypress('v', cmd | shift);
        break;
    case juce::StandardApplicationCommandIDs::quit:
        info.setInfo("Quit", "Quit The Dojo", categories::file, 0);
        info.addDefaultKeypress('q', cmd);
        break;

    case undo:
        info.setInfo(undoManager.canUndo() ? "Undo " + undoManager.getUndoDescription()
                                           : juce::String("Undo"),
                     "Undo the last edit", categories::edit, 0);
        info.setActive(undoManager.canUndo());
        info.addDefaultKeypress('z', cmd);
        break;
    case redo:
        info.setInfo(undoManager.canRedo() ? "Redo " + undoManager.getRedoDescription()
                                           : juce::String("Redo"),
                     "Redo the last undone edit", categories::edit, 0);
        info.setActive(undoManager.canRedo());
        info.addDefaultKeypress('z', cmd | shift);
        info.addDefaultKeypress('y', cmd);
        break;
    case deleteSelection:
        info.setInfo("Delete", "Delete the selected clips", categories::edit, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress(juce::KeyPress::deleteKey, 0);
        info.addDefaultKeypress(juce::KeyPress::backspaceKey, 0);
        break;
    case duplicateSelection:
        info.setInfo("Duplicate", "Duplicate the selected clips", categories::edit, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('d', cmd);
        break;
    case splitAtPlayhead:
        info.setInfo("Split at Playhead", "Split clips at the playhead", categories::edit, 0);
        info.addDefaultKeypress('e', cmd);
        break;
    case selectAllClips:
        info.setInfo("Select All Clips", "Select every clip", categories::edit, 0);
        info.addDefaultKeypress('a', cmd);
        break;

    case addTrack:
        info.setInfo("Add Audio Track", "Add an audio track", categories::track, 0);
        info.addDefaultKeypress('t', cmd);
        break;
    case deleteTrack:
        info.setInfo(actions::getSelectedTracks(session, editor_).size() > 1 ? "Delete Tracks" : "Delete Track",
                     "Delete the selected tracks and their clips", categories::track, 0);
        info.setActive(!actions::getSelectedTracks(session, editor_).empty());
        break;

    case playStop:
        info.setInfo(controller_.getLastTelemetry().playing ? "Stop" : "Play", "Start or stop playback",
                     categories::transport, 0);
        info.addDefaultKeypress(juce::KeyPress::spaceKey, 0);
        break;
    case returnToStart:
        info.setInfo("Return to Start", "Move the playhead to the start", categories::transport, 0);
        info.addDefaultKeypress(juce::KeyPress::homeKey, 0);
        info.addDefaultKeypress(juce::KeyPress::returnKey, 0);
        break;
    case toggleSnap:
        info.setInfo("Snap to Grid", "Toggle grid snapping", categories::view, 0);
        info.setTicked(editor_.isSnapEnabled());
        info.addDefaultKeypress('g', cmd);
        break;
    case zoomIn:
        info.setInfo("Zoom In", "Zoom the timeline in", categories::view, 0);
        info.addDefaultKeypress('=', cmd);
        info.addDefaultKeypress('+', cmd);
        break;
    case zoomOut:
        info.setInfo("Zoom Out", "Zoom the timeline out", categories::view, 0);
        info.addDefaultKeypress('-', cmd);
        break;
    case zoomToFit:
        info.setInfo("Zoom to Fit", "Show the whole session", categories::view, 0);
        info.addDefaultKeypress('0', cmd);
        break;

    case newTracks:
        info.setInfo("New...", "Add one or more audio tracks, aux inputs, instrument tracks or folders", categories::track, 0);
        info.addDefaultKeypress('n', cmd | shift);
        break;
    case duplicateTracks:
        info.setInfo("Duplicate...", "Copy the selected tracks: how many, and with what", categories::track, 0);
        info.setActive(!actions::getSelectedTracks(session, editor_).empty());
        info.addDefaultKeypress('d', juce::ModifierKeys::altModifier | shift);
        break;
    case splitIntoMono:
        info.setInfo("Split into Mono", "Make a mono track for each channel of the selected stereo and multichannel tracks", categories::track, 0);
        info.setActive(!actions::getSplittableTracks(session, editor_).empty());
        break;
    case toggleTrackActive:
    {
        const bool anyActive = actions::anySelectedTrackActive(session, editor_);
        info.setInfo(anyActive ? "Make Inactive" : "Make Active",
                     "Inactive tracks do not play and their devices use no processing power", categories::track, 0);
        info.setActive(!actions::getSelectedTracks(session, editor_).empty());
        break;
    }
    case scrollToTrack:
        info.setInfo("Scroll to Track...", "Bring a track (by its number) into view", categories::track, 0);
        info.setActive(session.getNumTracks() > 0);
        break;
    case createClickTrack:
        info.setInfo("Create Click Track", "Add an aux input with the Click (metronome) device", categories::track, 0);
        break;
    case clearClipIndicators:
        info.setInfo("Clear All Clip Indicators", "Clear the clip indication of every meter", categories::track, 0);
        info.addDefaultKeypress('c', juce::ModifierKeys::altModifier);
        break;
    case toggleEditLock:
    {
        const bool locked = actions::selectedClipsLocked(session, editor_, actions::ClipLock::edit);
        info.setInfo(locked ? "Edit Unlock" : "Edit Lock",
                     "Edit-locked clips cannot be edited, moved or deleted; copies of them stay edit-locked", categories::clip, 0);
        info.setActive(hasClipSelection);
        info.setTicked(locked);
        info.addDefaultKeypress('l', cmd | shift);
        break;
    }
    case toggleTimeLock:
    {
        const bool locked = actions::selectedClipsLocked(session, editor_, actions::ClipLock::time);
        info.setInfo(locked ? "Time Unlock" : "Time Lock",
                     "Time-locked clips stay at their time location: they can be trimmed, split or moved to another track, "
                     "but not moved in time or deleted",
                     categories::clip, 0);
        info.setActive(hasClipSelection);
        info.setTicked(locked);
        info.addDefaultKeypress('l', cmd | juce::ModifierKeys::altModifier);
        break;
    }
    case clipRatingNone:
    case clipRating1:
    case clipRating2:
    case clipRating3:
    case clipRating4:
    case clipRating5:
    {
        const int rating = static_cast<int>(id - clipRatingNone);
        info.setInfo(rating == 0 ? juce::String("Rating: None") : "Rating: " + juce::String(rating),
                     rating == 0 ? juce::String("Remove the rating of the selected clips")
                                 : "Rate the selected clips " + juce::String(rating) + " (of 5)",
                     categories::clip, 0);
        info.setActive(hasClipSelection);
        info.setTicked(hasClipSelection && actions::getSelectedClipsRating(session, editor_) == rating);
        info.addDefaultKeypress('0' + rating, cmd | alt);
        break;
    }
    case toggleClipRatings:
        info.setInfo("Clip Ratings", "Show the rating (stars) of clips in their title bars", categories::view, 0);
        info.setTicked(editor_.isShowingClipRatings());
        break;
    case groupClips:
        info.setInfo("Group", "Make a clip group of the selected clips (and groups), on any tracks", categories::clip, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('g', cmd | alt);
        break;
    case ungroupClips:
        info.setInfo("Ungroup", "Take the top layer of the selected clip groups apart (nested groups stay)", categories::clip, 0);
        info.setActive(actions::selectionHasGroups(session, editor_));
        info.addDefaultKeypress('u', cmd | alt);
        break;
    case ungroupAllClips:
        info.setInfo("Ungroup All", "Remove every layer of grouping from the selected clip groups", categories::clip, 0);
        info.setActive(actions::selectionHasGroups(session, editor_));
        break;
    case regroupClips:
        info.setInfo("Regroup", "Put back the clip groups the last Ungroup took apart", categories::clip, 0);
        info.setActive(!editor_.getLastUngroup().empty());
        info.addDefaultKeypress('r', cmd | alt);
        break;
    case loopClips:
        info.setInfo("Loop...", "Repeat the selected clips a number of times", categories::clip, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('o', cmd | alt);
        break;
    case unloopClips:
        info.setInfo("Unloop", "Turn looped clips back into standard clips, one per iteration", categories::clip, 0);
        info.setActive(actions::selectionHasLoopedClips(session, editor_));
        break;
    case captureClip:
        info.setInfo("Capture...", "Make the selection a new clip in the Clip List", categories::clip, 0);
        info.setActive(hasClipSelection || !editor_.getTimeSelection().isEmpty());
        info.addDefaultKeypress('c', cmd | alt);
        break;
    case renameClip:
        info.setInfo("Rename...", "Rename the selected clip (it becomes a user-defined clip in the Clip List)", categories::clip, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('r', cmd | shift);
        break;
    case identifySyncPoint:
        info.setInfo("Identify Sync Point", "Mark the playhead position in the selected clips as their sync point", categories::clip, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('s', cmd | alt);
        break;
    case removeSyncPoint:
        info.setInfo("Remove Sync Point", "Remove the sync point of the selected clips", categories::clip, 0);
        info.setActive(actions::selectionHasSyncPoints(session, editor_));
        break;
    case quantizeClipsToGrid:
        info.setInfo("Quantize to Grid", "Move the selected clips so that their sync point (or start) is on the nearest grid line",
                     categories::clip, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('q', cmd | alt);
        break;
    case bypassClipGain:
        info.setInfo("Bypass Clip Gain", "Play the selected clips without their clip gain (the setting is kept)", categories::clip, 0);
        info.setActive(hasClipSelection);
        info.setTicked(actions::selectedClipsGainBypassed(session, editor_));
        info.addDefaultKeypress('b', cmd | alt);
        break;
    case renderClipGain:
        info.setInfo("Render Clip Gain", "Apply the clip gain to the audio of the selected clips and reset it to 0 dB", categories::clip, 0);
        info.setActive(hasClipSelection);
        break;
    case toggleFullScreen:
        info.setInfo("Full Screen", "The window covers the whole screen (F11 again to go back)", categories::view, 0);
        info.setTicked(services_.isFullScreen && services_.isFullScreen());
        info.setActive(services_.toggleFullScreen != nullptr);
        info.addDefaultKeypress(juce::KeyPress::F11Key, 0);
        break;
    case showClipList:
        info.setInfo("Clip List", "Show the Clip List", categories::view, 0);
        info.setTicked(bottomTab_ == BottomTab::clips);
        info.addDefaultKeypress(juce::KeyPress::F6Key, 0);
        break;
    case togglePunchIn:
        info.setInfo("Punch In", "Recording starts at the loop start", categories::transport, 0);
        info.setTicked(session.isPunchInEnabled());
        break;
    case togglePunchOut:
        info.setInfo("Punch Out", "Recording ends at the loop end", categories::transport, 0);
        info.setTicked(session.isPunchOutEnabled());
        break;
    case toggleRecordMonitoring:
        // Named after what it switches to.
        if (session.isInputOnlyMonitoring())
            info.setInfo("Set Record Tracks to Auto Input",
                         "Record-enabled tracks hear their input while stopped and while punched in, and their clips otherwise",
                         categories::track, 0);
        else
            info.setInfo("Set Record Tracks to Input Only", "Record-enabled tracks always hear their input", categories::track, 0);
        break;

    case preferences:
        info.setInfo("Preferences...", "Playback options, themes, audio settings and keyboard shortcuts", categories::options, 0);
        info.addDefaultKeypress(',', cmd);
        break;
    case audioSettings:
        info.setInfo("Audio Settings...", "Choose the audio driver and device", categories::options, 0);
        info.addDefaultKeypress('a', cmd | shift);
        break;
    case toggleTestTone:
        info.setInfo("Test Tone", "Play a 440 Hz test tone through the master bus", categories::options, 0);
        info.setTicked(controller_.isTestToneEnabled());
        break;
    case showShortcuts:
        info.setInfo("Keyboard Shortcuts...", "See and change the keyboard shortcuts", categories::options, 0);
        info.addDefaultKeypress(juce::KeyPress::F1Key, 0);
        break;
    case record:
        info.setInfo(services_.recorder.isRecording() ? "Stop Recording" : "Record", "Record onto the armed tracks",
                     categories::transport, 0);
        info.setTicked(services_.recorder.isRecording());
        info.addDefaultKeypress('r', 0);
        break;
    case toggleLoop:
        info.setInfo("Loop", "Loop playback between the loop markers", categories::transport, 0);
        info.setTicked(session.isLoopEnabled());
        info.addDefaultKeypress('l', 0);
        break;
    case exportAudio:
        info.setInfo("Export Audio...", "Bounce the mix and/or stems to WAV, FLAC or MP3", categories::file, 0);
        info.addDefaultKeypress('r', cmd);
        break;
    case addInstrumentTrack:
        info.setInfo("Add Instrument Track", "Add an instrument track with a Sampler", categories::track, 0);
        info.addDefaultKeypress('t', cmd | shift);
        break;
    case addBusTrack:
        info.setInfo("Add Bus", "Add a bus (group or return)", categories::track, 0);
        info.addDefaultKeypress('b', cmd);
        break;
    case addFolder:
        info.setInfo("Add Folder", "Add a folder track", categories::track, 0);
        info.addDefaultKeypress('f', cmd | shift);
        break;
    case groupIntoFolder:
        info.setInfo("Group into Folder", "Put the selected tracks into a new folder", categories::track, 0);
        info.setActive(!actions::getSelectedTracks(session, editor_).empty());
        info.addDefaultKeypress('g', cmd | shift);
        break;
    case showMixer:
        info.setInfo("Mixer", "Show the mixer", categories::view, 0);
        info.setTicked(bottomTab_ == BottomTab::mixer);
        info.addDefaultKeypress(juce::KeyPress::F3Key, 0);
        break;
    case showDevices:
        info.setInfo("Devices", "Show the selected track's devices", categories::view, 0);
        info.setTicked(bottomTab_ == BottomTab::devices);
        info.addDefaultKeypress(juce::KeyPress::F4Key, 0);
        break;
    case showEditor:
        info.setInfo("Editor", "Show the piano roll / step sequencer / clip properties", categories::view, 0);
        info.setTicked(bottomTab_ == BottomTab::editor);
        info.addDefaultKeypress(juce::KeyPress::F5Key, 0);
        break;
    case scanPlugins:
        info.setInfo("Scan for Plug-ins", "Look for new VST3 and CLAP plug-ins", categories::options, 0);
        info.setActive(!services_.plugins.isScanning());
        break;
    case openDemo:
        info.setInfo("Open Demo Project", "A short song that uses every kind of track and device", categories::file, 0);
        break;
    case cutClips:
        info.setInfo("Cut", "Cut the selected clips", categories::edit, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('x', cmd);
        break;
    case copyClips:
        info.setInfo("Copy", "Copy the selected clips", categories::edit, 0);
        info.setActive(hasClipSelection);
        info.addDefaultKeypress('c', cmd);
        break;
    case pasteClips:
        info.setInfo("Paste", "Paste clips at the playhead on the selected track", categories::edit, 0);
        info.setActive(!editor_.getClipboard().empty());
        info.addDefaultKeypress('v', cmd);
        break;
    default:
        break;
    }
}

bool MainComponent::perform(const InvocationInfo& invocation)
{
    using namespace commands;
    auto& session = project_.getSession();

    switch (invocation.commandID)
    {
    case newSession:
        ensureSavedThen([this] { startNewSession(); });
        break;
    case openSession:
        ensureSavedThen([this] { showOpenChooser(); });
        break;
    case saveSession:
        save({});
        break;
    case saveSessionAs:
        saveAs({});
        break;
    case importVideo:
        showImportVideoChooser();
        break;
    case showVideo:
        showVideoWindow(videoWindow_ == nullptr || !videoWindow_->isVisible());
        break;
    case importAudio:
        showImportChooser();
        break;
    case juce::StandardApplicationCommandIDs::quit:
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
        break;

    case undo:
        session.getUndoManager().undo();
        break;
    case redo:
        session.getUndoManager().redo();
        break;
    case deleteSelection:
        actions::deleteSelectedClips(session, editor_);
        break;
    case duplicateSelection:
        actions::duplicateSelectedClips(session, editor_);
        break;
    case splitAtPlayhead:
        actions::splitAt(session, editor_, arranger_.getPlayheadSeconds());
        break;
    case selectAllClips:
        actions::selectAllClips(session, editor_);
        break;

    case addTrack:
        actions::addTrack(session, editor_);
        break;
    case deleteTrack:
        actions::deleteSelectedTracks(session, editor_);
        break;

    case playStop:
        togglePlayback();
        break;
    case returnToStart:
        goToStart();
        break;
    case toggleSnap:
        editor_.setSnapEnabled(!editor_.isSnapEnabled());
        break;
    case zoomIn:
        arranger_.zoomBy(1.5);
        break;
    case zoomOut:
        arranger_.zoomBy(1.0 / 1.5);
        break;
    case zoomToFit:
        arranger_.zoomToFit();
        break;

    case preferences:
        if (services_.showPreferences)
            services_.showPreferences(Preferences::Tab::general);
        break;
    case newTracks:
        showNewTracksDialog();
        break;
    case duplicateTracks:
        showDuplicateTracksDialog();
        break;
    case splitIntoMono:
        actions::splitSelectedIntoMono(project_, editor_);
        break;
    case toggleTrackActive:
        actions::setSelectedTracksActive(session, editor_, !actions::anySelectedTrackActive(session, editor_));
        break;
    case scrollToTrack:
        showScrollToTrackDialog();
        break;
    case togglePunchIn:
        session.setPunchInEnabled(!session.isPunchInEnabled());
        break;
    case clearClipIndicators:
        LevelMeter::clearAllClipIndicators();
        break;
    case createClickTrack:
        actions::createClickTrack(session, editor_);
        break;
    case toggleEditLock:
        actions::toggleSelectedClipsLock(session, editor_, actions::ClipLock::edit);
        break;
    case toggleTimeLock:
        actions::toggleSelectedClipsLock(session, editor_, actions::ClipLock::time);
        break;
    case clipRatingNone:
    case clipRating1:
    case clipRating2:
    case clipRating3:
    case clipRating4:
    case clipRating5:
        actions::setSelectedClipsRating(session, editor_, static_cast<int>(invocation.commandID - clipRatingNone));
        break;
    case toggleClipRatings:
        editor_.setShowingClipRatings(!editor_.isShowingClipRatings());
        break;
    case groupClips:
        actions::groupSelectedClips(session, editor_);
        break;
    case ungroupClips:
        actions::ungroupSelectedClips(session, editor_);
        break;
    case ungroupAllClips:
        actions::ungroupAllSelectedClips(session, editor_);
        break;
    case regroupClips:
        actions::regroupClips(session, editor_);
        break;
    case loopClips:
        showLoopDialog();
        break;
    case unloopClips:
        actions::unloopSelectedClips(session, editor_);
        break;
    case captureClip:
        showCaptureDialog();
        break;
    case renameClip:
        showRenameClipDialog();
        break;
    case identifySyncPoint:
        if (!actions::identifySyncPoint(session, editor_, arranger_.getPlayheadSeconds()))
            showMessage("Identify Sync Point", "Place the playhead inside a selected clip (that is not edit-locked).");
        break;
    case removeSyncPoint:
        actions::removeSyncPoints(session, editor_);
        break;
    case quantizeClipsToGrid:
        actions::quantizeSelectedClipsToGrid(session, editor_, editor_.getGridBeats() * 60.0 / session.getTempo());
        break;
    case bypassClipGain:
        actions::toggleSelectedClipsGainBypass(session, editor_);
        break;
    case renderClipGain:
    {
        juce::String error;
        if (actions::renderSelectedClipsGain(project_, editor_, error) == 0)
            showMessage("Render Clip Gain", error);
        break;
    }
    case showClipList:
        showBottomTab(BottomTab::clips);
        break;
    case toggleFullScreen:
        if (services_.toggleFullScreen)
            services_.toggleFullScreen();
        break;
    case togglePunchOut:
        session.setPunchOutEnabled(!session.isPunchOutEnabled());
        break;
    case toggleRecordMonitoring:
        session.setInputOnlyMonitoring(!session.isInputOnlyMonitoring());
        break;
    case audioSettings:
        showAudioSettings();
        break;
    case toggleTestTone:
        controller_.setTestToneEnabled(!controller_.isTestToneEnabled());
        break;
    case showShortcuts:
        if (services_.showPreferences)
            services_.showPreferences(Preferences::Tab::keyboard);
        else
            showShortcutHelp();
        break;
    case record:
        toggleRecording();
        break;
    case toggleLoop:
        session.beginTransaction("Toggle Loop");
        session.setLoopEnabled(!session.isLoopEnabled());
        break;
    case exportAudio:
        ExportDialog::show(project_, services_.exporter, this);
        break;
    case addInstrumentTrack:
        actions::addInstrumentTrack(session, editor_, "builtin:sampler", "Sampler");
        break;
    case addBusTrack:
        actions::addTrack(session, editor_, session::TrackKind::bus);
        break;
    case addFolder:
        actions::addFolder(session, editor_);
        break;
    case groupIntoFolder:
        actions::groupSelectedTracksIntoFolder(session, editor_);
        break;
    case showMixer:
        showBottomTab(BottomTab::mixer);
        break;
    case showDevices:
        showBottomTab(BottomTab::devices);
        break;
    case showEditor:
        showBottomTab(BottomTab::editor);
        break;
    case scanPlugins:
        browser_.scanPlugins(false);
        break;
    case openDemo:
        ensureSavedThen([this] { openDemoProject(); });
        break;
    case cutClips:
        actions::cutSelectedClips(session, editor_);
        break;
    case copyClips:
        actions::copySelectedClips(session, editor_);
        break;
    case pasteClips:
        actions::pasteClips(session, editor_, arranger_.getPlayheadSeconds());
        break;
    default:
        return false;
    }

    commandManager_.commandStatusChanged();
    return true;
}

//==================================================================================================
// Document actions

void MainComponent::ensureSavedThen(std::function<void()> proceed)
{
    if (!project_.hasUnsavedChanges())
    {
        proceed();
        return;
    }

    juce::Component::SafePointer<MainComponent> safe(this);
    const auto options =
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Unsaved changes")
            .withMessage("Do you want to save the changes to \"" + project_.getDisplayName() + "\"?")
            .withButton("Save")
            .withButton("Don't Save")
            .withButton("Cancel")
            .withAssociatedComponent(this);

    messageBox_ = juce::AlertWindow::showScopedAsync(options,
                                                     [safe, proceed](int result)
                                                     {
                                                         if (safe == nullptr)
                                                             return;
                                                         if (result == 1)
                                                             safe->save(
                                                                 [proceed](bool saved)
                                                                 {
                                                                     if (saved)
                                                                         proceed();
                                                                 });
                                                         else if (result == 2)
                                                             proceed();
                                                     });
}

void MainComponent::save(std::function<void(bool)> onDone)
{
    if (project_.getFile() == juce::File())
    {
        saveAs(std::move(onDone));
        return;
    }

    const auto result = project_.save();
    if (result.failed())
        showMessage("Could not save", result.getErrorMessage());

    if (onDone)
        onDone(result.wasOk());
}

void MainComponent::saveAs(std::function<void(bool)> onDone)
{
    const auto initial = project_.getFile() != juce::File()
                             ? project_.getFile()
                             : defaultSessionFolder().getChildFile(project_.getDisplayName() + ".dojo");

    chooser_ = std::make_unique<juce::FileChooser>("Save Session As", initial, "*.dojo");
    juce::Component::SafePointer<MainComponent> safe(this);

    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, onDone](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr)
                                  return;

                              const auto file = chooser.getResult();
                              bool saved = false;

                              if (file != juce::File())
                              {
                                  const auto result = safe->project_.saveAs(file);
                                  saved = result.wasOk();
                                  if (!saved)
                                      safe->showMessage("Could not save", result.getErrorMessage());
                              }

                              if (onDone)
                                  onDone(saved);
                          });
}

void MainComponent::showOpenChooser()
{
    const auto folder =
        project_.getFile() != juce::File() ? project_.getFile().getParentDirectory() : defaultSessionFolder();
    chooser_ = std::make_unique<juce::FileChooser>("Open Session", folder, "*.dojo");
    juce::Component::SafePointer<MainComponent> safe(this);

    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe != nullptr && chooser.getResult().existsAsFile())
                                  safe->openSessionFile(chooser.getResult());
                          });
}

void MainComponent::openSessionFile(const juce::File& file)
{
    controller_.stop();
    const auto result = project_.open(file);

    if (result.failed())
    {
        showMessage("Could not open session", result.getErrorMessage());
        return;
    }

    afterDocumentLoaded();
}

void MainComponent::startNewSession()
{
    controller_.stop();
    project_.newSession();
    afterDocumentLoaded();
}

void MainComponent::afterDocumentLoaded()
{
    editor_.clearClipSelection();
    editor_.selectTrack(project_.getSession().getNumTracks() > 0
                            ? session::track::getId(project_.getSession().getTrack(0))
                            : 0u);
    controller_.returnToStart();
    arranger_.zoomToFit();
    commandManager_.commandStatusChanged();
}

void MainComponent::showImportChooser()
{
    chooser_ = std::make_unique<juce::FileChooser>(
        "Import Audio", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        project_.getAssets().getWildcard());
    juce::Component::SafePointer<MainComponent> safe(this);

    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::canSelectMultipleItems,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr)
                                  return;

                              juce::StringArray paths;
                              for (const auto& file : chooser.getResults())
                                  if (file.existsAsFile())
                                      paths.add(file.getFullPathName());

                              if (paths.isEmpty())
                                  return;

                              auto& session = safe->project_.getSession();
                              const auto target = session.findTrack(safe->editor_.getSelectedTrack());
                              safe->project_.importFiles(paths, target, safe->arranger_.getPlayheadSeconds());
                          });
}

void MainComponent::showImportVideoChooser()
{
    chooser_ = std::make_unique<juce::FileChooser>("Import Video", juce::File::getSpecialLocation(juce::File::userMoviesDirectory),
                                                   video::getVideoFileWildcard());
    juce::Component::SafePointer<MainComponent> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe](const juce::FileChooser& chooser)
                          {
                              if (safe == nullptr || !chooser.getResult().existsAsFile())
                                  return;
                              auto& session = safe->project_.getSession();
                              juce::String error;
                              const auto target = session.findTrack(safe->editor_.getSelectedTrack());
                              if (actions::importVideo(session, safe->editor_, chooser.getResult(), safe->arranger_.getPlayheadSeconds(), target, error).isValid())
                                  safe->showVideoWindow(true);
                              else
                                  safe->showMessage("Import Video", error);
                          });
}

void MainComponent::showVideoWindow(bool show)
{
    if (show && videoWindow_ == nullptr)
    {
        videoWindow_ = std::make_unique<VideoWindow>(project_.getSession(), videoFrames_, [this] { return arranger_.getPlayheadSeconds(); });
        videoWindow_->onClosed = [this] { commandManager_.commandStatusChanged(); };
        // Top right of the main window.
        if (auto* top = getTopLevelComponent())
        {
            const auto area = top->getScreenBounds();
            videoWindow_->setBounds(area.getRight() - 680, area.getY() + 120, 660, 400);
        }
        else
        {
            videoWindow_->centreWithSize(660, 400);
        }
    }
    if (videoWindow_ != nullptr)
    {
        videoWindow_->setVisible(show);
        if (show)
            videoWindow_->toFront(false);
    }
    commandManager_.commandStatusChanged();
}

void MainComponent::openDemoProject()
{
    controller_.stop();
    project_.newSession();
    const auto folder = juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("The Dojo").getChildFile("Demo Samples");
    const auto result = createDemoProject(project_, folder);
    if (result.failed())
    {
        showMessage("Could not create the demo", result.getErrorMessage());
        return;
    }
    afterDocumentLoaded();
    for (const auto& t : project_.getSession().getAllTracks())
        if (session::track::getName(t) == "Drums")
            editor_.selectTrack(session::track::getId(t));
    showBottomTab(BottomTab::devices);
}

void MainComponent::importAudioFiles(const juce::StringArray& paths)
{
    project_.importFiles(paths, {}, arranger_.getPlayheadSeconds());
}

void MainComponent::requestQuit(std::function<void()> quit)
{
    ensureSavedThen(std::move(quit));
}

//==================================================================================================
// Bottom panel, recording, devices

MainComponent::ViewState MainComponent::getViewState() const
{
    return {arranger_.getViewState(), bottomTab_, bottomHeight_, editor_.getSelectedTrack(), editor_.getEditedClip()};
}

void MainComponent::setViewState(const ViewState& state)
{
    bottomHeight_ = state.bottomHeight;
    editor_.selectTrack(state.selectedTrack);
    if (state.editedClip != 0)
        editor_.setEditedClip(state.editedClip);
    showBottomTab(state.bottomTab);
    resized();
    arranger_.setViewState(state.arranger);
}

void MainComponent::showBottomTab(BottomTab tab)
{
    bottomTab_ = tab;
    mixer_.setVisible(tab == BottomTab::mixer);
    devicePanel_.setVisible(tab == BottomTab::devices);
    clipEditor_.setVisible(tab == BottomTab::editor);
    clipList_.setVisible(tab == BottomTab::clips);
    mixerTab_.setToggleState(tab == BottomTab::mixer, juce::dontSendNotification);
    devicesTab_.setToggleState(tab == BottomTab::devices, juce::dontSendNotification);
    editorTab_.setToggleState(tab == BottomTab::editor, juce::dontSendNotification);
    clipsTab_.setToggleState(tab == BottomTab::clips, juce::dontSendNotification);
    commandManager_.commandStatusChanged();
}

void MainComponent::toggleRecording()
{
    auto& recorder = services_.recorder;
    if (recorder.isRecording())
    {
        recorder.stop();
        return;
    }
    if (recorder.isBusy())
        return;

    const auto message = recorder.start();
    if (!recorder.isRecording())
        showMessage("Cannot record", message);
    else if (message.isNotEmpty())
        showMessage("Recording", message);
}

void MainComponent::addDevice(const deviceActions::DeviceChoice& choice, juce::ValueTree target)
{
    deviceActions::addDevice(project_.getSession(), editor_, std::move(target), choice);
    showBottomTab(BottomTab::devices);
}

//==================================================================================================
// Dialogs

void MainComponent::showAudioSettings()
{
    if (settingsWindow_ != nullptr)
    {
        settingsWindow_->toFront(true);
        return;
    }

    auto& deviceManager = controller_.getDeviceHost().getDeviceManager();
    // Inputs (for recording and monitoring) and MIDI inputs can be chosen here too.
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(deviceManager, 0, 32, 1, 2, true,
                                                                         false, true, false);
    selector->setSize(600, 560);

    juce::DialogWindow::LaunchOptions options;
    options.content.setOwned(selector.release());
    options.dialogTitle = "Audio Settings";
    options.dialogBackgroundColour = theme::colour::panel;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = false;
    options.resizable = false;
    options.componentToCentreAround = this;
    settingsWindow_ = options.launchAsync();
}

void MainComponent::showLoopDialog()
{
    if (editor_.getSelectedClips().empty())
        return;
    juce::Component::SafePointer<MainComponent> safe(this);
    TextPromptPanel::show(this, "Loop", "Number of loops (the clip plays this many times)", "4", "Loop", true,
                          [safe](const juce::String& text)
                          {
                              if (safe != nullptr)
                                  actions::loopSelectedClips(safe->project_.getSession(), safe->editor_, text.getDoubleValue());
                          });
}

void MainComponent::showCaptureDialog()
{
    auto& session = project_.getSession();
    juce::String error;
    const auto range = actions::getCaptureRange(session, editor_, error);
    if (!range.track.isValid())
    {
        showMessage("Capture", error);
        return;
    }
    juce::Component::SafePointer<MainComponent> safe(this);
    TextPromptPanel::show(this, "Capture", "Name of the new clip", actions::suggestCaptureName(session, range), "Capture", false,
                          [safe](const juce::String& name)
                          {
                              if (safe == nullptr)
                                  return;
                              juce::String problem;
                              if (actions::captureSelection(safe->project_.getSession(), safe->editor_, name, problem).isValid())
                                  safe->showBottomTab(BottomTab::clips);
                              else
                                  safe->showMessage("Capture", problem);
                          });
}

void MainComponent::showRenameClipDialog()
{
    auto& session = project_.getSession();
    juce::ValueTree clip;
    for (const auto id : editor_.getSelectedClips())
        if (clip = session.findClip(id); clip.isValid())
            break;
    if (!clip.isValid())
        return;
    const auto clipId = session::clip::getId(clip);
    juce::Component::SafePointer<MainComponent> safe(this);
    TextPromptPanel::show(this, "Rename Clip", "Name", session::clip::getName(clip), "Rename", false,
                          [safe, clipId](const juce::String& name)
                          {
                              if (safe != nullptr)
                                  actions::renameClip(safe->project_.getSession(), safe->project_.getSession().findClip(clipId), name);
                          });
}

void MainComponent::showMessage(const juce::String& title, const juce::String& message)
{
    messageBox_ = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions()
                                                         .withIconType(juce::MessageBoxIconType::WarningIcon)
                                                         .withTitle(title)
                                                         .withMessage(message)
                                                         .withButton("OK")
                                                         .withAssociatedComponent(this),
                                                     nullptr);
}

void MainComponent::showNewTracksDialog()
{
    juce::Component::SafePointer<MainComponent> safe(this);
    NewTracksPanel::show(this, [safe](const std::vector<actions::NewTracks>& rows, bool afterSelection)
                         {
                             if (safe != nullptr)
                                 actions::addNewTracks(safe->project_.getSession(), safe->editor_, rows, afterSelection);
                         });
}

void MainComponent::showDuplicateTracksDialog()
{
    const auto selected = actions::getSelectedTracks(project_.getSession(), editor_);
    if (selected.empty())
        return;
    juce::Component::SafePointer<MainComponent> safe(this);
    DuplicateTracksPanel::show(this, static_cast<int>(selected.size()), lastDuplicate_,
                               [safe](const actions::DuplicateSettings& settings)
                               {
                                   if (safe == nullptr)
                                       return;
                                   safe->lastDuplicate_ = settings;
                                   actions::duplicateSelectedTracks(safe->project_, safe->editor_, settings);
                               });
}

void MainComponent::showScrollToTrackDialog()
{
    auto& session = project_.getSession();
    juce::StringArray names;
    int current = 0;
    const auto tracks = session.getAllTracks();
    for (std::size_t i = 0; i < tracks.size(); ++i)
    {
        names.add(session::track::getName(tracks[i]));
        if (session::track::getId(tracks[i]) == editor_.getSelectedTrack())
            current = static_cast<int>(i) + 1;
    }
    if (names.isEmpty())
        return;
    juce::Component::SafePointer<MainComponent> safe(this);
    ScrollToTrackPanel::show(this, names, current, [safe](int number)
                             {
                                 if (safe != nullptr)
                                     safe->scrollToTrackNumber(number);
                             });
}

bool MainComponent::scrollToTrackNumber(int number)
{
    const auto t = actions::getTrackByNumber(project_.getSession(), number);
    if (!t.isValid())
        return false;
    editor_.selectTrack(session::track::getId(t));
    arranger_.scrollToTrack(t);
    mixer_.scrollToTrack(t);
    return true;
}

void MainComponent::showShortcutHelp()
{
    const juce::String text =
        "Space\tPlay / stop\n"
        "R\tRecord onto armed tracks\n"
        "L\tLoop on / off\n"
        "Ctrl+R\tExport audio\n"
        "F3 / F4 / F5\tMixer / Devices / Editor\n"
        "Ctrl+Shift+T / Ctrl+B / Ctrl+Shift+F\tAdd instrument track / bus / folder\n"
        "Piano roll\tclick to draw, drag to move or resize, Q quantise, arrows move\n"
        "Home, Enter\tReturn to start\n"
        "Ctrl+T\tAdd audio track\n"
        "Ctrl+I\tImport audio\n"
        "Ctrl+Z / Ctrl+Shift+Z\tUndo / redo\n"
        "Delete\tDelete selected clips\n"
        "Ctrl+D\tDuplicate selected clips\n"
        "Ctrl+E\tSplit at playhead\n"
        "Ctrl+A\tSelect all clips\n"
        "Ctrl+G\tToggle snap\n"
        "Ctrl+= / Ctrl+- / Ctrl+0\tZoom in / out / to fit\n"
        "Ctrl+S / Ctrl+O / Ctrl+N\tSave / open / new\n"
        "Ctrl+,\tPreferences (playback, themes, audio, shortcuts)\n"
        "Ctrl+Shift+A\tAudio settings\n"
        "F1\tKeyboard shortcuts (change them there)\n\n"
        "Mouse: drag clips to move (also across tracks); drag clip edges to trim; drag the\n"
        "small squares on a clip's top corners to fade; Alt or Shift while dragging bypasses\n"
        "snapping; Ctrl+wheel zooms; Shift+wheel scrolls sideways; double-click a track\n"
        "name to rename it; drag a track header to reorder; right-click for more.\n\n"
        "Selecting clips: drag in empty space to draw a selection box (Ctrl or Shift adds to\n"
        "the selection); Ctrl+click adds or removes one clip; Shift+click selects every clip on\n"
        "that track from the last clicked clip to this one (Ctrl+Shift+click adds that range).\n\n"
        "Selecting tracks: in the track headers, Shift+click selects every track from the last\n"
        "clicked one to this one, Ctrl+click adds or removes one (Ctrl+Shift+click adds a range).\n"
        "Ctrl+Shift+G groups the selected tracks into a new folder; drag one of their headers to\n"
        "move them all; M, S and R on one of them switch all of them.";

    messageBox_ = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions()
                                                         .withIconType(juce::MessageBoxIconType::InfoIcon)
                                                         .withTitle("Shortcuts")
                                                         .withMessage(text)
                                                         .withButton("OK")
                                                         .withAssociatedComponent(this),
                                                     nullptr);
}

} // namespace dojo::ui

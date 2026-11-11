#include "ui/MainComponent.h"

#include "core/MusicalTime.h"
#include "core/rt/RealtimeGuard.h"
#include "ui/Commands.h"
#include "ui/EditActions.h"
#include "engine/AudioEngine.h"
#include "ui/DemoProject.h"
#include "ui/components/ExportDialog.h"
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
    mixerTab_.setTooltip("Mixer (F3)");
    devicesTab_.setTooltip("Devices of the selected track (F4)");
    editorTab_.setTooltip("Piano roll / step sequencer / clip properties (F5)");
    for (auto* b : {&mixerTab_, &devicesTab_, &editorTab_})
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
    for (auto* b : {&mixerTab_, &devicesTab_, &editorTab_})
        b->setBounds(tabs.removeFromLeft(86));
    divider_->setBounds(area.removeFromBottom(kDividerHeight));
    for (auto* panel : std::initializer_list<juce::Component*>{&mixer_, &devicePanel_, &clipEditor_})
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
                                   pasteClips};
    c.addArray(ids, static_cast<int>(std::size(ids)));
}

void MainComponent::getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using namespace commands;
    constexpr auto cmd = juce::ModifierKeys::commandModifier;
    constexpr auto shift = juce::ModifierKeys::shiftModifier;
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
    editor_.selectTrack(project_.getSession().getNumTracks() > 1 ? session::track::getId(project_.getSession().getTrack(1)) : 0u);
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
    mixerTab_.setToggleState(tab == BottomTab::mixer, juce::dontSendNotification);
    devicesTab_.setToggleState(tab == BottomTab::devices, juce::dontSendNotification);
    editorTab_.setToggleState(tab == BottomTab::editor, juce::dontSendNotification);
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

#include "ui/MainComponent.h"

#include "core/MusicalTime.h"
#include "core/rt/RealtimeGuard.h"
#include "ui/Commands.h"
#include "ui/EditActions.h"
#include "ui/theme/DojoTheme.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include <iterator>

namespace dojo::ui
{
namespace
{
constexpr double kStatusRefreshIntervalSeconds = 0.25;
constexpr double kMaxFrameDeltaSeconds = 0.1; // clamp after stalls so meters don't jump

juce::File defaultSessionFolder()
{
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory);
}
} // namespace

MainComponent::MainComponent(AppServices& services)
    : services_(services)
    , project_(services.project)
    , controller_(services.controller)
    , commandManager_(services.commands)
    , transport_(services.controller, services.project.getSession(), editor_)
    , browser_(services.project.getAssets().getWildcard())
    , arranger_(services.project, services.controller, editor_)
    , mixer_(services.project, services.controller, editor_)
    , vblank_(this, [this](double timestampSeconds) { onDisplayRefresh(timestampSeconds); })
{
    setOpaque(true);

    addAndMakeVisible(transport_);
    addAndMakeVisible(browser_);
    addAndMakeVisible(arranger_);
    addAndMakeVisible(mixer_);
    addAndMakeVisible(statusBar_);

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
    project_.getSession().getUndoManager().removeChangeListener(this);
    project_.onImportProblem = nullptr;

    if (settingsWindow_ != nullptr)
        delete settingsWindow_.getComponent();
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

    const int mixerHeightClamped = juce::jlimit(180, juce::jmax(180, area.getHeight() / 2), mixerHeight);
    mixer_.setBounds(area.removeFromBottom(mixerHeightClamped));
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
    arranger_.setPlayhead(telemetry.playheadSamples, sampleRate, telemetry.playing);
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
                                   toggleTestTone,
                                   showShortcuts};
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
        info.setInfo("Delete Track", "Delete the selected track and its clips", categories::track, 0);
        info.setActive(session.findTrack(editor_.getSelectedTrack()).isValid());
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

    case audioSettings:
        info.setInfo("Audio Settings...", "Choose the audio driver and device", categories::options, 0);
        info.addDefaultKeypress(',', cmd);
        break;
    case toggleTestTone:
        info.setInfo("Test Tone", "Play a 440 Hz test tone through the master bus", categories::options, 0);
        info.setTicked(controller_.isTestToneEnabled());
        break;
    case showShortcuts:
        info.setInfo("Keyboard Shortcuts", "List keyboard and mouse shortcuts", categories::options, 0);
        info.addDefaultKeypress(juce::KeyPress::F1Key, 0);
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
        actions::deleteSelectedTrack(session, editor_);
        break;

    case playStop:
        controller_.togglePlayback();
        break;
    case returnToStart:
        controller_.returnToStart();
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

    case audioSettings:
        showAudioSettings();
        break;
    case toggleTestTone:
        controller_.setTestToneEnabled(!controller_.isTestToneEnabled());
        break;
    case showShortcuts:
        showShortcutHelp();
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

void MainComponent::importAudioFiles(const juce::StringArray& paths)
{
    project_.importFiles(paths, {}, arranger_.getPlayheadSeconds());
}

void MainComponent::requestQuit(std::function<void()> quit)
{
    ensureSavedThen(std::move(quit));
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
    auto selector = std::make_unique<juce::AudioDeviceSelectorComponent>(deviceManager, 0, 0, 1, 2, false,
                                                                         false, true, false);
    selector->setSize(560, 420);

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
        "Ctrl+,\tAudio settings\n\n"
        "Mouse: drag clips to move (also across tracks); drag clip edges to trim; drag the\n"
        "small squares on a clip's top corners to fade; Alt or Shift while dragging bypasses\n"
        "snapping; Ctrl+wheel zooms; Shift+wheel scrolls sideways; double-click a track\n"
        "name to rename it; drag a track header to reorder; right-click for more.";

    messageBox_ = juce::AlertWindow::showScopedAsync(juce::MessageBoxOptions()
                                                         .withIconType(juce::MessageBoxIconType::InfoIcon)
                                                         .withTitle("Shortcuts")
                                                         .withMessage(text)
                                                         .withButton("OK")
                                                         .withAssociatedComponent(this),
                                                     nullptr);
}

} // namespace dojo::ui

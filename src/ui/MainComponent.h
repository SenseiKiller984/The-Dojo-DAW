#pragma once

#include "engine/EngineController.h"
#include "session/Project.h"
#include "ui/AppServices.h"
#include "ui/components/ArrangerView.h"
#include "ui/components/BrowserPanel.h"
#include "ui/components/ClipEditor.h"
#include "ui/components/ClipListPanel.h"
#include "ui/components/DevicePanel.h"
#include "ui/components/MixerPanel.h"
#include "ui/EditActions.h"
#include "ui/components/StatusBar.h"
#include "ui/components/TransportBar.h"
#include "ui/components/VideoWindow.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace dojo::ui
{

/**
    Root content component of the main window.

        +-------------------------------------------------------------+
        | TransportBar                                                |
        +-----------+-------------------------------------------------+
        |           | ArrangerView (ruler / track headers / lanes)    |
        | Browser   |                                                 |
        |           +-------------------------------------------------+
        |           | MixerPanel (channel strips ............ master) |
        +-----------+-------------------------------------------------+
        | StatusBar                                                   |
        +-------------------------------------------------------------+

    Also the application's command target: every menu item and keyboard shortcut is an
    ApplicationCommand performed here, and document actions (new / open / save / import / quit with
    an unsaved-changes prompt) live here.

    UI refresh is driven by a juce::VBlankAttachment: once per display refresh the component polls
    the engine's lock-free telemetry and pushes it into the children. The audio thread never calls
    into the UI — the UI pulls.
*/
class MainComponent final : public juce::Component,
                            public juce::ApplicationCommandTarget,
                            public juce::DragAndDropContainer,
                            private juce::ChangeListener
{
public:
    explicit MainComponent(AppServices& services);
    ~MainComponent() override;

    /** Asks to save unsaved changes, then calls `quit` (unless the user cancels). */
    void requestQuit(std::function<void()> quit);

    /** Opens a session file (e.g. given on the command line). */
    void openSessionFile(const juce::File& file);

    /** Replaces the session with the demo project (after asking to save changes). */
    void openDemoProject();

    /** Imports audio files onto new tracks at the playhead (e.g. given on the command line). */
    void importAudioFiles(const juce::StringArray& paths);

    enum class BottomTab
    {
        mixer,
        devices,
        editor,
        clips,
    };

    /** Shows one of the bottom panel's tabs. */
    void showBottomTab(BottomTab tab);

    /** What the user was looking at: carried over when the window is rebuilt (new theme). */
    struct ViewState
    {
        ArrangerView::ViewState arranger;
        BottomTab bottomTab = BottomTab::mixer;
        int bottomHeight = 330;
        std::uint32_t selectedTrack = 0;
        std::uint32_t editedClip = 0;
    };
    [[nodiscard]] ViewState getViewState() const;
    /** Call once the component has its size. */
    void setViewState(const ViewState& state);

    /** Number of UI frames processed so far (used by the start-up smoke test). */
    [[nodiscard]] std::uint64_t getFrameCount() const noexcept { return frameCount_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    /** The mouse and editing tips (Keyboard tab of the Preferences). */
    void showShortcutHelp();
    [[nodiscard]] ArrangerView& getArranger() noexcept { return arranger_; }
    [[nodiscard]] MixerPanel& getMixer() noexcept { return mixer_; }
    [[nodiscard]] EditorState& getEditorState() noexcept { return editor_; }
    [[nodiscard]] ClipListPanel& getClipList() noexcept { return clipList_; }
    /** Track > Scroll to Track: selects track `number` (1 = first) and brings it to the top of the
        arranger and the left of the mixer. False if there is no such track. */
    bool scrollToTrackNumber(int number);

    /** Keyboard focus starts in the arranger (not in the first text field, e.g. a browser search). */
    void focusGained(FocusChangeType cause) override;

    // juce::ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& commands) override;
    void getCommandInfo(juce::CommandID commandID, juce::ApplicationCommandInfo& result) override;
    bool perform(const InvocationInfo& info) override;

private:
    void onDisplayRefresh(double timestampSeconds);
    void refreshStatusBar();
    void changeListenerCallback(juce::ChangeBroadcaster*) override;

    // Document actions
    void ensureSavedThen(std::function<void()> proceed);
    void save(std::function<void(bool saved)> onDone);
    void saveAs(std::function<void(bool saved)> onDone);
    void showOpenChooser();
    void showImportChooser();
    void showImportVideoChooser();
    /** Shows (or hides) the Video window. */
    void showVideoWindow(bool show);

public:
    [[nodiscard]] video::FrameServer& getVideoFrames() noexcept { return videoFrames_; }
    [[nodiscard]] VideoWindow* getVideoWindow() noexcept { return videoWindow_.get(); }

private:
    void startNewSession();
    void afterDocumentLoaded();

    // Recording, devices
    void toggleRecording();
    void addDevice(const deviceActions::DeviceChoice& choice, juce::ValueTree target);

    // Dialogs
    void showAudioSettings();
    /** Track menu dialogs. */
    void showNewTracksDialog();
    void showDuplicateTracksDialog();
    void showScrollToTrackDialog();
    /** Clip menu dialogs. */
    void showLoopDialog();
    void showCaptureDialog();
    void showRenameClipDialog();
    void showMessage(const juce::String& title, const juce::String& message);
    /** Playhead to the start; with the preference on, the view follows. */
    void goToStart();
    /** Play / stop. Playing again after a stop at the end of the arrangement starts from the
        beginning (the engine does that); the view follows when "return the view" is on. */
    void togglePlayback();

    AppServices& services_;
    session::Project& project_;
    engine::EngineController& controller_;
    juce::ApplicationCommandManager& commandManager_;
    actions::DuplicateSettings lastDuplicate_; ///< the Duplicate dialog starts from the last choices

    EditorState editor_; // before the components that observe it

    TransportBar transport_;
    BrowserPanel browser_;
    ArrangerView arranger_;
    MixerPanel mixer_;
    DevicePanel devicePanel_;
    ClipEditor clipEditor_;
    ClipListPanel clipList_;
    StatusBar statusBar_;

    class Divider;
    std::unique_ptr<Divider> divider_;
    juce::TextButton mixerTab_;
    juce::TextButton devicesTab_;
    juce::TextButton editorTab_;
    juce::TextButton clipsTab_;
    BottomTab bottomTab_ = BottomTab::mixer;
    int bottomHeight_ = 330;

    std::unique_ptr<juce::FileChooser> chooser_;
    video::FrameServer videoFrames_; ///< decodes video for the arranger's thumbnails and the Video window
    std::unique_ptr<VideoWindow> videoWindow_;
    juce::ScopedMessageBox messageBox_;
    juce::Component::SafePointer<juce::DialogWindow> settingsWindow_;

    double lastFrameTimestamp_ = 0.0;
    double lastStatusRefresh_ = 0.0;
    float loadPeakSinceStatusRefresh_ = 0.0f;
    std::uint64_t frameCount_ = 0;

    juce::VBlankAttachment vblank_; // last: must not fire before the children exist

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace dojo::ui

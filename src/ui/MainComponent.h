#pragma once

#include "engine/EngineController.h"
#include "session/Project.h"
#include "ui/AppServices.h"
#include "ui/components/ArrangerView.h"
#include "ui/components/BrowserPanel.h"
#include "ui/components/ClipEditor.h"
#include "ui/components/DevicePanel.h"
#include "ui/components/MixerPanel.h"
#include "ui/components/StatusBar.h"
#include "ui/components/TransportBar.h"
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
    };

    /** Shows one of the bottom panel's tabs. */
    void showBottomTab(BottomTab tab);

    /** Number of UI frames processed so far (used by the start-up smoke test). */
    [[nodiscard]] std::uint64_t getFrameCount() const noexcept { return frameCount_; }

    void paint(juce::Graphics& g) override;
    void resized() override;
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
    void startNewSession();
    void afterDocumentLoaded();

    // Recording, devices
    void toggleRecording();
    void addDevice(const deviceActions::DeviceChoice& choice, juce::ValueTree target);

    // Dialogs
    void showAudioSettings();
    void showMessage(const juce::String& title, const juce::String& message);
    void showShortcutHelp();

    AppServices& services_;
    session::Project& project_;
    engine::EngineController& controller_;
    juce::ApplicationCommandManager& commandManager_;

    EditorState editor_; // before the components that observe it

    TransportBar transport_;
    BrowserPanel browser_;
    ArrangerView arranger_;
    MixerPanel mixer_;
    DevicePanel devicePanel_;
    ClipEditor clipEditor_;
    StatusBar statusBar_;

    class Divider;
    std::unique_ptr<Divider> divider_;
    juce::TextButton mixerTab_;
    juce::TextButton devicesTab_;
    juce::TextButton editorTab_;
    BottomTab bottomTab_ = BottomTab::mixer;
    int bottomHeight_ = 330;

    std::unique_ptr<juce::FileChooser> chooser_;
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

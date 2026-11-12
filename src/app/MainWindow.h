#pragma once

#include "ui/AppServices.h"
#include "ui/MainComponent.h"
#include "ui/MainMenuModel.h"
#include "ui/PreferencesWindow.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::app
{

/**
    The single top-level window: menu bar, keyboard shortcuts and the main component. Uses JUCE-drawn
    (non-native) chrome so the window looks and behaves identically on Windows and Linux. The title
    shows the session name and an asterisk while there are unsaved changes.
*/
class MainWindow final : public juce::DocumentWindow, private juce::ChangeListener
{
public:
    explicit MainWindow(ui::AppServices& services);
    ~MainWindow() override;

    void closeButtonPressed() override;

    /** The square button (and a double-click on the title bar): maximise to the screen's work area,
        or back to the window it was. Kept here rather than in JUCE's full-screen state, which a
        window with its own (non-native) title bar loses track of after the window manager
        configures it - leaving the button unable to restore the window. */
    void maximiseButtonPressed() override;
    [[nodiscard]] bool isMaximised() const noexcept { return maximised_; }
    void setMaximised(bool shouldBeMaximised);

    /** View > Full Screen (F11): the whole screen, without the title bar; F11 again restores the
        window as it was (maximised or not). */
    void setFullScreenMode(bool shouldBeFullScreen);
    [[nodiscard]] bool isFullScreenMode() const noexcept { return fullScreenMode_; }
    /** The display the window is on: its work area (without task bars / docks), or all of it. */
    [[nodiscard]] juce::Rectangle<int> getScreenArea(bool wholeScreen) const;

    void resized() override;
    void moved() override;

    [[nodiscard]] ui::MainComponent& getMainComponent() noexcept { return *mainComponent_; }

    /** Opens the Preferences window on `tab`. */
    void showPreferences(ui::Preferences::Tab tab);
    [[nodiscard]] ui::PreferencesWindow* getPreferencesWindow() noexcept { return preferencesWindow_.get(); }

    /** Applies the theme chosen in the preferences: palette, look-and-feel, and a fresh main
        component (every control picks the new colours up). The document is untouched. */
    void applyTheme();

private:
    void updateTitle();
    void rebuildMainComponent();
    /** Sets the window's bounds itself (not the user dragging it). */
    void setOwnBounds(juce::Rectangle<int> bounds);
    /** The user moved or resized a maximised window: it is an ordinary window again. */
    void checkUserGeometry();
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    ui::AppServices& services_;
    session::Project& project_;
    ui::MainMenuModel menuModel_;
    ui::MainComponent* mainComponent_ = nullptr; // owned by the DocumentWindow (setContentOwned)
    std::unique_ptr<ui::PreferencesWindow> preferencesWindow_;

    bool maximised_ = false;
    bool fullScreenMode_ = false;
    bool maximisedBeforeFullScreen_ = false;
    bool settingBounds_ = false;
    juce::Rectangle<int> restoreBounds_; ///< the window before it was maximised / full screen
    juce::Rectangle<int> zoomedBounds_;  ///< what maximising / full screen set

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace dojo::app

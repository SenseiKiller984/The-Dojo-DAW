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
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;

    ui::AppServices& services_;
    session::Project& project_;
    ui::MainMenuModel menuModel_;
    ui::MainComponent* mainComponent_ = nullptr; // owned by the DocumentWindow (setContentOwned)
    std::unique_ptr<ui::PreferencesWindow> preferencesWindow_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace dojo::app

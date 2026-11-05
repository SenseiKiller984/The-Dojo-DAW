#pragma once

#include "ui/AppServices.h"
#include "ui/MainComponent.h"
#include "ui/MainMenuModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::app
{

/**
    The single top-level window: menu bar, keyboard shortcuts and the main component. Uses JUCE-drawn
    (non-native) chrome so the window looks and behaves identically on Windows and Linux. The title
    shows the session name and an asterisk while there are unsaved changes.
*/
class MainWindow final : public juce::DocumentWindow
{
public:
    explicit MainWindow(ui::AppServices& services);
    ~MainWindow() override;

    void closeButtonPressed() override;

    [[nodiscard]] ui::MainComponent& getMainComponent() noexcept { return *mainComponent_; }

private:
    void updateTitle();

    session::Project& project_;
    ui::MainMenuModel menuModel_;
    ui::MainComponent* mainComponent_ = nullptr; // owned by the DocumentWindow (setContentOwned)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace dojo::app

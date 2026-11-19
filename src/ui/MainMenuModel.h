#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui
{

/** The main window's menu bar. Every item is an application command, so shortcuts, enablement and
    tick marks come from MainComponent::getCommandInfo(). */
class MainMenuModel final : public juce::MenuBarModel
{
public:
    explicit MainMenuModel(juce::ApplicationCommandManager& commandManager);

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int, int) override {}

private:
    juce::ApplicationCommandManager& commandManager_;
};

} // namespace dojo::ui

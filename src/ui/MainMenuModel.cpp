#include "ui/MainMenuModel.h"

#include "ui/Commands.h"

namespace dojo::ui
{

MainMenuModel::MainMenuModel(juce::ApplicationCommandManager& commandManager)
    : commandManager_(commandManager)
{
    setApplicationCommandManagerToWatch(&commandManager_);
}

juce::StringArray MainMenuModel::getMenuBarNames()
{
    return {"File", "Edit", "Track", "Transport", "View", "Options"};
}

juce::PopupMenu MainMenuModel::getMenuForIndex(int topLevelMenuIndex, const juce::String&)
{
    using namespace commands;
    juce::PopupMenu menu;
    auto* cm = &commandManager_;

    switch (topLevelMenuIndex)
    {
    case 0:
        menu.addCommandItem(cm, newSession);
        menu.addCommandItem(cm, openSession);
        menu.addSeparator();
        menu.addCommandItem(cm, saveSession);
        menu.addCommandItem(cm, saveSessionAs);
        menu.addSeparator();
        menu.addCommandItem(cm, importAudio);
        menu.addSeparator();
        menu.addCommandItem(cm, juce::StandardApplicationCommandIDs::quit);
        break;
    case 1:
        menu.addCommandItem(cm, undo);
        menu.addCommandItem(cm, redo);
        menu.addSeparator();
        menu.addCommandItem(cm, deleteSelection);
        menu.addCommandItem(cm, duplicateSelection);
        menu.addCommandItem(cm, splitAtPlayhead);
        menu.addSeparator();
        menu.addCommandItem(cm, selectAllClips);
        break;
    case 2:
        menu.addCommandItem(cm, addTrack);
        menu.addCommandItem(cm, deleteTrack);
        break;
    case 3:
        menu.addCommandItem(cm, playStop);
        menu.addCommandItem(cm, returnToStart);
        break;
    case 4:
        menu.addCommandItem(cm, zoomIn);
        menu.addCommandItem(cm, zoomOut);
        menu.addCommandItem(cm, zoomToFit);
        menu.addSeparator();
        menu.addCommandItem(cm, toggleSnap);
        break;
    case 5:
        menu.addCommandItem(cm, audioSettings);
        menu.addCommandItem(cm, toggleTestTone);
        menu.addSeparator();
        menu.addCommandItem(cm, showShortcuts);
        break;
    default:
        break;
    }

    return menu;
}

} // namespace dojo::ui

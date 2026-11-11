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
        menu.addCommandItem(cm, openDemo);
        menu.addSeparator();
        menu.addCommandItem(cm, saveSession);
        menu.addCommandItem(cm, saveSessionAs);
        menu.addSeparator();
        menu.addCommandItem(cm, importAudio);
        menu.addCommandItem(cm, exportAudio);
        menu.addSeparator();
        menu.addCommandItem(cm, juce::StandardApplicationCommandIDs::quit);
        break;
    case 1:
        menu.addCommandItem(cm, undo);
        menu.addCommandItem(cm, redo);
        menu.addSeparator();
        menu.addCommandItem(cm, cutClips);
        menu.addCommandItem(cm, copyClips);
        menu.addCommandItem(cm, pasteClips);
        menu.addCommandItem(cm, deleteSelection);
        menu.addCommandItem(cm, duplicateSelection);
        menu.addCommandItem(cm, splitAtPlayhead);
        menu.addSeparator();
        menu.addCommandItem(cm, selectAllClips);
        break;
    case 2:
        menu.addCommandItem(cm, newTracks);
        menu.addCommandItem(cm, addTrack);
        menu.addCommandItem(cm, addInstrumentTrack);
        menu.addCommandItem(cm, addBusTrack);
        menu.addCommandItem(cm, addFolder);
        menu.addSeparator();
        menu.addCommandItem(cm, duplicateTracks);
        menu.addCommandItem(cm, deleteTrack);
        menu.addCommandItem(cm, splitIntoMono);
        menu.addCommandItem(cm, groupIntoFolder);
        menu.addCommandItem(cm, toggleTrackActive);
        menu.addSeparator();
        menu.addCommandItem(cm, toggleRecordMonitoring);
        menu.addSeparator();
        menu.addCommandItem(cm, scrollToTrack);
        break;
    case 3:
        menu.addCommandItem(cm, playStop);
        menu.addCommandItem(cm, record);
        menu.addCommandItem(cm, returnToStart);
        menu.addSeparator();
        menu.addCommandItem(cm, toggleLoop);
        menu.addCommandItem(cm, togglePunchIn);
        menu.addCommandItem(cm, togglePunchOut);
        break;
    case 4:
        menu.addCommandItem(cm, zoomIn);
        menu.addCommandItem(cm, zoomOut);
        menu.addCommandItem(cm, zoomToFit);
        menu.addSeparator();
        menu.addCommandItem(cm, toggleSnap);
        menu.addSeparator();
        menu.addCommandItem(cm, showMixer);
        menu.addCommandItem(cm, showDevices);
        menu.addCommandItem(cm, showEditor);
        break;
    case 5:
        menu.addCommandItem(cm, preferences);
        menu.addSeparator();
        menu.addCommandItem(cm, audioSettings);
        menu.addCommandItem(cm, showShortcuts);
        menu.addSeparator();
        menu.addCommandItem(cm, toggleTestTone);
        menu.addCommandItem(cm, scanPlugins);
        break;
    default:
        break;
    }

    return menu;
}

} // namespace dojo::ui

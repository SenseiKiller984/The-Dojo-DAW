#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui::commands
{

/** Application command IDs (menu items + keyboard shortcuts), handled by MainComponent. */
enum : juce::CommandID
{
    newSession = 0x2000,
    openSession,
    saveSession,
    saveSessionAs,
    importAudio,

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
    pasteClips,
    preferences,
    newTracks,
    duplicateTracks,
    splitIntoMono,
    toggleTrackActive,
    scrollToTrack,
    togglePunchIn,
    togglePunchOut,
    toggleRecordMonitoring,
};

namespace categories
{
inline constexpr const char* file = "File";
inline constexpr const char* edit = "Edit";
inline constexpr const char* track = "Track";
inline constexpr const char* transport = "Transport";
inline constexpr const char* view = "View";
inline constexpr const char* options = "Options";
} // namespace categories

} // namespace dojo::ui::commands

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
    clearClipIndicators,
    createClickTrack,
    toggleEditLock,
    toggleTimeLock,
    importVideo,
    showVideo,
    clipRatingNone, ///< followed by clipRating1 .. clipRating5 (clipRatingNone + n)
    clipRating1,
    clipRating2,
    clipRating3,
    clipRating4,
    clipRating5,
    toggleClipRatings,
    groupClips,
    ungroupClips,
    ungroupAllClips,
    regroupClips,
    loopClips,
    unloopClips,
    captureClip,
    renameClip,
    identifySyncPoint,
    removeSyncPoint,
    quantizeClipsToGrid,
    bypassClipGain,
    renderClipGain,
    showClipList,
};

namespace categories
{
inline constexpr const char* file = "File";
inline constexpr const char* edit = "Edit";
inline constexpr const char* clip = "Clip";
inline constexpr const char* track = "Track";
inline constexpr const char* transport = "Transport";
inline constexpr const char* view = "View";
inline constexpr const char* options = "Options";
} // namespace categories

} // namespace dojo::ui::commands

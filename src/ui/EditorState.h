#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <cstdint>
#include <set>
#include <vector>

namespace dojo::ui
{

/**
    UI-only editing state shared by the arranger, mixer and commands: what is selected and how the
    timeline grid snaps. Not part of the document (not saved, not undoable). Message thread only.
*/
class EditorState final
{
public:
    enum class Grid
    {
        bar,
        beat,
        halfBeat,
        quarterBeat,
    };

    struct Listener
    {
        virtual ~Listener() = default;
        virtual void editorStateChanged() = 0;
    };

    // --- Selection -------------------------------------------------------------------------------
    [[nodiscard]] const std::set<std::uint32_t>& getSelectedClips() const noexcept { return selectedClips_; }
    [[nodiscard]] bool isClipSelected(std::uint32_t clipId) const { return selectedClips_.count(clipId) > 0; }
    void selectClip(std::uint32_t clipId, bool addToSelection);
    void toggleClip(std::uint32_t clipId);
    void setSelectedClips(std::set<std::uint32_t> clipIds);
    void clearClipSelection();

    /** The current track (the last one clicked): its devices are shown, new tracks go below it. */
    [[nodiscard]] std::uint32_t getSelectedTrack() const noexcept { return selectedTrack_; }
    /** Every selected track (includes the current one). May hold ids of tracks deleted since. */
    [[nodiscard]] const std::set<std::uint32_t>& getSelectedTracks() const noexcept { return selectedTracks_; }
    [[nodiscard]] bool isTrackSelected(std::uint32_t trackId) const { return selectedTracks_.count(trackId) > 0; }
    /** Selects only this track (and deselects the master); it becomes the anchor for Shift ranges. */
    void selectTrack(std::uint32_t trackId);
    /**
        Explorer-like track selection; `order` lists the track ids top to bottom as shown.
        - neither: only `trackId` (as selectTrack)
        - toggle (Ctrl): adds or removes `trackId`, which becomes the anchor; the last selected
          track stays selected
        - range (Shift): the tracks from the anchor to `trackId` replace the selection; the anchor stays
        - toggle and range (Ctrl+Shift): that range is added to the selection
        The clicked track becomes the current one, unless Ctrl removed it.
    */
    void selectTrackWithModifiers(std::uint32_t trackId, bool toggle, bool range, const std::vector<std::uint32_t>& order);
    /** Selects exactly `trackIds`; `current` (one of them) becomes the current track and the anchor. */
    void setSelectedTracks(std::set<std::uint32_t> trackIds, std::uint32_t current);

    /** The master bus is selected (its devices are shown in the Devices panel). */
    [[nodiscard]] bool isMasterSelected() const noexcept { return masterSelected_; }
    void selectMaster();

    // --- Clipboard ---------------------------------------------------------------------------------
    /** Copied clips (deep copies) with the index of the track each came from. */
    struct ClipboardEntry
    {
        juce::ValueTree clip;
        int trackIndex = 0;
    };
    [[nodiscard]] const std::vector<ClipboardEntry>& getClipboard() const noexcept { return clipboard_; }
    void setClipboard(std::vector<ClipboardEntry> entries) { clipboard_ = std::move(entries); }

    // --- Editor ------------------------------------------------------------------------------------
    /** The clip open in the bottom editor (piano roll / step sequencer / clip inspector), 0 = none. */
    [[nodiscard]] std::uint32_t getEditedClip() const noexcept { return editedClip_; }
    void setEditedClip(std::uint32_t clipId);

    // --- Grid ------------------------------------------------------------------------------------
    [[nodiscard]] bool isSnapEnabled() const noexcept { return snapEnabled_; }
    void setSnapEnabled(bool enabled);
    [[nodiscard]] Grid getGrid() const noexcept { return grid_; }
    void setGrid(Grid grid);

    /** Grid spacing in beats (quarter notes, 4/4). */
    [[nodiscard]] double getGridBeats() const noexcept;

    /** Rounds `seconds` to the nearest grid line at `tempoBpm` (unless snapping is off or `bypass`). */
    [[nodiscard]] double snap(double seconds, double tempoBpm, bool bypass = false) const noexcept;

    void addListener(Listener* l) { listeners_.add(l); }
    void removeListener(Listener* l) { listeners_.remove(l); }

private:
    void notify();

    std::set<std::uint32_t> selectedClips_;
    std::uint32_t selectedTrack_ = 0;
    std::set<std::uint32_t> selectedTracks_;
    std::uint32_t trackAnchor_ = 0;
    bool masterSelected_ = false;
    std::uint32_t editedClip_ = 0;
    std::vector<ClipboardEntry> clipboard_;
    bool snapEnabled_ = true;
    Grid grid_ = Grid::beat;
    juce::ListenerList<Listener> listeners_;
};

} // namespace dojo::ui

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

    [[nodiscard]] std::uint32_t getSelectedTrack() const noexcept { return selectedTrack_; }
    /** Selects a track (and deselects the master). */
    void selectTrack(std::uint32_t trackId);

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
    bool masterSelected_ = false;
    std::uint32_t editedClip_ = 0;
    std::vector<ClipboardEntry> clipboard_;
    bool snapEnabled_ = true;
    Grid grid_ = Grid::beat;
    juce::ListenerList<Listener> listeners_;
};

} // namespace dojo::ui

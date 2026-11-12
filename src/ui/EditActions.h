#pragma once

#include "session/Session.h"
#include "ui/EditorState.h"

namespace dojo::session
{
class Project;
}

namespace dojo::ui::actions
{

/** Clip/track edits shared by menus, context menus and keyboard shortcuts. Each call is one undo step. */

/** Deletes the selected clips (except locked ones, which stay selected). Returns false if nothing was
    deleted. */
bool deleteSelectedClips(session::Session& session, EditorState& editor);

/** Duplicates the selected clips (each copy right after its original) and selects the copies. */
bool duplicateSelectedClips(session::Session& session, EditorState& editor);

/**
    Splits at `seconds`: the selected clips if any are selected; otherwise every clip under that time
    on the selected track; otherwise every clip under that time.
*/
bool splitAt(session::Session& session, EditorState& editor, double seconds);

/** Copies the selected clips to the editor's clipboard. Returns false if nothing was selected. */
bool copySelectedClips(session::Session& session, EditorState& editor);

/** Copies, then deletes the selected clips (one undo step). Locked clips are left out (and stay
    selected); returns false if every selected clip is locked. */
bool cutSelectedClips(session::Session& session, EditorState& editor);

/**
    Pastes the clipboard at `seconds`: the earliest clip lands there and the rest keep their relative
    positions; the first clip's track becomes the selected track (others keep their track offsets,
    falling back to the selected track where kinds don't match). Selects the pasted clips.
*/
bool pasteClips(session::Session& session, EditorState& editor, double seconds);

/** Selects every clip in the session. */
void selectAllClips(session::Session& session, EditorState& editor);

/** Clip locks of the selection (Clip > Edit Lock / Time Lock). */
enum class ClipLock
{
    edit,
    time,
};
/** True if every selected clip has the lock (so the command unlocks); false if none is selected. */
bool selectedClipsLocked(const session::Session& session, const EditorState& editor, ClipLock lock);
/** Locks the selected clips, or unlocks them if every one is already locked. One undo step. Returns
    false if no clip is selected. */
bool toggleSelectedClipsLock(session::Session& session, EditorState& editor, ClipLock lock);

/** Adds a track of `kind` below the selected one (or at the end) and selects it. */
juce::ValueTree addTrack(session::Session& session, EditorState& editor,
                         session::TrackKind kind = session::TrackKind::audio);

/** Adds an instrument track with `instrumentType` (e.g. "builtin:drumrack") loaded, and selects it. */
juce::ValueTree addInstrumentTrack(session::Session& session, EditorState& editor, const juce::String& instrumentType,
                                   const juce::String& instrumentName);

/** Adds an empty folder below the selected track and selects it. */
juce::ValueTree addFolder(session::Session& session, EditorState& editor);

/** The selected tracks that still exist, in arranger order, without those inside another selected
    track (a selected folder brings its contents along). */
std::vector<juce::ValueTree> getSelectedTracks(const session::Session& session, const EditorState& editor);

/** Puts the selected tracks into a new folder where the topmost of them is (tracks from other
    folders move in too), and selects the folder. */
juce::ValueTree groupSelectedTracksIntoFolder(session::Session& session, EditorState& editor);

/** Deletes the selected tracks (with their clips). Returns false if no track is selected. */
bool deleteSelectedTracks(session::Session& session, EditorState& editor);

// --- Track menu ------------------------------------------------------------------------------------

/** What Track > New can create. */
enum class NewTrackType
{
    audio,
    auxInput, ///< a bus: a return for sends, or a group other tracks are routed to
    instrument,
    midi,        ///< MIDI clips sent to an instrument track
    vcaMaster,   ///< controls the fader, mute and solo of its member tracks
    masterFader, ///< a last fader (and post-fader devices) on the main output or a bus
    clickTrack,  ///< a mono aux input with the Click device: a metronome that follows the transport
    folder,
    video,       ///< video clips, shown in the video window (View > Video)
};
[[nodiscard]] juce::String getName(NewTrackType type);
/** Whether the type has a format choice (mono, stereo or a multichannel format): audio tracks, aux
    inputs and folders. Instrument and MIDI tracks are stereo. */
[[nodiscard]] bool hasFormatChoice(NewTrackType type);

/** One row of the New Tracks dialog. */
struct NewTracks
{
    int count = 1;
    NewTrackType type = NewTrackType::audio;
    session::TrackFormat format = session::TrackFormat::stereo;
    juce::String name; ///< empty: default names; one track: this name; several: name 1, name 2, ...
};
/**
    Track > New: adds every row's tracks, in order, after the current track (inside its folder) or,
    without one (or with afterSelection false), at the end. The new tracks are selected. One undo step.
*/
std::vector<juce::ValueTree> addNewTracks(session::Session& session, EditorState& editor, const std::vector<NewTracks>& rows,
                                          bool afterSelection = true);

/** Track > Duplicate. */
struct DuplicateSettings
{
    int copies = 1;
    session::Session::DuplicateOptions options;
    /** All copies after the last selected track (in sets: A.dup1 B.dup1 A.dup2 B.dup2 ...), instead of
        each copy right after its source. */
    bool afterLastSelected = false;
};
/** Duplicates the selected tracks (with the devices' current state) and selects the copies. One undo step. */
std::vector<juce::ValueTree> duplicateSelectedTracks(session::Project& project, EditorState& editor, const DuplicateSettings& settings);

/** Stereo audio tracks among the selection (what Split into Mono works on). */
[[nodiscard]] std::vector<juce::ValueTree> getSplittableTracks(const session::Session& session, const EditorState& editor);
/**
    Track > Split into Mono: each selected stereo audio track gets a mono track per channel right after
    it ("Name.L", "Name.R"), panned hard left and right, each playing that channel of every clip and
    recording that input channel, with the source's devices, sends and automation. The source track is
    kept. The new tracks are selected. One undo step.
*/
std::vector<juce::ValueTree> splitSelectedIntoMono(session::Project& project, EditorState& editor);

/** True if any selected track is active (so the command reads "Make Inactive"). */
[[nodiscard]] bool anySelectedTrackActive(const session::Session& session, const EditorState& editor);
/** Track > Make Inactive / Make Active for every selected track. One undo step. */
bool setSelectedTracksActive(session::Session& session, EditorState& editor, bool active);

/** Track > Create Click Track: a mono aux input named "Click" with the Click device inserted, after
    the current track (selected). One undo step. */
juce::ValueTree createClickTrack(session::Session& session, EditorState& editor);

// --- Clip menu: ratings, groups, loops, sync points, clip gain, rename ------------------------------

/** The selected clips, with every clip of their outermost groups (a group is selected as a whole). */
[[nodiscard]] std::vector<juce::ValueTree> getSelectedClipsWithGroups(const session::Session& session, const EditorState& editor);

/** Clip > Rating: 0 (none) .. 5 on every selected clip. One undo step. False if none selected. */
bool setSelectedClipsRating(session::Session& session, EditorState& editor, int rating);
/** The selected clips' rating, or -1 if they differ (or none is selected). */
[[nodiscard]] int getSelectedClipsRating(const session::Session& session, const EditorState& editor);

/** Clip > Group: one new clip group of the selection (clips and whole groups, on any tracks). */
std::uint32_t groupSelectedClips(session::Session& session, EditorState& editor);
/** Clip > Ungroup: removes the top layer of the selected groups (nested groups stay). Remembered
    for Regroup. */
bool ungroupSelectedClips(session::Session& session, EditorState& editor);
/** Clip > Regroup: puts back what the last Ungroup took apart. */
bool regroupClips(session::Session& session, EditorState& editor);
/** Clip > Ungroup All: every layer of the selected groups. */
bool ungroupAllSelectedClips(session::Session& session, EditorState& editor);
[[nodiscard]] bool selectionHasGroups(const session::Session& session, const EditorState& editor);

/** Clip > Loop: the selected clips repeat `iterations` times (of their length, or loop). */
bool loopSelectedClips(session::Session& session, EditorState& editor, double iterations);
/** Clip > Unloop: looped clips become one standard clip per iteration (selected). */
bool unloopSelectedClips(session::Session& session, EditorState& editor);
[[nodiscard]] bool selectionHasLoopedClips(const session::Session& session, const EditorState& editor);

/** Clip > Identify Sync Point at `seconds` on the selected clips that hold it. */
bool identifySyncPoint(session::Session& session, EditorState& editor, double seconds);
/** Clip > Remove Sync Point from the selected clips. */
bool removeSyncPoints(session::Session& session, EditorState& editor);
[[nodiscard]] bool selectionHasSyncPoints(const session::Session& session, const EditorState& editor);

/** Clip > Quantize to Grid: each selected clip moves so that its sync point (or start) sits on the
    nearest line of a `gridSeconds` grid. Time- and edit-locked clips stay. */
bool quantizeSelectedClipsToGrid(session::Session& session, EditorState& editor, double gridSeconds);

/** Clip > Clip Gain > Bypass Clip Gain: toggles the selected audio clips (all on if any is off). */
bool toggleSelectedClipsGainBypass(session::Session& session, EditorState& editor);
[[nodiscard]] bool selectedClipsGainBypassed(const session::Session& session, const EditorState& editor);
/** Clip > Clip Gain > Render Clip Gain: writes each selected audio clip's material with its gain
    applied to a new file (next to the session), plays that instead and resets the gain to 0 dB.
    Returns how many clips were rendered; `error` says why none could be. */
int renderSelectedClipsGain(session::Project& project, EditorState& editor, juce::String& error);

/** Clip > Rename: the clip's new name; it becomes a user-defined clip (listed in the Clip List). */
bool renameClip(session::Session& session, juce::ValueTree clip, const juce::String& name);

/** What Clip > Capture would capture: the time selection (on the one track in it that has clips), or
    else the range of the selected clips (all on one track). `track` invalid and `error` set if none. */
struct CaptureRange
{
    juce::ValueTree track;
    double start = 0.0;
    double end = 0.0;
};
[[nodiscard]] CaptureRange getCaptureRange(const session::Session& session, const EditorState& editor, juce::String& error);
/** A name for a capture of `range` that no Clip List entry has yet ("<clip> Capture", "... 2"). */
[[nodiscard]] juce::String suggestCaptureName(const session::Session& session, const CaptureRange& range);
/** Clip > Capture: the range becomes a Clip List entry called `name`. One undo step. */
juce::ValueTree captureSelection(session::Session& session, EditorState& editor, const juce::String& name, juce::String& error);

/** One row of the Clip List. */
struct ClipListItem
{
    juce::ValueTree clip; ///< a captured entry, or a clip on a track
    bool captured = false;
    bool userDefined = false; ///< captured or renamed; others were created by editing (auto-created)
};
/** The Clip List: captured clips, then the clips on tracks that were renamed, then (with
    `includeAutoCreated`) every other clip; each name, file and range once. */
[[nodiscard]] std::vector<ClipListItem> getClipListItems(const session::Session& session, bool includeAutoCreated);
/** Drops a Clip List item on `track` at `seconds` (on a new track of the right kind if `track` cannot
    hold it). The new clip is selected. One undo step. */
juce::ValueTree placeClipListItem(session::Session& session, EditorState& editor, const juce::ValueTree& source,
                                  juce::ValueTree track, double seconds);

/**
    File > Import Video (and video files dropped on the arranger): a video clip of the whole file at
    `seconds`, on `target` if it is a video track, else on the first video track, else on a new one
    at the top. Selects the clip. Invalid (and `error` set) if the file cannot be decoded.
*/
juce::ValueTree importVideo(session::Session& session, EditorState& editor, const juce::File& file, double seconds,
                            const juce::ValueTree& target, juce::String& error);

/** What the video window shows at `seconds`: the clip of the topmost active, unmuted video track
    there, and the time within its file. */
struct VideoAtTime
{
    juce::ValueTree clip;
    juce::File file;
    double fileSeconds = 0.0;
};
[[nodiscard]] VideoAtTime findVideoAt(const session::Session& session, double seconds);

/** Track numbers (Scroll to Track): 1 = the first track in the arranger, counting tracks inside folders. */
[[nodiscard]] juce::ValueTree getTrackByNumber(const session::Session& session, int number);

} // namespace dojo::ui::actions

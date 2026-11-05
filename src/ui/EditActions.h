#pragma once

#include "session/Session.h"
#include "ui/EditorState.h"

namespace dojo::ui::actions
{

/** Clip/track edits shared by menus, context menus and keyboard shortcuts. Each call is one undo step. */

/** Deletes the selected clips. Returns false if nothing was selected. */
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

/** Copies, then deletes the selected clips (one undo step). */
bool cutSelectedClips(session::Session& session, EditorState& editor);

/**
    Pastes the clipboard at `seconds`: the earliest clip lands there and the rest keep their relative
    positions; the first clip's track becomes the selected track (others keep their track offsets,
    falling back to the selected track where kinds don't match). Selects the pasted clips.
*/
bool pasteClips(session::Session& session, EditorState& editor, double seconds);

/** Selects every clip in the session. */
void selectAllClips(session::Session& session, EditorState& editor);

/** Adds a track of `kind` below the selected one (or at the end) and selects it. */
juce::ValueTree addTrack(session::Session& session, EditorState& editor,
                         session::TrackKind kind = session::TrackKind::audio);

/** Adds an instrument track with `instrumentType` (e.g. "builtin:drumrack") loaded, and selects it. */
juce::ValueTree addInstrumentTrack(session::Session& session, EditorState& editor, const juce::String& instrumentType,
                                   const juce::String& instrumentName);

/** Adds an empty folder below the selected track and selects it. */
juce::ValueTree addFolder(session::Session& session, EditorState& editor);

/** Puts the selected track into a new folder. */
juce::ValueTree groupSelectedTrackIntoFolder(session::Session& session, EditorState& editor);

/** Deletes the selected track (with its clips). Returns false if no track is selected. */
bool deleteSelectedTrack(session::Session& session, EditorState& editor);

} // namespace dojo::ui::actions

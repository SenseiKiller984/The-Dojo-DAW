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

/** Selects every clip in the session. */
void selectAllClips(session::Session& session, EditorState& editor);

/** Adds a track below the selected one (or at the end) and selects it. */
juce::ValueTree addTrack(session::Session& session, EditorState& editor);

/** Deletes the selected track (with its clips). Returns false if no track is selected. */
bool deleteSelectedTrack(session::Session& session, EditorState& editor);

} // namespace dojo::ui::actions

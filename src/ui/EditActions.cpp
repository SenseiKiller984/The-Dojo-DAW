#include "ui/EditActions.h"

#include <vector>

namespace dojo::ui::actions
{
namespace
{
std::vector<juce::ValueTree> selectedClips(session::Session& session, const EditorState& editor)
{
    std::vector<juce::ValueTree> clips;
    for (const auto id : editor.getSelectedClips())
        if (auto c = session.findClip(id); c.isValid())
            clips.push_back(c);
    return clips;
}
} // namespace

bool deleteSelectedClips(session::Session& session, EditorState& editor)
{
    const auto clips = selectedClips(session, editor);
    if (clips.empty())
        return false;

    session.beginTransaction(clips.size() == 1 ? "Delete Clip" : "Delete Clips");
    for (const auto& c : clips)
        session.removeClip(c);

    editor.clearClipSelection();
    return true;
}

bool duplicateSelectedClips(session::Session& session, EditorState& editor)
{
    const auto clips = selectedClips(session, editor);
    if (clips.empty())
        return false;

    session.beginTransaction(clips.size() == 1 ? "Duplicate Clip" : "Duplicate Clips");
    std::set<std::uint32_t> copies;
    for (const auto& c : clips)
        if (auto copy = session.duplicateClip(c); copy.isValid())
            copies.insert(session::clip::getId(copy));

    editor.setSelectedClips(std::move(copies));
    return true;
}

bool splitAt(session::Session& session, EditorState& editor, double seconds)
{
    const auto covers = [seconds](const juce::ValueTree& c)
    {
        return seconds > session::clip::getStart(c) && seconds < session::clip::getEnd(c);
    };

    std::vector<juce::ValueTree> targets;
    for (const auto& c : selectedClips(session, editor))
        if (covers(c))
            targets.push_back(c);

    if (targets.empty() && editor.getSelectedClips().empty())
    {
        const auto selectedTrack = session.findTrack(editor.getSelectedTrack());

        for (int i = 0; i < session.getNumTracks(); ++i)
        {
            const auto t = session.getTrack(i);
            if (selectedTrack.isValid() && t != selectedTrack)
                continue;

            for (const auto& c : t)
                if (session::Session::isClip(c) && covers(c))
                    targets.push_back(c);
        }
    }

    if (targets.empty())
        return false;

    session.beginTransaction("Split");
    std::set<std::uint32_t> rightParts;
    for (auto& c : targets)
        if (auto right = session.splitClip(c, seconds); right.isValid())
            rightParts.insert(session::clip::getId(right));

    editor.setSelectedClips(std::move(rightParts));
    return true;
}

bool copySelectedClips(session::Session& session, EditorState& editor)
{
    const auto clips = selectedClips(session, editor);
    if (clips.empty())
        return false;

    std::vector<EditorState::ClipboardEntry> entries;
    for (const auto& c : clips)
        entries.push_back({c.createCopy(), session.indexOf(c.getParent())});
    editor.setClipboard(std::move(entries));
    return true;
}

bool cutSelectedClips(session::Session& session, EditorState& editor)
{
    if (!copySelectedClips(session, editor))
        return false;
    session.beginTransaction("Cut");
    for (const auto& c : selectedClips(session, editor))
        session.removeClip(c);
    editor.clearClipSelection();
    return true;
}

bool pasteClips(session::Session& session, EditorState& editor, double seconds)
{
    const auto& entries = editor.getClipboard();
    if (entries.empty())
        return false;

    double earliest = 1.0e12;
    int firstTrack = entries.front().trackIndex;
    for (const auto& e : entries)
        if (session::clip::getStart(e.clip) < earliest)
        {
            earliest = session::clip::getStart(e.clip);
            firstTrack = e.trackIndex;
        }

    const auto selected = session.findTrack(editor.getSelectedTrack());
    const int targetFirst = selected.isValid() ? session.indexOf(selected) : firstTrack;
    const auto tracks = session.getAllTracks();

    session.beginTransaction("Paste");
    std::set<std::uint32_t> pasted;
    for (const auto& e : entries)
    {
        const bool midi = session::clip::isMidi(e.clip);
        const auto wanted = midi ? session::TrackKind::instrument : session::TrackKind::audio;
        const int index = targetFirst + (e.trackIndex - firstTrack);
        juce::ValueTree target = index >= 0 && index < static_cast<int>(tracks.size()) ? tracks[static_cast<std::size_t>(index)] : juce::ValueTree();
        if (!target.isValid() || session::track::getKind(target) != wanted)
            target = selected.isValid() && session::track::getKind(selected) == wanted ? selected : juce::ValueTree();
        if (!target.isValid())
            target = session.addTrack(wanted, {}, -1);

        auto copy = session.pasteClip(e.clip, target, seconds + (session::clip::getStart(e.clip) - earliest));
        if (copy.isValid())
            pasted.insert(session::clip::getId(copy));
    }

    editor.setSelectedClips(std::move(pasted));
    return true;
}

void selectAllClips(session::Session& session, EditorState& editor)
{
    std::set<std::uint32_t> all;
    for (const auto& t : session.getAllTracks())
        for (const auto& c : t)
            if (session::Session::isClip(c))
                all.insert(session::clip::getId(c));

    editor.setSelectedClips(std::move(all));
}

juce::ValueTree addTrack(session::Session& session, EditorState& editor, session::TrackKind kind)
{
    const auto selected = session.findTrack(editor.getSelectedTrack());
    int index = -1;
    if (selected.isValid())
    {
        // Below the selected track and everything inside it (if it is a folder).
        const auto tracks = session.getAllTracks();
        index = session.indexOf(selected) + 1;
        while (index < static_cast<int>(tracks.size()) && session::Session::isWithin(tracks[static_cast<std::size_t>(index)], selected))
            ++index;
        if (index >= static_cast<int>(tracks.size()))
            index = -1;
    }

    const char* name = kind == session::TrackKind::instrument ? "Add Instrument Track"
                       : kind == session::TrackKind::bus      ? "Add Bus"
                                                              : "Add Audio Track";
    session.beginTransaction(name);
    auto t = session.addTrack(kind, {}, index);

    // A new instrument track listens to the MIDI keyboard straight away (armed) unless another
    // instrument track is already armed.
    if (kind == session::TrackKind::instrument)
    {
        bool anyArmed = false;
        for (const auto& other : session.getAllTracks())
            anyArmed = anyArmed || (other != t && session::track::getKind(other) == session::TrackKind::instrument && session::track::isArmed(other));
        if (!anyArmed)
            session::track::setArmed(t, true, &session.getUndoManager());
    }

    // Keep the new track in the selected track's folder.
    if (selected.isValid())
        if (const auto folder = session::Session::getParentFolder(selected); folder.isValid() && t.getParent() != folder)
            session.moveIntoFolder(t, folder);

    editor.selectTrack(session::track::getId(t));
    return t;
}

juce::ValueTree addInstrumentTrack(session::Session& session, EditorState& editor, const juce::String& instrumentType,
                                   const juce::String& instrumentName)
{
    auto t = addTrack(session, editor, session::TrackKind::instrument);
    session::track::setName(t, instrumentName, &session.getUndoManager());
    session.addDevice(t, instrumentType, instrumentName);
    return t;
}

juce::ValueTree addFolder(session::Session& session, EditorState& editor)
{
    const auto selected = session.findTrack(editor.getSelectedTrack());
    const int index = selected.isValid() ? session.indexOf(selected) + 1 : -1;
    session.beginTransaction("Add Folder");
    auto folder = session.addFolder({}, index < session.getNumTracks() ? index : -1);
    editor.selectTrack(session::track::getId(folder));
    return folder;
}

std::vector<juce::ValueTree> getSelectedTracks(const session::Session& session, const EditorState& editor)
{
    std::vector<juce::ValueTree> selected;
    for (const auto id : editor.getSelectedTracks())
        if (const auto t = session.findTrack(id); t.isValid())
            selected.push_back(t);
    if (selected.empty()) // e.g. a selection set before multi-selection existed
        if (const auto t = session.findTrack(editor.getSelectedTrack()); t.isValid())
            selected.push_back(t);
    return session.outermostInOrder(selected);
}

juce::ValueTree groupSelectedTracksIntoFolder(session::Session& session, EditorState& editor)
{
    const auto selected = getSelectedTracks(session, editor);
    if (selected.empty())
        return {};
    session.beginTransaction(selected.size() > 1 ? "Group Tracks into Folder" : "Group into Folder");
    auto folder = session.groupIntoFolder(selected);
    if (folder.isValid())
        editor.selectTrack(session::track::getId(folder));
    return folder;
}

bool deleteSelectedTracks(session::Session& session, EditorState& editor)
{
    const auto selected = getSelectedTracks(session, editor);
    if (selected.empty())
        return false;

    const int index = session.indexOf(selected.front());
    session.beginTransaction(selected.size() > 1 ? "Delete Tracks" : "Delete Track");
    for (const auto& t : selected)
        session.removeTrack(t);
    editor.clearClipSelection();

    // Keep a sensible selection: the track that moved into the first deleted one's place, or the new last.
    const int count = session.getNumTracks();
    editor.selectTrack(count > 0 ? session::track::getId(session.getTrack(juce::jmin(index, count - 1)))
                                 : 0u);
    return true;
}

} // namespace dojo::ui::actions

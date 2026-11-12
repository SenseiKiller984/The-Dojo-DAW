#include "ui/EditActions.h"

#include "session/Project.h"

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

    // Locked clips stay (and stay selected).
    std::set<std::uint32_t> kept;
    std::vector<juce::ValueTree> removable;
    for (const auto& c : clips)
    {
        if (session::clip::canRemove(c))
            removable.push_back(c);
        else
            kept.insert(session::clip::getId(c));
    }
    if (removable.empty())
        return false;

    session.beginTransaction(removable.size() == 1 ? "Delete Clip" : "Delete Clips");
    for (const auto& c : removable)
        session.removeClip(c);

    editor.setSelectedClips(std::move(kept));
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
        // (edit-locked clips are not split; time-locked ones are)
        return seconds > session::clip::getStart(c) && seconds < session::clip::getEnd(c) && session::clip::canEdit(c);
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
    // Only the clips that may be removed are cut; locked ones stay where they are (and selected).
    std::vector<EditorState::ClipboardEntry> entries;
    std::vector<juce::ValueTree> removable;
    std::set<std::uint32_t> kept;
    for (const auto& c : selectedClips(session, editor))
    {
        if (session::clip::canRemove(c))
        {
            entries.push_back({c.createCopy(), session.indexOf(c.getParent())});
            removable.push_back(c);
        }
        else
        {
            kept.insert(session::clip::getId(c));
        }
    }
    if (removable.empty())
        return false;

    editor.setClipboard(std::move(entries));
    session.beginTransaction("Cut");
    for (const auto& c : removable)
        session.removeClip(c);
    editor.setSelectedClips(std::move(kept));
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
    // A deleted VCA Master's fader and automation are coalesced into its members (they sound the same).
    const auto isDeleted = [&selected](const juce::ValueTree& t)
    { return std::any_of(selected.begin(), selected.end(), [&t](const juce::ValueTree& s) { return session::Session::isWithin(t, s); }); };
    for (const auto& t : session.getAllTracks())
        if (session::track::getKind(t) == session::TrackKind::vca && isDeleted(t))
            for (auto member : session.getVcaMembers(t))
                if (!isDeleted(member))
                    session.coalesceVca(member, t);
    for (const auto& t : selected)
        session.removeTrack(t);
    editor.clearClipSelection();

    // Keep a sensible selection: the track that moved into the first deleted one's place, or the new last.
    const int count = session.getNumTracks();
    editor.selectTrack(count > 0 ? session::track::getId(session.getTrack(juce::jmin(index, count - 1)))
                                 : 0u);
    return true;
}

// --- Track menu ------------------------------------------------------------------------------------

juce::String getName(NewTrackType type)
{
    switch (type)
    {
    case NewTrackType::auxInput:    return "Aux Input";
    case NewTrackType::instrument:  return "Instrument Track";
    case NewTrackType::midi:        return "MIDI Track";
    case NewTrackType::vcaMaster:   return "VCA Master";
    case NewTrackType::masterFader: return "Master Fader";
    case NewTrackType::clickTrack:  return "Click Track";
    case NewTrackType::folder:      return "Folder";
    case NewTrackType::audio:       break;
    }
    return "Audio Track";
}

bool hasFormatChoice(NewTrackType type)
{
    return type == NewTrackType::audio || type == NewTrackType::auxInput;
}

std::vector<juce::ValueTree> addNewTracks(session::Session& session, EditorState& editor, const std::vector<NewTracks>& rows,
                                          bool afterSelection)
{
    std::vector<juce::ValueTree> created;
    int total = 0;
    for (const auto& row : rows)
        total += std::max(0, row.count);
    if (total <= 0)
        return created;

    juce::ValueTree after = afterSelection ? session.findTrack(editor.getSelectedTrack()) : juce::ValueTree();
    session.beginTransaction(total > 1 ? "New Tracks" : "New Track");

    for (const auto& row : rows)
    {
        const auto base = row.name.trim();
        for (int n = 0; n < row.count; ++n)
        {
            const juce::String name = base.isEmpty() ? juce::String()
                                      : row.count == 1 ? session.uniqueTrackName(base)
                                                       : session.uniqueTrackName(base, " ", true);
            juce::ValueTree t;
            switch (row.type)
            {
            case NewTrackType::audio:      t = session.addTrack(session::TrackKind::audio, name); break;
            case NewTrackType::auxInput:   t = session.addTrack(session::TrackKind::bus, name); break;
            case NewTrackType::instrument: t = session.addTrack(session::TrackKind::instrument, name); break;
            case NewTrackType::midi:       t = session.addTrack(session::TrackKind::midi, name); break;
            case NewTrackType::vcaMaster:  t = session.addTrack(session::TrackKind::vca, name); break;
            case NewTrackType::masterFader:
            {
                // The main output's, unless it has one: then the first bus without one.
                std::set<std::uint32_t> taken;
                for (const auto& other : session.getAllTracks())
                    if (session::track::getKind(other) == session::TrackKind::masterFader)
                        taken.insert(session::track::getControlledBus(other));
                std::uint32_t path = 0;
                if (taken.count(0) > 0)
                    for (const auto& bus : session.getBusses())
                        if (!session::track::isFolder(bus) && taken.count(session::track::getId(bus)) == 0)
                        {
                            path = session::track::getId(bus);
                            break;
                        }
                t = session.addTrack(session::TrackKind::masterFader, name);
                session::track::setControlledBus(t, path, nullptr);
                break;
            }
            case NewTrackType::clickTrack:
                t = session.addTrack(session::TrackKind::bus, name.isNotEmpty() ? name : session.uniqueTrackName("Click"));
                session::track::setFormat(t, session::TrackFormat::mono, nullptr);
                session.addDevice(t, "builtin:click", "Click");
                break;
            case NewTrackType::folder:     t = session.addFolder(name); break;
            }
            if (!t.isValid())
                continue;

            auto* um = &session.getUndoManager();
            if (hasFormatChoice(row.type))
            {
                session::track::setFormat(t, row.format, um);
                if (row.type == NewTrackType::audio)
                    session::track::setInput(t, session::track::getInput(t), row.format == session::TrackFormat::stereo, um);
            }
            if (after.isValid())
                session.moveAfter(t, after);
            after = t;
            created.push_back(t);
        }
    }

    std::set<std::uint32_t> ids;
    for (const auto& t : created)
        ids.insert(session::track::getId(t));
    editor.setSelectedTracks(std::move(ids), created.empty() ? 0u : session::track::getId(created.front()));
    return created;
}

std::vector<juce::ValueTree> duplicateSelectedTracks(session::Project& project, EditorState& editor, const DuplicateSettings& settings)
{
    auto& session = project.getSession();
    const auto sources = getSelectedTracks(session, editor);
    std::vector<juce::ValueTree> created;
    if (sources.empty() || settings.copies < 1)
        return created;

    project.getDevices().storeStates(); // copies start from the devices' current settings
    session.beginTransaction(sources.size() * static_cast<std::size_t>(settings.copies) > 1 ? "Duplicate Tracks" : "Duplicate Track");

    if (settings.afterLastSelected)
    {
        auto after = sources.back();
        for (int k = 0; k < settings.copies; ++k)
            for (const auto& source : sources)
                if (auto copy = session.duplicateTrack(source, settings.options, after); copy.isValid())
                {
                    after = copy;
                    created.push_back(copy);
                }
    }
    else
    {
        for (const auto& source : sources)
        {
            auto after = source;
            for (int k = 0; k < settings.copies; ++k)
                if (auto copy = session.duplicateTrack(source, settings.options, after); copy.isValid())
                {
                    after = copy;
                    created.push_back(copy);
                }
        }
    }

    std::set<std::uint32_t> ids;
    for (const auto& t : created)
        ids.insert(session::track::getId(t));
    editor.setSelectedTracks(std::move(ids), created.empty() ? 0u : session::track::getId(created.front()));
    return created;
}

std::vector<juce::ValueTree> getSplittableTracks(const session::Session& session, const EditorState& editor)
{
    std::vector<juce::ValueTree> tracks;
    for (const auto& t : getSelectedTracks(session, editor))
        if (session::track::getKind(t) == session::TrackKind::audio && !session::track::isFolder(t)
            && session::track::getFormat(t) == session::TrackFormat::stereo)
            tracks.push_back(t);
    return tracks;
}

std::vector<juce::ValueTree> splitSelectedIntoMono(session::Project& project, EditorState& editor)
{
    auto& session = project.getSession();
    const auto sources = getSplittableTracks(session, editor);
    std::vector<juce::ValueTree> created;
    if (sources.empty())
        return created;

    project.getDevices().storeStates();
    session.beginTransaction("Split into Mono");
    auto* um = &session.getUndoManager();

    for (const auto& source : sources)
    {
        auto after = source;
        const int input = session::track::getInput(source);
        for (int channel = 0; channel < 2; ++channel)
        {
            const auto name = session.uniqueTrackName(session::track::getName(source) + (channel == 0 ? ".L" : ".R"));
            auto mono = session.duplicateTrack(source, {}, after, name);
            if (!mono.isValid())
                continue;
            session::track::setFormat(mono, session::TrackFormat::mono, um);
            session::track::setPan(mono, channel == 0 ? -1.0 : 1.0, um);
            session::track::setInput(mono, input >= 0 ? input + channel : -1, false, um);
            for (auto c : mono)
                if (c.hasType(session::ids::clip))
                    session::clip::setSourceChannel(c, channel, um);
            after = mono;
            created.push_back(mono);
        }
    }

    std::set<std::uint32_t> ids;
    for (const auto& t : created)
        ids.insert(session::track::getId(t));
    editor.setSelectedTracks(std::move(ids), created.empty() ? 0u : session::track::getId(created.front()));
    return created;
}

bool anySelectedTrackActive(const session::Session& session, const EditorState& editor)
{
    const auto selected = getSelectedTracks(session, editor);
    return std::any_of(selected.begin(), selected.end(), [](const juce::ValueTree& t) { return session::track::isActive(t); });
}

bool setSelectedTracksActive(session::Session& session, EditorState& editor, bool active)
{
    std::vector<juce::ValueTree> selected;
    for (const auto& t : session.getAllTracks())
        if (editor.isTrackSelected(session::track::getId(t)) && session::track::isActive(t) != active)
            selected.push_back(t);
    if (selected.empty())
        return false;
    session.beginTransaction(active ? "Make Active" : "Make Inactive");
    for (auto t : selected)
        session::track::setActive(t, active, &session.getUndoManager());
    return true;
}

namespace
{
bool hasLock(const juce::ValueTree& c, ClipLock lock)
{
    return lock == ClipLock::edit ? session::clip::isEditLocked(c) : session::clip::isTimeLocked(c);
}
} // namespace

bool selectedClipsLocked(const session::Session& session, const EditorState& editor, ClipLock lock)
{
    bool any = false;
    for (const auto id : editor.getSelectedClips())
        if (const auto c = session.findClip(id); c.isValid())
        {
            if (!hasLock(c, lock))
                return false;
            any = true;
        }
    return any;
}

bool toggleSelectedClipsLock(session::Session& session, EditorState& editor, ClipLock lock)
{
    const auto clips = selectedClips(session, editor);
    if (clips.empty())
        return false;

    const bool locking = !selectedClipsLocked(session, editor, lock);
    const bool edit = lock == ClipLock::edit;
    session.beginTransaction(juce::String(edit ? "Edit " : "Time ") + (locking ? "Lock" : "Unlock"));
    for (const auto& c : clips)
    {
        if (edit)
            session::clip::setEditLocked(c, locking, &session.getUndoManager());
        else
            session::clip::setTimeLocked(c, locking, &session.getUndoManager());
    }
    return true;
}

juce::ValueTree createClickTrack(session::Session& session, EditorState& editor)
{
    NewTracks row;
    row.type = NewTrackType::clickTrack;
    const auto created = addNewTracks(session, editor, {row});
    return created.empty() ? juce::ValueTree() : created.front();
}

juce::ValueTree getTrackByNumber(const session::Session& session, int number)
{
    return session.getTrack(number - 1);
}

} // namespace dojo::ui::actions

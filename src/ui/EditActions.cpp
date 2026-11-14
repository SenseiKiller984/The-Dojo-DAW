#include "ui/EditActions.h"

#include "session/Project.h"
#include "video/VideoSource.h"

#include <algorithm>
#include <cmath>
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
    std::vector<juce::ValueTree> copyTrees;
    for (const auto& c : session.withWholeGroups(clips))
        if (auto copy = session.duplicateClip(c); copy.isValid())
        {
            copies.insert(session::clip::getId(copy));
            copyTrees.push_back(copy);
        }
    session.renewGroups(copyTrees); // (copies of a group form a group of their own)

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
    std::vector<juce::ValueTree> pastedTrees;
    for (const auto& e : entries)
    {
        const bool midi = session::clip::isMidi(e.clip);
        const auto wanted = midi ? session::TrackKind::instrument
                            : session::clip::isVideo(e.clip) ? session::TrackKind::video
                                                             : session::TrackKind::audio;
        const int index = targetFirst + (e.trackIndex - firstTrack);
        juce::ValueTree target = index >= 0 && index < static_cast<int>(tracks.size()) ? tracks[static_cast<std::size_t>(index)] : juce::ValueTree();
        if (!target.isValid() || session::track::getKind(target) != wanted)
            target = selected.isValid() && session::track::getKind(selected) == wanted ? selected : juce::ValueTree();
        if (!target.isValid())
            target = session.addTrack(wanted, {}, -1);

        auto copy = session.pasteClip(e.clip, target, seconds + (session::clip::getStart(e.clip) - earliest));
        if (copy.isValid())
        {
            pasted.insert(session::clip::getId(copy));
            pastedTrees.push_back(copy);
        }
    }
    session.renewGroups(pastedTrees); // (pasted groups are groups of their own)

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
    case NewTrackType::video:       return "Video Track";
    case NewTrackType::audio:       break;
    }
    return "Audio Track";
}

bool hasFormatChoice(NewTrackType type)
{
    return type == NewTrackType::audio || type == NewTrackType::auxInput || type == NewTrackType::folder;
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
            case NewTrackType::video:      t = session.addTrack(session::TrackKind::video, name); break;
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
            && session::getNumChannels(session::track::getFormat(t)) >= 2)
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
        const auto layout = session::getLayout(session::track::getFormat(source));
        const bool hasRears = layout.has(engine::Speaker::leftRear);
        for (int channel = 0; channel < layout.numChannels; ++channel)
        {
            // One mono track per speaker ("Name.L", "Name.C", "Name.LFE", ...), placed at that speaker.
            const auto speaker = layout.speakers[static_cast<std::size_t>(channel)];
            const auto name = session.uniqueTrackName(session::track::getName(source) + "." + engine::shortName(speaker));
            auto mono = session.duplicateTrack(source, {}, after, name);
            if (!mono.isValid())
                continue;
            session::track::setFormat(mono, session::TrackFormat::mono, um);
            const int side = engine::sideOf(speaker);
            session::track::setPan(mono, static_cast<double>(side), um);
            const double frontRear = speaker == engine::Speaker::leftRear || speaker == engine::Speaker::rightRear ? 1.0
                                     : speaker == engine::Speaker::leftSurround || speaker == engine::Speaker::rightSurround
                                         ? (hasRears ? 0.0 : 1.0)
                                         : -1.0;
            session::track::setPanFrontRear(mono, frontRear, um);
            session::track::setPanCentre(mono, speaker == engine::Speaker::centre ? 1.0 : 0.0, um);
            session::track::setInput(mono, input >= 0 ? input + channel : -1, false, um);

            // Each clip plays the file channel that fed this speaker (a clip with none goes).
            std::vector<juce::ValueTree> silent;
            for (auto c : mono)
            {
                if (!c.hasType(session::ids::clip))
                    continue;
                const auto asset = project.getAssets().find(session::clip::getFile(c));
                const int fileChannels = asset != nullptr ? asset->numChannels : layout.numChannels;
                const int fileChannel = layout.numChannels == 2 ? channel : session::fileChannelFor(fileChannels, layout, channel);
                if (fileChannel < 0)
                    silent.push_back(c);
                else
                    session::clip::setSourceChannel(c, fileChannel, um);
            }
            for (auto& c : silent)
                mono.removeChild(c, um);
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

// --- Clip menu: ratings, groups, loops, sync points, clip gain, rename ------------------------------

std::vector<juce::ValueTree> getSelectedClipsWithGroups(const session::Session& session, const EditorState& editor)
{
    std::vector<juce::ValueTree> clips;
    for (const auto id : editor.getSelectedClips())
        if (auto c = session.findClip(id); c.isValid())
            clips.push_back(c);
    return session.withWholeGroups(clips);
}

namespace
{
std::set<std::uint32_t> idsOf(const std::vector<juce::ValueTree>& clips)
{
    std::set<std::uint32_t> ids;
    for (const auto& c : clips)
        ids.insert(session::clip::getId(c));
    return ids;
}
} // namespace

bool setSelectedClipsRating(session::Session& session, EditorState& editor, int rating)
{
    const auto clips = getSelectedClipsWithGroups(session, editor);
    if (clips.empty())
        return false;
    session.beginTransaction(rating == 0 ? juce::String("Clear Rating") : "Rate " + juce::String(rating));
    for (auto c : clips)
        session::clip::setRating(c, rating, &session.getUndoManager());
    return true;
}

int getSelectedClipsRating(const session::Session& session, const EditorState& editor)
{
    int rating = -1;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
    {
        const int r = session::clip::getRating(c);
        if (rating >= 0 && r != rating)
            return -1;
        rating = r;
    }
    return rating;
}

std::uint32_t groupSelectedClips(session::Session& session, EditorState& editor)
{
    const auto clips = getSelectedClipsWithGroups(session, editor);
    if (clips.empty())
        return 0;
    session.beginTransaction("Group Clips");
    const auto id = session.groupClips(clips);
    editor.setSelectedClips(idsOf(clips));
    return id;
}

bool ungroupSelectedClips(session::Session& session, EditorState& editor)
{
    std::set<std::uint32_t> groups;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (const auto outer = session::clip::getOuterGroup(c); outer != 0)
            groups.insert(outer);
    if (groups.empty())
        return false;

    session.beginTransaction("Ungroup Clips");
    std::vector<EditorState::UngroupedGroup> memory;
    for (const auto id : groups)
    {
        const auto name = session.getGroupName(id);
        const auto freed = idsOf(session.ungroup(id));
        memory.push_back({id, name, std::vector<std::uint32_t>(freed.begin(), freed.end())});
    }
    editor.setLastUngroup(std::move(memory));
    return true;
}

bool regroupClips(session::Session& session, EditorState& editor)
{
    const auto memory = editor.getLastUngroup();
    if (memory.empty())
        return false;
    session.beginTransaction("Regroup Clips");
    std::set<std::uint32_t> selection;
    for (const auto& group : memory)
    {
        std::vector<juce::ValueTree> clips;
        for (const auto id : group.clipIds)
            if (auto c = session.findClip(id); c.isValid())
                clips.push_back(c);
        session.regroup(group.groupId, clips, group.name);
        for (const auto& c : clips)
            selection.insert(session::clip::getId(c));
    }
    editor.setLastUngroup({});
    editor.setSelectedClips(std::move(selection));
    return true;
}

bool ungroupAllSelectedClips(session::Session& session, EditorState& editor)
{
    std::set<std::uint32_t> groups;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (const auto outer = session::clip::getOuterGroup(c); outer != 0)
            groups.insert(outer);
    if (groups.empty())
        return false;
    session.beginTransaction("Ungroup All");
    for (const auto id : groups)
        session.ungroupAll(id);
    editor.setLastUngroup({});
    return true;
}

bool selectionHasGroups(const session::Session& session, const EditorState& editor)
{
    for (const auto id : editor.getSelectedClips())
        if (const auto c = session.findClip(id); c.isValid() && session::clip::isGrouped(c))
            return true;
    return false;
}

bool loopSelectedClips(session::Session& session, EditorState& editor, double iterations)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (session::clip::canEdit(c))
            clips.push_back(c);
    if (clips.empty() || iterations < 1.0)
        return false;
    session.beginTransaction("Loop Clips");
    for (auto c : clips)
        session.loopClip(c, iterations);
    return true;
}

bool unloopSelectedClips(session::Session& session, EditorState& editor)
{
    std::vector<juce::ValueTree> looped;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (session::clip::isLooped(c) && session::clip::canEdit(c))
            looped.push_back(c);
    if (looped.empty())
        return false;
    session.beginTransaction("Unloop Clips");
    std::set<std::uint32_t> selection = editor.getSelectedClips();
    for (auto c : looped)
        for (const auto& piece : session.unloopClip(c))
            selection.insert(session::clip::getId(piece));
    editor.setSelectedClips(std::move(selection));
    return true;
}

bool selectionHasLoopedClips(const session::Session& session, const EditorState& editor)
{
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (session::clip::isLooped(c))
            return true;
    return false;
}

bool identifySyncPoint(session::Session& session, EditorState& editor, double seconds)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (session::clip::canEdit(c) && seconds >= session::clip::getStart(c) && seconds <= session::clip::getEnd(c))
            clips.push_back(c);
    if (clips.empty())
        return false;
    session.beginTransaction("Identify Sync Point");
    for (auto c : clips)
        session.identifySyncPoint(c, seconds);
    return true;
}

bool removeSyncPoints(session::Session& session, EditorState& editor)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (session::clip::getSyncPoint(c).has_value() && session::clip::canEdit(c))
            clips.push_back(c);
    if (clips.empty())
        return false;
    session.beginTransaction("Remove Sync Point");
    for (auto c : clips)
        session::clip::setSyncPoint(c, std::nullopt, &session.getUndoManager());
    return true;
}

bool selectionHasSyncPoints(const session::Session& session, const EditorState& editor)
{
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (session::clip::getSyncPoint(c).has_value())
            return true;
    return false;
}

bool quantizeSelectedClipsToGrid(session::Session& session, EditorState& editor, double gridSeconds)
{
    if (gridSeconds <= 0.0)
        return false;
    std::vector<juce::ValueTree> clips;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (session::clip::canMoveInTime(c))
            clips.push_back(c);
    if (clips.empty())
        return false;
    session.beginTransaction("Quantize Clips to Grid");
    for (auto c : clips)
    {
        // The sync point (or the start) goes to the nearest grid line.
        const double reference = session::clip::getReferenceTime(c);
        const double target = std::round(reference / gridSeconds) * gridSeconds;
        session.moveClip(c, c.getParent(), std::max(0.0, session::clip::getStart(c) + (target - reference)));
    }
    return true;
}

bool toggleSelectedClipsGainBypass(session::Session& session, EditorState& editor)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (c.hasType(session::ids::clip) && session::clip::canEdit(c))
            clips.push_back(c);
    if (clips.empty())
        return false;
    const bool bypass = !std::all_of(clips.begin(), clips.end(), [](const juce::ValueTree& c) { return session::clip::isGainBypassed(c); });
    session.beginTransaction(bypass ? "Bypass Clip Gain" : "Enable Clip Gain");
    for (auto c : clips)
        session::clip::setGainBypassed(c, bypass, &session.getUndoManager());
    return true;
}

bool selectedClipsGainBypassed(const session::Session& session, const EditorState& editor)
{
    bool any = false;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
    {
        if (!c.hasType(session::ids::clip))
            continue;
        if (!session::clip::isGainBypassed(c))
            return false;
        any = true;
    }
    return any;
}

int renderSelectedClipsGain(session::Project& project, EditorState& editor, juce::String& error)
{
    auto& session = project.getSession();
    std::vector<juce::ValueTree> clips;
    for (const auto& c : getSelectedClipsWithGroups(session, editor))
        if (c.hasType(session::ids::clip) && session::clip::canEdit(c) && !session::clip::isGainBypassed(c)
            && std::abs(session::clip::getGainDb(c)) > 1.0e-6)
            clips.push_back(c);
    if (clips.empty())
    {
        error = "Select audio clips that have clip gain (and are not bypassed or edit-locked).";
        return 0;
    }

    const auto folder = project.getFile() != juce::File()
                            ? project.getFile().getParentDirectory().getChildFile(project.getFile().getFileNameWithoutExtension() + " Rendered")
                            : juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("The Dojo").getChildFile("Rendered");
    if (!folder.createDirectory())
    {
        error = "Could not create " + folder.getFullPathName();
        return 0;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    int rendered = 0;
    auto* um = &session.getUndoManager();
    session.beginTransaction("Render Clip Gain");
    for (auto c : clips)
    {
        const auto file = session::clip::getFile(c);
        std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (reader == nullptr)
        {
            error = "Could not read " + file.getFileName();
            continue;
        }

        // The part of the file the clip plays (one iteration of a looped clip).
        const double stretch = std::max(session::kMinStretch, session::clip::getStretch(c));
        const double rate = reader->sampleRate;
        const auto first = static_cast<juce::int64>(std::floor(session::clip::getOffset(c) * rate));
        const auto count = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples - first,
                                                                  static_cast<juce::int64>(std::ceil(session::clip::getLoopLength(c) / stretch * rate)) + 1));
        if (count <= 0)
            continue;
        const int channels = static_cast<int>(reader->numChannels);
        juce::AudioBuffer<float> audio(channels, count);
        reader->read(&audio, 0, count, first, true, true);
        audio.applyGain(juce::Decibels::decibelsToGain(static_cast<float>(session::clip::getGainDb(c)), -100.0f));

        const auto target = folder.getNonexistentChildFile(juce::File::createLegalFileName(session::clip::getName(c) + " (clip gain)"), ".wav", false);
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(target);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(rate)
                                                      .withNumChannels(channels)
                                                      .withBitsPerSample(32)
                                                      .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr || !writer->writeFromAudioSampleBuffer(audio, 0, count))
        {
            error = "Could not write " + target.getFullPathName();
            continue;
        }
        writer.reset();

        // The clip now plays the rendered file from its start, at 0 dB (the file has a whole-file clip of
        // its own, time stamped like the material it came from).
        session.addWholeFileClip(target, static_cast<double>(count) / rate, false, session.getOriginalTimeStamp(c));
        c.setProperty(session::ids::timeStamp, session.getOriginalTimeStamp(c), um);
        c.setProperty(session::ids::file, target.getFullPathName(), um);
        session::clip::setOffset(c, 0.0, um);
        session::clip::setGainDb(c, 0.0, um);
        ++rendered;
    }
    return rendered;
}

bool renameClip(session::Session& session, juce::ValueTree clip, const juce::String& name)
{
    if (!clip.isValid() || name.trim().isEmpty())
        return false;
    session.beginTransaction("Rename Clip");
    session::clip::setName(clip, name.trim(), &session.getUndoManager());
    clip.setProperty(session::ids::userNamed, true, &session.getUndoManager());
    return true;
}

CaptureRange getCaptureRange(const session::Session& session, const EditorState& editor, juce::String& error)
{
    const auto hasMaterial = [](const juce::ValueTree& track, double start, double end)
    {
        for (const auto& c : track)
            if (session::Session::isClip(c) && session::clip::getEnd(c) > start + 1.0e-9 && session::clip::getStart(c) < end - 1.0e-9)
                return true;
        return false;
    };

    if (const auto& selection = editor.getTimeSelection(); !selection.isEmpty())
    {
        std::vector<juce::ValueTree> tracks;
        for (const auto id : selection.trackIds)
            if (const auto t = session.findTrack(id); t.isValid() && hasMaterial(t, selection.start, selection.end))
                tracks.push_back(t);
        if (tracks.size() == 1)
            return {tracks.front(), selection.start, selection.end};
        error = tracks.empty() ? "There is no clip in the selection." : "Capture works on one track at a time: select a range on one track.";
        return {};
    }

    juce::ValueTree track;
    double start = 1.0e12;
    double end = 0.0;
    for (const auto id : editor.getSelectedClips())
    {
        const auto c = session.findClip(id);
        if (!c.isValid())
            continue;
        if (track.isValid() && c.getParent() != track)
        {
            error = "Capture works on one track at a time: select clips on one track.";
            return {};
        }
        track = c.getParent();
        start = std::min(start, session::clip::getStart(c));
        end = std::max(end, session::clip::getEnd(c));
    }
    if (!track.isValid())
    {
        error = "Select a clip, or drag over a range of a track, to capture.";
        return {};
    }
    return {track, start, end};
}

juce::String suggestCaptureName(const session::Session& session, const CaptureRange& range)
{
    juce::String base = "Capture";
    for (const auto& c : range.track)
        if (session::Session::isClip(c) && session::clip::getEnd(c) > range.start + 1.0e-9 && session::clip::getStart(c) < range.end - 1.0e-9)
        {
            base = session::clip::getName(c) + " Capture";
            break;
        }
    const auto taken = [&](const juce::String& name)
    {
        const auto entries = session.getClipListEntries();
        return std::any_of(entries.begin(), entries.end(), [&](const juce::ValueTree& e) { return session::clip::getName(e) == name; });
    };
    juce::String name = base;
    for (int n = 2; taken(name); ++n)
        name = base + " " + juce::String(n);
    return name;
}

juce::ValueTree captureSelection(session::Session& session, EditorState& editor, const juce::String& name, juce::String& error)
{
    const auto range = getCaptureRange(session, editor, error);
    if (!range.track.isValid())
        return {};
    session.beginTransaction("Capture Clip");
    return session.captureClip(range.track, range.start, range.end, name.trim().isNotEmpty() ? name : suggestCaptureName(session, range), error);
}

juce::ValueTree importVideo(session::Session& session, EditorState& editor, const juce::File& file, double seconds,
                            const juce::ValueTree& target, juce::String& error)
{
    auto source = video::openVideo(file, error);
    if (source == nullptr)
        return {};
    const auto& info = source->getInfo();

    juce::ValueTree t = target.isValid() && session::track::getKind(target) == session::TrackKind::video ? target : juce::ValueTree();
    if (!t.isValid())
        for (const auto& candidate : session.getAllTracks())
            if (session::track::getKind(candidate) == session::TrackKind::video)
            {
                t = candidate;
                break;
            }

    session.beginTransaction("Import Video");
    if (!t.isValid())
        t = session.addTrack(session::TrackKind::video, {}, 0); // video sits on top, like the picture above the mix

    const double length = info.durationSeconds > 0.0 ? info.durationSeconds : 1.0;
    session.addWholeFileClip(file, length, true, std::max(0.0, seconds));
    auto c = session.addVideoClip(t, file, std::max(0.0, seconds), length);
    editor.selectTrack(session::track::getId(t));
    editor.setSelectedClips({session::clip::getId(c)});
    return c;
}

VideoAtTime findVideoAt(const session::Session& session, double seconds)
{
    for (const auto& t : session.getAllTracks())
    {
        if (session::track::getKind(t) != session::TrackKind::video || session::track::isMuted(t) || !session::Session::isEffectivelyActive(t))
            continue;
        // Overlapping clips: the one drawn on top (added last) is the one seen.
        for (int i = t.getNumChildren(); --i >= 0;)
        {
            const auto c = t.getChild(i);
            if (c.hasType(session::ids::videoClip) && seconds >= session::clip::getStart(c) && seconds < session::clip::getEnd(c))
            {
                // (a looped clip shows its first loop-length again and again)
                double inClip = seconds - session::clip::getStart(c);
                if (session::clip::isLooped(c))
                    inClip = std::fmod(inClip, session::clip::getLoopLength(c));
                return {c, session::clip::getFile(c), session::clip::getOffset(c) + inClip};
            }
        }
    }
    return {};
}

juce::ValueTree getTrackByNumber(const session::Session& session, int number)
{
    return session.getTrack(number - 1);
}

} // namespace dojo::ui::actions

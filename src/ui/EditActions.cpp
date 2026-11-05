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

void selectAllClips(session::Session& session, EditorState& editor)
{
    std::set<std::uint32_t> all;
    for (const auto& t : session.getAllTracks())
        for (const auto& c : t)
            if (session::Session::isClip(c))
                all.insert(session::clip::getId(c));

    editor.setSelectedClips(std::move(all));
}

juce::ValueTree addTrack(session::Session& session, EditorState& editor)
{
    const auto selected = session.findTrack(editor.getSelectedTrack());
    const int index = selected.isValid() ? session.indexOf(selected) + 1 : -1;

    session.beginTransaction("Add Track");
    auto t = session.addTrack({}, index);
    editor.selectTrack(session::track::getId(t));
    return t;
}

bool deleteSelectedTrack(session::Session& session, EditorState& editor)
{
    const auto selected = session.findTrack(editor.getSelectedTrack());
    if (!selected.isValid())
        return false;

    const int index = session.indexOf(selected);
    session.beginTransaction("Delete Track");
    session.removeTrack(selected);
    editor.clearClipSelection();

    // Keep a sensible selection: the track that moved into the deleted one's place, or the new last.
    const int count = session.getNumTracks();
    editor.selectTrack(count > 0 ? session::track::getId(session.getTrack(juce::jmin(index, count - 1)))
                                 : 0u);
    return true;
}

} // namespace dojo::ui::actions

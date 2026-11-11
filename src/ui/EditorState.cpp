#include "ui/EditorState.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace dojo::ui
{

void EditorState::notify()
{
    listeners_.call([](Listener& l) { l.editorStateChanged(); });
}

void EditorState::selectClip(std::uint32_t clipId, bool addToSelection)
{
    if (!addToSelection)
        selectedClips_.clear();

    selectedClips_.insert(clipId);
    notify();
}

void EditorState::toggleClip(std::uint32_t clipId)
{
    if (!selectedClips_.erase(clipId))
        selectedClips_.insert(clipId);

    notify();
}

void EditorState::setSelectedClips(std::set<std::uint32_t> clipIds)
{
    selectedClips_ = std::move(clipIds);
    notify();
}

void EditorState::clearClipSelection()
{
    if (selectedClips_.empty())
        return;

    selectedClips_.clear();
    notify();
}

void EditorState::selectTrack(std::uint32_t trackId)
{
    trackAnchor_ = trackId;
    const bool onlyThis = trackId == 0 ? selectedTracks_.empty() : (selectedTracks_.size() == 1 && isTrackSelected(trackId));
    if (selectedTrack_ == trackId && onlyThis && !masterSelected_)
        return;

    selectedTrack_ = trackId;
    selectedTracks_.clear();
    if (trackId != 0)
        selectedTracks_.insert(trackId);
    masterSelected_ = false;
    notify();
}

void EditorState::selectTrackWithModifiers(std::uint32_t trackId, bool toggle, bool range, const std::vector<std::uint32_t>& order)
{
    const auto position = [&order](std::uint32_t id)
    {
        const auto it = std::find(order.begin(), order.end(), id);
        return it != order.end() ? static_cast<int>(it - order.begin()) : -1;
    };

    const int clicked = position(trackId);
    if (trackId == 0 || clicked < 0 || (!toggle && !range))
    {
        selectTrack(trackId);
        return;
    }

    if (range)
    {
        // Without an anchor (or with one no longer shown), the current track serves as one.
        if (position(trackAnchor_) < 0)
            trackAnchor_ = position(selectedTrack_) >= 0 ? selectedTrack_ : trackId;
        const int anchor = position(trackAnchor_);
        std::set<std::uint32_t> span;
        for (int i = std::min(anchor, clicked); i <= std::max(anchor, clicked); ++i)
            span.insert(order[static_cast<std::size_t>(i)]);

        if (toggle)
            selectedTracks_.insert(span.begin(), span.end());
        else
            selectedTracks_ = std::move(span);
        selectedTrack_ = trackId;
    }
    else if (!isTrackSelected(trackId))
    {
        selectedTracks_.insert(trackId);
        selectedTrack_ = trackId;
        trackAnchor_ = trackId;
    }
    else if (selectedTracks_.size() > 1)
    {
        selectedTracks_.erase(trackId);
        trackAnchor_ = trackId;
        if (selectedTrack_ == trackId || !isTrackSelected(selectedTrack_))
        {
            // The nearest selected track (as shown) becomes the current one.
            int best = -1;
            for (int i = 0; i < static_cast<int>(order.size()); ++i)
                if (isTrackSelected(order[static_cast<std::size_t>(i)]) && (best < 0 || std::abs(i - clicked) < std::abs(best - clicked)))
                    best = i;
            selectedTrack_ = best >= 0 ? order[static_cast<std::size_t>(best)] : *selectedTracks_.begin();
        }
    }
    else
    {
        trackAnchor_ = trackId; // Ctrl+click on the only selected track keeps it
    }

    masterSelected_ = false;
    notify();
}

void EditorState::selectMaster()
{
    if (masterSelected_)
        return;
    masterSelected_ = true;
    notify();
}

void EditorState::setEditedClip(std::uint32_t clipId)
{
    if (editedClip_ == clipId)
        return;
    editedClip_ = clipId;
    notify();
}

void EditorState::setSnapEnabled(bool enabled)
{
    if (snapEnabled_ != enabled)
    {
        snapEnabled_ = enabled;
        notify();
    }
}

void EditorState::setGrid(Grid grid)
{
    if (grid_ != grid)
    {
        grid_ = grid;
        notify();
    }
}

double EditorState::getGridBeats() const noexcept
{
    switch (grid_)
    {
    case Grid::bar:
        return 4.0;
    case Grid::beat:
        return 1.0;
    case Grid::halfBeat:
        return 0.5;
    case Grid::quarterBeat:
        return 0.25;
    }

    return 1.0;
}

double EditorState::snap(double seconds, double tempoBpm, bool bypass) const noexcept
{
    if (!snapEnabled_ || bypass || tempoBpm <= 0.0)
        return seconds;

    const double gridSeconds = getGridBeats() * 60.0 / tempoBpm;
    return std::round(seconds / gridSeconds) * gridSeconds;
}

} // namespace dojo::ui

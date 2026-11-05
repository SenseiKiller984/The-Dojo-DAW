#include "ui/EditorState.h"

#include <cmath>

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
    if (selectedTrack_ == trackId)
        return;

    selectedTrack_ = trackId;
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

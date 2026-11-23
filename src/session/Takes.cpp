// Take lanes (playlists) and comping: the Session's take lane functions.

#include "session/Session.h"

#include <algorithm>
#include <cmath>

namespace dojo::session
{
namespace
{
constexpr double kEpsilon = 1.0e-9;
constexpr double kSameTime = 1.0e-6; ///< (seconds: two edits this close are the same one)

/** The comp: a track's audio and MIDI clips. */
std::vector<juce::ValueTree> compClipsOf(const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& c : track)
        if (c.hasType(ids::clip) || c.hasType(ids::midiClip))
            clips.push_back(c);
    return clips;
}

/** True if `segment` plays what `take` plays at the same moments (it was cut from the take and not
    moved, stretched or warped since): the comp can go on from one to the other with a plain cut.
    Clip gain and fades may differ. */
bool playsSameMaterial(const juce::ValueTree& segment, const juce::ValueTree& take, const time::TempoMap& map)
{
    if (segment.getType() != take.getType() || clip::isLooped(segment) || clip::isLooped(take))
        return false;
    const double into = clip::getStart(segment) - clip::getStart(take);

    if (clip::isMidi(segment))
    {
        const double intoBeats = map.beatAt(clip::getStart(segment)) - map.beatAt(clip::getStart(take));
        if (std::abs(clip::getOffset(segment) - (clip::getOffset(take) + intoBeats)) > kSameTime
            || segment.getNumChildren() != take.getNumChildren())
            return false;
        for (int i = 0; i < segment.getNumChildren(); ++i)
            if (!segment.getChild(i).isEquivalentTo(take.getChild(i)))
                return false;
        return true;
    }

    if (clip::getFile(segment) != clip::getFile(take) || clip::getSourceChannel(segment) != clip::getSourceChannel(take)
        || std::abs(clip::getPitch(segment) - clip::getPitch(take)) > 1.0e-9)
        return false;
    const double length = clip::getLength(segment);
    for (const double at : {0.0, 0.5 * length, length})
        if (std::abs(clip::sourceAt(segment, at) - clip::sourceAt(take, into + at)) > kSameTime)
            return false;
    return true;
}

/** ... and left as it was cut: the take's gain, no locks or groups (it can be cut afresh). */
bool isUntouchedCopy(const juce::ValueTree& segment, const juce::ValueTree& take, const time::TempoMap& map)
{
    if (!playsSameMaterial(segment, take, map) || clip::isEditLocked(segment) || clip::isTimeLocked(segment)
        || clip::isGrouped(segment))
        return false;
    return clip::isMidi(segment)
           || (std::abs(clip::getGainDb(segment) - clip::getGainDb(take)) < 1.0e-9 && clip::isGainBypassed(segment) == clip::isGainBypassed(take));
}

/** What Session::clearRange trims (it leaves locked and looped clips alone). */
bool isTrimmable(const juce::ValueTree& c)
{
    return !clip::isEditLocked(c) && !clip::isTimeLocked(c) && !clip::isLooped(c);
}

/** "Take 12" -> 12 (for prefix "Take"); 0 for any other name. */
int numberIn(const juce::String& name, const juce::String& prefix)
{
    if (!name.startsWith(prefix + " "))
        return 0;
    const auto rest = name.substring(prefix.length() + 1).trim();
    return rest.isNotEmpty() && rest.containsOnly("0123456789") ? rest.getIntValue() : 0;
}

/** A copy of `c` for another place (a lane or the comp): fresh ids are given by the caller; no
    locks, groups or take link. */
juce::ValueTree plainCopyOf(const juce::ValueTree& c)
{
    auto copy = c.createCopy();
    for (const auto& property : {ids::editLocked, ids::timeLocked, ids::clipGroups, ids::fromTake})
        copy.removeProperty(property, nullptr);
    return copy;
}
} // namespace

std::vector<juce::ValueTree> Session::getTakeLanes(const juce::ValueTree& track)
{
    std::vector<juce::ValueTree> lanes;
    for (const auto& child : track)
        if (child.hasType(ids::takeLane))
            lanes.push_back(child);
    return lanes;
}

juce::ValueTree Session::findTakeLane(std::uint32_t laneId) const
{
    if (laneId == 0)
        return {};
    for (const auto& t : getAllTracks())
        for (const auto& lane : getTakeLanes(t))
            if (takelane::getId(lane) == laneId)
                return lane;
    return {};
}

juce::ValueTree Session::getTakeLaneOf(const juce::ValueTree& compClip) const
{
    const auto id = clip::getTake(compClip);
    if (id == 0)
        return {};
    for (const auto& lane : getTakeLanes(compClip.getParent()))
        if (takelane::getId(lane) == id)
            return lane;
    return {};
}

juce::ValueTree Session::addTakeLane(juce::ValueTree track, const juce::String& name)
{
    if (!track.isValid() || !track.hasType(ids::track))
        return {};

    auto laneName = name;
    if (laneName.isEmpty())
    {
        int highest = 0;
        for (const auto& lane : getTakeLanes(track))
            highest = std::max(highest, numberIn(takelane::getName(lane), "Take"));
        laneName = "Take " + juce::String(highest + 1);
    }

    juce::ValueTree lane(ids::takeLane);
    lane.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    lane.setProperty(ids::name, laneName, nullptr);
    track.addChild(lane, -1, &undo_);
    return lane;
}

juce::ValueTree Session::addTakeLaneFromComp(juce::ValueTree track)
{
    if (!track.isValid() || !track.hasType(ids::track))
        return {};

    int highest = 0;
    for (const auto& lane : getTakeLanes(track))
        highest = std::max(highest, numberIn(takelane::getName(lane), "Comp"));
    auto lane = addTakeLane(track, "Comp " + juce::String(highest + 1));
    const auto laneId = takelane::getId(lane);

    for (auto c : compClipsOf(track))
    {
        auto copy = plainCopyOf(c);
        reassignIds(copy);
        lane.addChild(copy, -1, &undo_);
        if (!getTakeLaneOf(c).isValid()) // (its material is in a take lane now)
            clip::setTake(c, laneId, &undo_);
    }
    return lane;
}

juce::ValueTree Session::keepCompInTakeLane(juce::ValueTree track, double start, double end)
{
    std::vector<juce::ValueTree> loose;
    for (const auto& c : compClipsOf(track))
        if (clip::getEnd(c) > start + kEpsilon && clip::getStart(c) < end - kEpsilon && !getTakeLaneOf(c).isValid())
            loose.push_back(c);
    if (loose.empty())
        return {};

    auto lane = addTakeLane(track);
    for (auto c : loose)
    {
        auto copy = plainCopyOf(c);
        reassignIds(copy);
        lane.addChild(copy, -1, &undo_);
        clip::setTake(c, takelane::getId(lane), &undo_);
    }
    return lane;
}

void Session::removeTakeLane(juce::ValueTree lane)
{
    auto track = lane.getParent();
    if (!lane.hasType(ids::takeLane) || !track.isValid())
        return;

    const auto laneId = takelane::getId(lane);
    for (auto c : compClipsOf(track))
        if (clip::getTake(c) == laneId)
            clip::setTake(c, 0, &undo_);
    if (track::getAuditionedTake(track) == laneId)
        track::setAuditionedTake(track, 0, nullptr);
    track.removeChild(lane, &undo_);
}

int Session::removeUnusedTakeLanes(juce::ValueTree track)
{
    int removed = 0;
    for (auto lane : getTakeLanes(track))
        if (getCompedRanges(lane).empty())
        {
            removeTakeLane(lane);
            ++removed;
        }
    return removed;
}

void Session::removeAllTakeLanes(juce::ValueTree track)
{
    for (auto lane : getTakeLanes(track))
        removeTakeLane(lane);
}

std::vector<std::pair<double, double>> Session::getCompedRanges(const juce::ValueTree& lane)
{
    std::vector<std::pair<double, double>> ranges;
    const auto laneId = takelane::getId(lane);
    for (const auto& c : lane.getParent())
        if (isClip(c) && clip::getTake(c) == laneId)
            ranges.emplace_back(clip::getStart(c), clip::getEnd(c));
    std::sort(ranges.begin(), ranges.end());

    std::vector<std::pair<double, double>> merged;
    for (const auto& range : ranges)
    {
        if (!merged.empty() && range.first <= merged.back().second + kEpsilon)
            merged.back().second = std::max(merged.back().second, range.second);
        else
            merged.push_back(range);
    }
    return merged;
}

//==================================================================================================
// Comping

void Session::compTake(juce::ValueTree lane, double start, double end)
{
    auto track = lane.getParent();
    start = std::max(0.0, start);
    if (!lane.hasType(ids::takeLane) || !track.isValid() || end - start < kMinClipLength)
        return;

    const auto laneId = takelane::getId(lane);
    const auto& tempoMap = getTempoMap();
    const auto takes = takelane::getClips(lane);
    const bool midi = std::any_of(takes.begin(), takes.end(), [](const juce::ValueTree& c) { return clip::isMidi(c); });
    const double crossfade = midi ? 0.0 : getCompCrossfade();

    const auto isUntouchedSegment = [&](const juce::ValueTree& c)
    {
        return clip::getTake(c) == laneId
               && std::any_of(takes.begin(), takes.end(), [&](const juce::ValueTree& take) { return isUntouchedCopy(c, take, tempoMap); });
    };

    // 1. Stretches of the comp at (or within a crossfade of) the range that already play this take
    //    untouched join it, so the take is cut once more as one clip; their outer edits stay.
    juce::ValueTree keptBefore, keptAfter; // the joined segments whose outer edits bound the range
    for (bool grown = true; grown;)
    {
        grown = false;
        for (const auto& c : compClipsOf(track))
        {
            const double cs = clip::getStart(c), ce = clip::getEnd(c);
            const bool inside = cs >= start - kEpsilon && ce <= end + kEpsilon;
            if (inside || ce < start - crossfade - kEpsilon || cs > end + crossfade + kEpsilon || !isUntouchedSegment(c))
                continue;
            if (cs < start - kEpsilon)
            {
                start = cs;
                keptBefore = c;
                grown = true;
            }
            if (ce > end + kEpsilon)
            {
                end = ce;
                keptAfter = c;
                grown = true;
            }
        }
    }

    // 2. Plan both edits (before anything changes).
    const double half = std::min(0.5 * crossfade, (end - start) / 6.0);
    const auto clips = compClipsOf(track);

    // The take that plays on the inner side of an edit (the last one: it is drawn on top).
    const auto takeInside = [&](double time, bool leading)
    {
        juce::ValueTree found;
        for (const auto& take : takes)
        {
            const double ts = clip::getStart(take), te = clip::getEnd(take);
            if (leading ? (ts <= time + kEpsilon && te > time + kEpsilon) : (ts < time - kEpsilon && te >= time - kEpsilon))
                found = take;
        }
        return found;
    };

    struct Edit
    {
        double reach = 0.0;        ///< the new material reaches out to here
        double keep = 0.0;         ///< the comp's material stays up to (leading) / from (trailing) here
        juce::ValueTree neighbour; ///< the comp clip the edit is made against (cut at `keep`)
        double neighbourEdge = 0.0; ///< its end (leading) / start (trailing) before the cut
        double neighbourEnd = 0.0;  ///< its end before the cut (what is left of it ends there too)
        bool fadeNeighbour = false; ///< silence comes in: the neighbour fades out (in) quickly
        bool fadeNew = false;       ///< the new material starts (ends) inside its take, in silence
        bool equalGain = false;     ///< the same material on both sides: a linear (equal-gain) crossfade
        juce::ValueTree touching;   ///< a clip ending (starting) right at the edit, overlapped by the new material
        double keptFade = -1.0;     ///< an edit kept from a joined segment: the fade it had
        std::vector<juce::ValueTree> slivers; ///< other clips across the edit, inside an earlier crossfade
        std::vector<double> sliverEnds;       ///< ... and their ends before the cut
    };

    const auto planEdit = [&](double time, bool leading, const juce::ValueTree& kept)
    {
        Edit edit;
        edit.reach = edit.keep = time;
        const auto outward = [leading, time](double d) { return leading ? time - d : time + d; };
        const auto inward = [leading, time](double d) { return leading ? time + d : time - d; };
        const auto across = [time](const juce::ValueTree& c) { return clip::getStart(c) < time - kEpsilon && clip::getEnd(c) > time + kEpsilon; };

        if (kept.isValid())
        {
            // A joined segment's outer edit stays as it was, its crossfade partner untouched.
            for (const auto& c : clips)
            {
                if (c == kept || !across(c))
                    continue;
                if (leading && clip::getEnd(c) <= time + 2.0 * crossfade + kEpsilon)
                    edit.keep = std::max(edit.keep, clip::getEnd(c));
                if (!leading && clip::getStart(c) >= time - 2.0 * crossfade - kEpsilon)
                    edit.keep = std::min(edit.keep, clip::getStart(c));
            }
            edit.keptFade = leading ? clip::getFadeIn(kept) : clip::getFadeOut(kept);
            return edit;
        }

        // The edit is made against the clip across it that reaches furthest out; others across it
        // lie inside an earlier crossfade and go.
        for (const auto& c : clips)
            if (isTrimmable(c) && across(c))
                if (!edit.neighbour.isValid()
                    || (leading ? clip::getStart(c) < clip::getStart(edit.neighbour) : clip::getEnd(c) > clip::getEnd(edit.neighbour)))
                    edit.neighbour = c;
        for (const auto& c : clips)
            if (isTrimmable(c) && across(c) && c != edit.neighbour)
            {
                edit.slivers.push_back(c);
                edit.sliverEnds.push_back(clip::getEnd(c));
            }
        if (edit.neighbour.isValid())
        {
            edit.neighbourEdge = leading ? clip::getEnd(edit.neighbour) : clip::getStart(edit.neighbour);
            edit.neighbourEnd = clip::getEnd(edit.neighbour);
        }

        const auto take = takeInside(time, leading);
        if (!take.isValid())
        {
            edit.fadeNeighbour = edit.neighbour.isValid() && crossfade > 0.0;
            return edit; // silence comes in
        }
        const double spareNew = leading ? time - clip::getStart(take) : clip::getEnd(take) - time;

        if (edit.neighbour.isValid())
        {
            // Different takes crossfade with equal power; where the same material goes on (its gain
            // changed, say) the crossfade is equal-gain, so nothing dips or swells.
            edit.equalGain = playsSameMaterial(edit.neighbour, take, tempoMap);
            const double spareOld = leading ? clip::getEnd(edit.neighbour) - time : time - clip::getStart(edit.neighbour);
            double newSide = std::min(half, spareNew);
            double oldSide = std::min(half, spareOld);
            if (newSide < half)
                oldSide = std::min(2.0 * half - newSide, spareOld);
            else if (oldSide < half)
                newSide = std::min(2.0 * half - oldSide, spareNew);
            edit.reach = outward(newSide);
            edit.keep = inward(oldSide);
            return edit;
        }

        // A clip that ends (starts) right at the edit: the crossfade lies on its side.
        for (const auto& c : clips)
            if (std::abs((leading ? clip::getEnd(c) : clip::getStart(c)) - time) <= kSameTime)
            {
                edit.reach = outward(std::min(2.0 * half, spareNew));
                edit.equalGain = playsSameMaterial(c, take, tempoMap);
                if (edit.equalGain && isTrimmable(c))
                    edit.touching = c;
                return edit;
            }

        edit.fadeNew = spareNew > kEpsilon && crossfade > 0.0;
        return edit;
    };

    const auto leftTake = takeInside(start, true);
    const auto rightTake = takeInside(end, false);
    const auto first = planEdit(start, true, keptBefore);
    const auto last = planEdit(end, false, keptAfter);

    // 3. Clear: this take's untouched stretches inside the range (cut afresh below), then the rest
    //    between the edits.
    for (const auto& c : clips)
        if (clip::getStart(c) >= start - kEpsilon && clip::getEnd(c) <= end + kEpsilon && isUntouchedSegment(c))
            removeClip(c);
    if (last.keep > first.keep + kEpsilon)
        clearRange(track, first.keep, last.keep);

    // Clips that lay inside an earlier crossfade at an edit: what is left of them goes.
    for (auto c : first.slivers)
        if (c.getParent() == track && clip::getEnd(c) <= first.keep + kSameTime)
            removeClip(c);
    juce::ValueTree rightPiece; // what is left of the trailing neighbour
    for (const auto& c : compClipsOf(track))
    {
        if (std::abs(clip::getStart(c) - last.keep) > kSameTime)
            continue;
        if (last.neighbour.isValid() && std::abs(clip::getEnd(c) - last.neighbourEnd) <= kSameTime && !rightPiece.isValid())
            rightPiece = c;
        else if (std::any_of(last.sliverEnds.begin(), last.sliverEnds.end(),
                             [&](double sliverEnd) { return std::abs(sliverEnd - clip::getEnd(c)) <= kSameTime; }))
            removeClip(c);
    }
    // Anything else left wholly inside a crossfade (a clip that began right at an edit, say) goes:
    // there, only the neighbour and the new material play.
    for (const auto& c : compClipsOf(track))
    {
        if (c == first.neighbour || c == rightPiece || !isTrimmable(c))
            continue;
        const double cs = clip::getStart(c), ce = clip::getEnd(c);
        const bool inFirst = first.keep > first.reach + kSameTime && cs >= first.reach - kSameTime && ce <= first.keep + kSameTime;
        const bool inLast = last.reach > last.keep + kSameTime && cs >= last.keep - kSameTime && ce <= last.reach + kSameTime;
        if (inFirst || inLast)
            removeClip(c);
    }

    // The neighbours: a quick fade where silence comes in; where takes overlap, the crossfade is
    // made by the overlap (equal power), so a fade of their own goes; an equal-gain crossfade is
    // set as linear fades over the overlap (a hair longer, so it stands).
    const auto quickFade = [crossfade](const juce::ValueTree& c) { return std::min(crossfade, 0.5 * clip::getLength(c)); };
    const double firstOverlap = first.keep - first.reach + 1.0e-9;
    const double lastOverlap = last.reach - last.keep + 1.0e-9;
    if (!midi)
    {
        if (first.neighbour.isValid() && first.neighbour.getParent() == track
            && (first.equalGain || std::abs(clip::getEnd(first.neighbour) - first.neighbourEdge) > kSameTime))
            clip::setFadeOut(first.neighbour, first.fadeNeighbour ? quickFade(first.neighbour) : first.equalGain ? firstOverlap : 0.0, &undo_);
        if (rightPiece.isValid() && (last.equalGain || std::abs(clip::getStart(rightPiece) - last.neighbourEdge) > kSameTime))
            clip::setFadeIn(rightPiece, last.fadeNeighbour ? quickFade(rightPiece) : last.equalGain ? lastOverlap : 0.0, &undo_);
        if (first.touching.isValid() && first.touching.getParent() == track)
            clip::setFadeOut(first.touching, firstOverlap, &undo_);
        if (last.touching.isValid() && last.touching.getParent() == track)
            clip::setFadeIn(last.touching, lastOverlap, &undo_);
    }

    // 4. The take's material between the edits (reaching across them for the crossfades).
    for (const auto& take : takes)
    {
        const double ts = clip::getStart(take), te = clip::getEnd(take);
        const double from = std::max(ts, take == leftTake ? first.reach : start);
        const double to = std::min(te, take == rightTake ? last.reach : end);
        if (to - from < kMinClipLength)
            continue;

        auto copy = plainCopyOf(take);
        reassignIds(copy);
        copy.setProperty(ids::fromTake, static_cast<int>(laneId), nullptr);
        track.addChild(copy, -1, &undo_);

        std::vector<juce::ValueTree> pieces{copy};
        if (clip::isLooped(copy))
            pieces = unloopClip(copy);
        for (auto piece : pieces)
        {
            if (clip::getEnd(piece) <= from + kEpsilon || clip::getStart(piece) >= to - kEpsilon)
            {
                removeClip(piece);
                continue;
            }
            if (clip::getStart(piece) < from - kEpsilon)
                if (auto right = splitClip(piece, from); right.isValid())
                {
                    removeClip(piece);
                    piece = right;
                }
            if (clip::getEnd(piece) > to + kEpsilon)
                if (auto right = splitClip(piece, to); right.isValid())
                    removeClip(right);

            if (midi)
                continue;
            if (std::abs(clip::getStart(piece) - first.reach) <= kSameTime && take == leftTake)
            {
                if (first.keptFade >= 0.0)
                    clip::setFadeIn(piece, first.keptFade, &undo_);
                else if (first.fadeNew)
                    clip::setFadeIn(piece, std::max(clip::getFadeIn(piece), quickFade(piece)), &undo_);
                else if (first.equalGain)
                    clip::setFadeIn(piece, firstOverlap, &undo_);
            }
            if (std::abs(clip::getEnd(piece) - last.reach) <= kSameTime && take == rightTake)
            {
                if (last.keptFade >= 0.0)
                    clip::setFadeOut(piece, last.keptFade, &undo_);
                else if (last.fadeNew)
                    clip::setFadeOut(piece, std::max(clip::getFadeOut(piece), quickFade(piece)), &undo_);
                else if (last.equalGain)
                    clip::setFadeOut(piece, lastOverlap, &undo_);
            }
        }
    }
}

} // namespace dojo::session

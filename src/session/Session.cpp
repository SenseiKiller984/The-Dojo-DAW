#include "session/Session.h"

#include <algorithm>

namespace dojo::session
{
namespace
{
juce::ValueTree createEmptyState()
{
    juce::ValueTree state(ids::session);
    state.setProperty(ids::version, Session::kFileFormatVersion, nullptr);
    state.setProperty(ids::tempo, 120.0, nullptr);
    state.setProperty(ids::masterVolume, 0.0, nullptr);
    state.setProperty(ids::nextId, 1, nullptr);
    return state;
}

std::uint32_t readId(const juce::ValueTree& tree)
{
    return static_cast<std::uint32_t>(static_cast<int>(tree.getProperty(ids::id, 0)));
}
} // namespace

const std::array<juce::Colour, 8>& getTrackPalette()
{
    static const std::array<juce::Colour, 8> palette{
        juce::Colour{0xffe0533d}, // vermilion
        juce::Colour{0xffe58a3d}, // persimmon
        juce::Colour{0xffd6a645}, // gold
        juce::Colour{0xff8fb35a}, // matcha
        juce::Colour{0xff3fb59b}, // jade
        juce::Colour{0xff4f8fd9}, // indigo
        juce::Colour{0xff8a6fd6}, // wisteria
        juce::Colour{0xffd66fa7}, // sakura
    };
    return palette;
}

//==================================================================================================
// Track accessors

namespace track
{
std::uint32_t getId(const juce::ValueTree& t)
{
    return readId(t);
}
juce::String getName(const juce::ValueTree& t)
{
    return t.getProperty(ids::name).toString();
}
juce::Colour getColour(const juce::ValueTree& t)
{
    return juce::Colour::fromString(t.getProperty(ids::colour, "ffe0533d").toString());
}
double getVolumeDb(const juce::ValueTree& t)
{
    return t.getProperty(ids::volume, 0.0);
}
double getPan(const juce::ValueTree& t)
{
    return t.getProperty(ids::pan, 0.0);
}
bool isMuted(const juce::ValueTree& t)
{
    return t.getProperty(ids::muted, false);
}
bool isSoloed(const juce::ValueTree& t)
{
    return t.getProperty(ids::soloed, false);
}
bool isPhaseInverted(const juce::ValueTree& t)
{
    return t.getProperty(ids::phase, false);
}

void setName(juce::ValueTree t, const juce::String& name, juce::UndoManager* um)
{
    t.setProperty(ids::name, name.trim().isEmpty() ? juce::String("Track") : name.trim(), um);
}
void setColour(juce::ValueTree t, juce::Colour colour, juce::UndoManager* um)
{
    t.setProperty(ids::colour, colour.toString(), um);
}
void setVolumeDb(juce::ValueTree t, double db, juce::UndoManager* um)
{
    t.setProperty(ids::volume, juce::jlimit(kMinusInfinityDb, kMaxVolumeDb, db), um);
}
void setPan(juce::ValueTree t, double pan, juce::UndoManager* um)
{
    t.setProperty(ids::pan, juce::jlimit(-1.0, 1.0, pan), um);
}
void setMuted(juce::ValueTree t, bool muted, juce::UndoManager* um)
{
    t.setProperty(ids::muted, muted, um);
}
void setSoloed(juce::ValueTree t, bool soloed, juce::UndoManager* um)
{
    t.setProperty(ids::soloed, soloed, um);
}
void setPhaseInverted(juce::ValueTree t, bool inverted, juce::UndoManager* um)
{
    t.setProperty(ids::phase, inverted, um);
}
} // namespace track

//==================================================================================================
// Clip accessors

namespace clip
{
std::uint32_t getId(const juce::ValueTree& c)
{
    return readId(c);
}
juce::String getName(const juce::ValueTree& c)
{
    return c.getProperty(ids::name).toString();
}
juce::File getFile(const juce::ValueTree& c)
{
    return juce::File(c.getProperty(ids::file).toString());
}
double getStart(const juce::ValueTree& c)
{
    return c.getProperty(ids::start, 0.0);
}
double getLength(const juce::ValueTree& c)
{
    return c.getProperty(ids::length, 0.0);
}
double getEnd(const juce::ValueTree& c)
{
    return getStart(c) + getLength(c);
}
double getOffset(const juce::ValueTree& c)
{
    return c.getProperty(ids::offset, 0.0);
}
double getFadeIn(const juce::ValueTree& c)
{
    return c.getProperty(ids::fadeIn, 0.0);
}
double getFadeOut(const juce::ValueTree& c)
{
    return c.getProperty(ids::fadeOut, 0.0);
}
double getGainDb(const juce::ValueTree& c)
{
    return c.getProperty(ids::gain, 0.0);
}

void setStart(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::start, std::max(0.0, seconds), um);
}
void setLength(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::length, std::max(kMinClipLength, seconds), um);
}
void setOffset(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::offset, std::max(0.0, seconds), um);
}
void setFadeIn(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::fadeIn, juce::jlimit(0.0, std::max(0.0, getLength(c) - getFadeOut(c)), seconds), um);
}
void setFadeOut(juce::ValueTree c, double seconds, juce::UndoManager* um)
{
    c.setProperty(ids::fadeOut, juce::jlimit(0.0, std::max(0.0, getLength(c) - getFadeIn(c)), seconds), um);
}
void setName(juce::ValueTree c, const juce::String& name, juce::UndoManager* um)
{
    c.setProperty(ids::name, name, um);
}
} // namespace clip

//==================================================================================================
// Session

Session::Session()
    : state_(createEmptyState())
{
}

std::uint32_t Session::allocateId()
{
    const int next = state_.getProperty(ids::nextId, 1);
    state_.setProperty(ids::nextId, next + 1, nullptr); // ids are never reused, even across undo
    return static_cast<std::uint32_t>(next);
}

double Session::getTempo() const
{
    return state_.getProperty(ids::tempo, 120.0);
}

void Session::setTempo(double bpm)
{
    state_.setProperty(ids::tempo, juce::jlimit(kMinTempo, kMaxTempo, bpm), &undo_);
}

double Session::getMasterVolumeDb() const
{
    return state_.getProperty(ids::masterVolume, 0.0);
}

void Session::setMasterVolumeDb(double db)
{
    state_.setProperty(ids::masterVolume, juce::jlimit(kMinusInfinityDb, kMaxVolumeDb, db), &undo_);
}

juce::ValueTree Session::findTrack(std::uint32_t trackId) const
{
    for (const auto& t : state_)
        if (track::getId(t) == trackId)
            return t;

    return {};
}

juce::ValueTree Session::addTrack(const juce::String& name, int index)
{
    juce::ValueTree t(ids::track);
    const auto id = allocateId();
    const auto& palette = getTrackPalette();

    t.setProperty(ids::id, static_cast<int>(id), nullptr);
    t.setProperty(ids::name, name.isNotEmpty() ? name : "Audio " + juce::String(getNumTracks() + 1), nullptr);
    t.setProperty(ids::colour, palette[static_cast<std::size_t>(getNumTracks()) % palette.size()].toString(),
                  nullptr);
    t.setProperty(ids::volume, 0.0, nullptr);
    t.setProperty(ids::pan, 0.0, nullptr);
    t.setProperty(ids::muted, false, nullptr);
    t.setProperty(ids::soloed, false, nullptr);
    t.setProperty(ids::phase, false, nullptr);

    state_.addChild(t, index, &undo_);
    return t;
}

void Session::removeTrack(const juce::ValueTree& t)
{
    if (t.isValid() && t.getParent() == state_)
        state_.removeChild(t, &undo_);
}

void Session::moveTrack(int fromIndex, int toIndex)
{
    const int last = getNumTracks() - 1;
    if (fromIndex < 0 || fromIndex > last)
        return;

    toIndex = juce::jlimit(0, last, toIndex);
    if (fromIndex != toIndex)
        state_.moveChild(fromIndex, toIndex, &undo_);
}

juce::ValueTree Session::addClip(juce::ValueTree t, const juce::File& file, double start, double length,
                                 double offset)
{
    if (!t.isValid())
        return {};

    juce::ValueTree c(ids::clip);
    c.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    c.setProperty(ids::name, file.getFileNameWithoutExtension(), nullptr);
    c.setProperty(ids::file, file.getFullPathName(), nullptr);
    c.setProperty(ids::start, std::max(0.0, start), nullptr);
    c.setProperty(ids::length, std::max(kMinClipLength, length), nullptr);
    c.setProperty(ids::offset, std::max(0.0, offset), nullptr);
    c.setProperty(ids::fadeIn, 0.0, nullptr);
    c.setProperty(ids::fadeOut, 0.0, nullptr);
    c.setProperty(ids::gain, 0.0, nullptr);

    t.addChild(c, -1, &undo_);
    return c;
}

juce::ValueTree Session::findClip(std::uint32_t clipId) const
{
    for (const auto& t : state_)
        for (const auto& c : t)
            if (clip::getId(c) == clipId)
                return c;

    return {};
}

void Session::moveClip(juce::ValueTree c, juce::ValueTree targetTrack, double newStart)
{
    if (!c.isValid())
        return;

    clip::setStart(c, newStart, &undo_);

    auto currentTrack = c.getParent();
    if (targetTrack.isValid() && targetTrack != currentTrack && currentTrack.isValid())
    {
        currentTrack.removeChild(c, &undo_);
        targetTrack.addChild(c, -1, &undo_);
    }
}

void Session::removeClip(juce::ValueTree c)
{
    auto parent = c.getParent();
    if (parent.isValid())
        parent.removeChild(c, &undo_);
}

juce::ValueTree Session::duplicateClip(const juce::ValueTree& original)
{
    auto parent = original.getParent();
    if (!parent.isValid())
        return {};

    auto copy = original.createCopy();
    copy.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    copy.setProperty(ids::start, clip::getEnd(original), nullptr);
    parent.addChild(copy, -1, &undo_);
    return copy;
}

juce::ValueTree Session::splitClip(juce::ValueTree c, double atSeconds)
{
    auto parent = c.getParent();
    const double start = clip::getStart(c);
    const double end = clip::getEnd(c);

    if (!parent.isValid() || atSeconds <= start + kMinClipLength || atSeconds >= end - kMinClipLength)
        return {};

    auto right = c.createCopy();
    const double leftLength = atSeconds - start;

    right.setProperty(ids::id, static_cast<int>(allocateId()), nullptr);
    right.setProperty(ids::start, atSeconds, nullptr);
    right.setProperty(ids::length, end - atSeconds, nullptr);
    right.setProperty(ids::offset, clip::getOffset(c) + leftLength, nullptr);
    right.setProperty(ids::fadeIn, 0.0, nullptr);

    c.setProperty(ids::length, leftLength, &undo_);
    c.setProperty(ids::fadeOut, 0.0, &undo_);
    c.setProperty(ids::fadeIn, std::min(clip::getFadeIn(c), leftLength), &undo_);
    right.setProperty(ids::fadeOut, std::min(clip::getFadeOut(right), end - atSeconds), nullptr);

    parent.addChild(right, parent.indexOf(c) + 1, &undo_);
    return right;
}

double Session::getEndTime() const
{
    double end = 0.0;
    for (const auto& t : state_)
        for (const auto& c : t)
            end = std::max(end, clip::getEnd(c));
    return end;
}

void Session::reset()
{
    state_.copyPropertiesAndChildrenFrom(createEmptyState(), nullptr);
    undo_.clearUndoHistory();
}

juce::Result Session::save(const juce::File& file) const
{
    auto copy = state_.createCopy();
    copy.setProperty(ids::version, kFileFormatVersion, nullptr);

    const auto folder = file.getParentDirectory();
    for (auto t : copy)
        for (auto c : t)
            c.setProperty(ids::fileRelative, clip::getFile(c).getRelativePathFrom(folder), nullptr);

    const auto xml = copy.createXml();
    if (xml == nullptr)
        return juce::Result::fail("Could not serialise the session.");

    juce::TemporaryFile temp(file);
    if (!xml->writeTo(temp.getFile()) || !temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not write " + file.getFullPathName());

    return juce::Result::ok();
}

juce::Result Session::load(const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr)
        return juce::Result::fail(file.getFileName() + " is not a readable session file.");

    auto loaded = juce::ValueTree::fromXml(*xml);
    if (!loaded.hasType(ids::session))
        return juce::Result::fail(file.getFileName() + " is not a Dojo session.");

    if (static_cast<int>(loaded.getProperty(ids::version, 0)) > kFileFormatVersion)
        return juce::Result::fail(file.getFileName() + " was saved by a newer version of The Dojo.");

    // Prefer the path relative to the session file, so projects can be moved or copied as a folder.
    const auto folder = file.getParentDirectory();
    for (auto t : loaded)
    {
        for (auto c : t)
        {
            const auto relative = c.getProperty(ids::fileRelative).toString();
            if (relative.isNotEmpty())
            {
                const auto candidate = folder.getChildFile(relative);
                if (candidate.existsAsFile())
                    c.setProperty(ids::file, candidate.getFullPathName(), nullptr);
            }
            c.removeProperty(ids::fileRelative, nullptr);
        }
    }

    state_.copyPropertiesAndChildrenFrom(loaded, nullptr);
    undo_.clearUndoHistory();
    return juce::Result::ok();
}

} // namespace dojo::session

#include "ui/ClipList.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <set>

namespace dojo::ui::cliplist
{
namespace clip = session::clip;
using session::Session;

namespace
{
constexpr double kTolerance = 1.0e-6;

Kind kindOf(const juce::ValueTree& c)
{
    return clip::isMidi(c) ? Kind::midi : clip::isVideo(c) ? Kind::video : Kind::audio;
}

/** Same material: type, file, start and length in it (and name). */
bool sameMaterial(const juce::ValueTree& a, const juce::ValueTree& b, bool compareNames)
{
    return a.getType() == b.getType() && clip::getFile(a) == clip::getFile(b)
           && std::abs(clip::getOffset(a) - clip::getOffset(b)) < kTolerance
           && std::abs(clip::getLength(a) - clip::getLength(b)) < kTolerance
           && (!compareNames || clip::getName(a) == clip::getName(b));
}

bool isOffline(const juce::ValueTree& c)
{
    return !clip::isMidi(c) && !clip::getFile(c).existsAsFile();
}

bool wanted(const Show& show, Kind kind)
{
    switch (kind)
    {
    case Kind::audio: return show.audio;
    case Kind::midi:  return show.midi;
    case Kind::video: return show.video;
    case Kind::group: return show.groups;
    }
    return true;
}

std::vector<juce::ValueTree> trackClips(const Session& session)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& t : session.getAllTracks())
        for (const auto& c : t)
            if (Session::isClip(c))
                clips.push_back(c);
    return clips;
}

/** A sort value: a number or a text (empty text and NaN sort last). */
struct SortValue
{
    double number = std::numeric_limits<double>::quiet_NaN();
    juce::String text;
    bool isText = false;
};

SortValue sortValueOf(const Session& session, const Item& item, SortKey key, FileInfoCache& files)
{
    SortValue v;
    const auto number = [&v](double x) { v.number = x; return v; };
    const auto text = [&v](const juce::String& s) { v.text = s; v.isText = true; return v; };
    const bool isClip = item.clip.isValid();
    const auto file = getFile(session, item);
    const bool hasFile = item.kind == Kind::audio || item.kind == Kind::video || (item.kind == Kind::group && file != juce::File());

    switch (key)
    {
    case SortKey::none:              return v;
    case SortKey::clipType:          return number(static_cast<double>(item.kind));
    case SortKey::name:              return text(getName(session, item));
    case SortKey::length:            return number(getLength(session, item));
    case SortKey::originalTimeStamp: return number(getOriginalTimeStamp(session, item));
    case SortKey::userTimeStamp:     return number(getUserTimeStamp(session, item));
    case SortKey::timebase:          return number(isTickBased(item) ? 1.0 : 0.0);
    case SortKey::startInParent:     return isClip ? number(session.getStartInParent(item.clip)) : v;
    case SortKey::endInParent:       return isClip ? number(session.getEndInParent(item.clip)) : v;
    case SortKey::fileName:          return hasFile ? text(file.getFileName()) : v;
    case SortKey::fileLength:        return hasFile && files.get(file).readable ? number(files.get(file).lengthSeconds) : v;
    case SortKey::fileCreationDate:
        return hasFile && files.get(file).exists ? number(static_cast<double>(files.get(file).created.toMilliseconds())) : v;
    case SortKey::fileModificationDate:
        return hasFile && files.get(file).exists ? number(static_cast<double>(files.get(file).modified.toMilliseconds())) : v;
    case SortKey::diskName:          return hasFile ? text(getDiskName(session, item, files)) : v;
    case SortKey::trackFormat:       return item.kind == Kind::audio ? number(getChannelCount(item, files)) : v;
    case SortKey::channelName:       return item.kind == Kind::audio ? text(getChannelName(item, files)) : v;
    case SortKey::sceneAndTake:
    {
        if (!hasFile)
            return v;
        const auto& info = files.get(file);
        if (info.scene.isEmpty() && info.take.isEmpty())
            return v;
        // (natural order: scene 2 before scene 10, then the take)
        return text(info.scene.paddedLeft('0', 8) + "/" + info.take.paddedLeft('0', 8));
    }
    }
    return v;
}
} // namespace

// --- Options -------------------------------------------------------------------------------------------

juce::String getSortKeyName(SortKey key)
{
    switch (key)
    {
    case SortKey::none:                 return "Order Added";
    case SortKey::clipType:             return "Clip Type";
    case SortKey::name:                 return "Name";
    case SortKey::length:               return "Length";
    case SortKey::originalTimeStamp:    return "Original Time Stamp";
    case SortKey::userTimeStamp:        return "User Time Stamp";
    case SortKey::timebase:             return "Timebase";
    case SortKey::startInParent:        return "Start in Parent";
    case SortKey::endInParent:          return "End in Parent";
    case SortKey::fileName:             return "File Name";
    case SortKey::fileLength:           return "File Length";
    case SortKey::fileCreationDate:     return "File Creation Date";
    case SortKey::fileModificationDate: return "File Modification Date";
    case SortKey::diskName:             return "Disk Name";
    case SortKey::trackFormat:          return "Track Format/Width";
    case SortKey::channelName:          return "Channel Name";
    case SortKey::sceneAndTake:         return "Scene and Take";
    }
    return {};
}

bool appliesToMidi(SortKey key)
{
    switch (key)
    {
    case SortKey::none:
    case SortKey::clipType:
    case SortKey::name:
    case SortKey::length:
    case SortKey::originalTimeStamp:
    case SortKey::userTimeStamp:
    case SortKey::timebase:
    case SortKey::startInParent:
    case SortKey::endInParent:
        return true;
    case SortKey::fileName:
    case SortKey::fileLength:
    case SortKey::fileCreationDate:
    case SortKey::fileModificationDate:
    case SortKey::diskName:
    case SortKey::trackFormat:
    case SortKey::channelName:
    case SortKey::sceneAndTake:
        return false;
    }
    return false;
}

juce::String Options::toString() const
{
    const std::array<bool, 11> flags{show.audio,  show.midi,     show.video,      show.groups,     show.autoCreated, show.colour,
                                     show.fileType, show.timebase, show.processing, show.fileName, show.diskName};
    juce::String text;
    for (const bool f : flags)
        text << (f ? '1' : '0');
    text << ";" << static_cast<int>(sort) << ";" << (ascending ? 1 : 0);
    return text;
}

Options Options::fromString(const juce::String& text)
{
    Options o;
    const auto parts = juce::StringArray::fromTokens(text, ";", "");
    if (parts.size() != 3 || parts[0].length() != 11)
        return o;
    const auto flag = [&parts](int i) { return parts[0][i] == '1'; };
    o.show = {flag(0), flag(1), flag(2), flag(3), flag(4), flag(5), flag(6), flag(7), flag(8), flag(9), flag(10)};
    o.sort = static_cast<SortKey>(juce::jlimit(0, kNumSortKeys - 1, parts[1].getIntValue()));
    o.ascending = parts[2].getIntValue() != 0;
    return o;
}

FileInfoCache::FileInfoCache()
{
    formats_.registerBasicFormats();
}

const session::MediaFileInfo& FileInfoCache::get(const juce::File& file)
{
    const auto key = file.getFullPathName();
    if (auto it = cache_.find(key); it != cache_.end())
        return it->second;
    return cache_.emplace(key, session::readMediaFileInfo(formats_, file)).first->second;
}

// --- Collecting and sorting ---------------------------------------------------------------------------

std::vector<Item> collectItems(const Session& session, const Show& show)
{
    std::vector<Item> items;
    const auto clips = trackClips(session);
    const auto entries = session.getClipListEntries();

    // Clip List entries: whole-file and captured clips.
    for (const auto& e : entries)
    {
        Item item;
        item.clip = e;
        item.kind = kindOf(e);
        item.entry = true;
        item.wholeFile = e.getProperty(session::ids::wholeFile, false);
        item.userDefined = !item.wholeFile;
        item.offline = isOffline(e);
        const auto id = static_cast<int>(clip::getId(e));
        item.used = std::any_of(clips.begin(), clips.end(), [&](const juce::ValueTree& c)
                                { return static_cast<int>(c.getProperty(session::ids::listEntry, 0)) == id || sameMaterial(c, e, false); });
        if (wanted(show, item.kind))
            items.push_back(item);
    }

    // Clips on tracks that are not one of those.
    std::vector<juce::ValueTree> listed;
    for (const auto& c : clips)
    {
        const bool isEntry = std::any_of(entries.begin(), entries.end(), [&](const juce::ValueTree& e) { return sameMaterial(c, e, true); });
        if (isEntry)
            continue;
        if (std::any_of(listed.begin(), listed.end(), [&](const juce::ValueTree& l) { return sameMaterial(c, l, true); }))
            continue; // (the same clip, placed again)

        Item item;
        item.clip = c;
        item.kind = kindOf(c);
        item.userDefined = clip::isUserNamed(c);
        // Made by editing, or (audio and video) a part of a file rather than the whole of it.
        item.autoCreated = !item.userDefined && (c.getProperty(session::ids::autoCreated, false) || item.kind != Kind::midi);
        item.offline = isOffline(c);
        if (!wanted(show, item.kind) || (item.autoCreated && !show.autoCreated))
            continue;
        listed.push_back(c);
        items.push_back(item);
    }

    // Clip groups.
    if (show.groups)
        for (const auto& g : session.getState().getChildWithName(session::ids::clipGroupList))
        {
            const auto id = static_cast<std::uint32_t>(static_cast<int>(g.getProperty(session::ids::id, 0)));
            if (id == 0 || session.getGroupMembers(id).empty())
                continue;
            Item item;
            item.groupId = id;
            item.kind = Kind::group;
            item.userDefined = true;
            items.push_back(item);
        }
    return items;
}

void sortItems(std::vector<Item>& items, const Session& session, SortKey key, bool ascending, FileInfoCache& files)
{
    if (key == SortKey::none)
    {
        if (!ascending)
            std::reverse(items.begin(), items.end());
        return;
    }

    std::vector<std::pair<SortValue, Item>> keyed;
    keyed.reserve(items.size());
    for (auto& item : items)
        keyed.emplace_back(sortValueOf(session, item, key, files), std::move(item));

    std::stable_sort(keyed.begin(), keyed.end(), [ascending](const auto& a, const auto& b)
                     {
                         const auto& x = a.first;
                         const auto& y = b.first;
                         // Rows without a value (MIDI clips for file keys...) go last either way.
                         const bool xMissing = x.isText ? x.text.isEmpty() : std::isnan(x.number);
                         const bool yMissing = y.isText ? y.text.isEmpty() : std::isnan(y.number);
                         if (xMissing != yMissing)
                             return yMissing;
                         if (xMissing)
                             return false;
                         if (x.isText)
                         {
                             const int c = x.text.compareNatural(y.text);
                             return ascending ? c < 0 : c > 0;
                         }
                         return ascending ? x.number < y.number : x.number > y.number;
                     });

    items.clear();
    for (auto& k : keyed)
        items.push_back(std::move(k.second));
}

std::vector<Item> getItems(const Session& session, const Options& options, FileInfoCache& files)
{
    auto items = collectItems(session, options.show);
    sortItems(items, session, options.sort, options.ascending, files);
    return items;
}

// --- Information --------------------------------------------------------------------------------------

juce::String getName(const Session& session, const Item& item)
{
    return item.kind == Kind::group ? session.getGroupName(item.groupId) : clip::getName(item.clip);
}

double getLength(const Session& session, const Item& item)
{
    if (item.kind != Kind::group)
        return clip::getLength(item.clip);
    double start = 1.0e12;
    double end = 0.0;
    for (const auto& c : session.getGroupMembers(item.groupId))
    {
        start = std::min(start, clip::getStart(c));
        end = std::max(end, clip::getEnd(c));
    }
    return std::max(0.0, end - start);
}

juce::String getFileTypeName(const Item& item)
{
    switch (item.kind)
    {
    case Kind::audio: return "Audio";
    case Kind::midi:  return "MIDI";
    case Kind::video: return "Video";
    case Kind::group: return "Group";
    }
    return {};
}

bool isTickBased(const Item& item) { return item.kind == Kind::midi; }

bool isProcessed(const Item& item)
{
    return item.kind == Kind::audio
           && (std::abs(clip::getStretch(item.clip) - 1.0) > kTolerance || std::abs(clip::getPitch(item.clip)) > kTolerance);
}

juce::Colour getColour(const Session& session, const Item& item, juce::Colour fallback)
{
    if (item.kind == Kind::group)
    {
        const auto members = session.getGroupMembers(item.groupId);
        return members.empty() ? fallback : session::track::getColour(members.front().getParent());
    }
    if (!item.entry)
        return session::track::getColour(item.clip.getParent());
    // An entry: the track a copy of it is on.
    for (const auto& c : trackClips(session))
        if (static_cast<int>(c.getProperty(session::ids::listEntry, 0)) == static_cast<int>(clip::getId(item.clip))
            || sameMaterial(c, item.clip, false))
            return session::track::getColour(c.getParent());
    return fallback;
}

juce::File getFile(const Session& session, const Item& item)
{
    if (item.kind == Kind::group)
    {
        for (const auto& c : session.getGroupMembers(item.groupId))
            if (c.hasType(session::ids::clip))
                return clip::getFile(c);
        return {};
    }
    return item.kind == Kind::midi ? juce::File() : clip::getFile(item.clip);
}

juce::String getDiskName(const Session& session, const Item& item, FileInfoCache& files)
{
    const auto file = getFile(session, item);
    if (file == juce::File())
        return {};
    const auto& info = files.get(file);
    return info.exists ? info.diskName : session::getDiskName(file);
}

int getChannelCount(const Item& item, FileInfoCache& files)
{
    if (item.kind != Kind::audio)
        return 0;
    if (clip::getSourceChannel(item.clip) >= 0)
        return 1;
    return files.get(clip::getFile(item.clip)).numChannels;
}

juce::String getChannelName(const Item& item, FileInfoCache& files)
{
    if (item.kind != Kind::audio)
        return {};
    const int channels = files.get(clip::getFile(item.clip)).numChannels;
    if (const int source = clip::getSourceChannel(item.clip); source >= 0)
    {
        if (channels == 2)
            return source == 0 ? "Left" : "Right";
        return "Channel " + juce::String(source + 1);
    }
    switch (channels)
    {
    case 0:  return {};
    case 1:  return "Mono";
    case 2:  return "Stereo";
    default: return juce::String(channels) + " Channels";
    }
}

double getOriginalTimeStamp(const Session& session, const Item& item)
{
    if (item.kind != Kind::group)
        return session.getOriginalTimeStamp(item.clip);
    double earliest = 1.0e12;
    for (const auto& c : session.getGroupMembers(item.groupId))
        earliest = std::min(earliest, session.getOriginalTimeStamp(c));
    return earliest < 1.0e12 ? earliest : 0.0;
}

double getUserTimeStamp(const Session& session, const Item& item)
{
    if (item.kind != Kind::group)
        return session.getUserTimeStamp(item.clip);
    double earliest = 1.0e12;
    for (const auto& c : session.getGroupMembers(item.groupId))
        earliest = std::min(earliest, session.getUserTimeStamp(c));
    return earliest < 1.0e12 ? earliest : 0.0;
}

// --- Select -------------------------------------------------------------------------------------------

std::vector<int> select(const std::vector<Item>& items, Selection selection)
{
    std::vector<int> rows;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        bool pick = false;
        switch (selection)
        {
        case Selection::all:                    pick = true; break;
        case Selection::unused:                 pick = !item.used; break;
        case Selection::unusedExceptWholeFiles: pick = !item.used && !item.wholeFile; break;
        case Selection::offline:                pick = item.offline; break;
        }
        if (pick)
            rows.push_back(static_cast<int>(i));
    }
    return rows;
}

// --- Drag and drop ------------------------------------------------------------------------------------

juce::var describeDrag(const std::vector<Item>& items)
{
    juce::StringArray tokens;
    for (const auto& item : items)
        tokens.add(item.kind == Kind::group ? "g" + juce::String(static_cast<int>(item.groupId))
                                            : "c" + juce::String(static_cast<int>(clip::getId(item.clip))));
    return juce::String(kDragPrefix) + tokens.joinIntoString(",");
}

std::vector<Item> findDragged(const Session& session, const juce::var& description)
{
    const auto text = description.toString();
    if (!text.startsWith(kDragPrefix))
        return {};
    std::vector<Item> items;
    for (const auto& token : juce::StringArray::fromTokens(text.fromFirstOccurrenceOf(kDragPrefix, false, false), ",", ""))
    {
        const auto id = static_cast<std::uint32_t>(token.substring(1).getIntValue());
        Item item;
        if (token.startsWithChar('g'))
        {
            if (session.getGroupMembers(id).empty())
                continue;
            item.groupId = id;
            item.kind = Kind::group;
        }
        else
        {
            item.clip = session.findClipListEntry(id);
            item.entry = item.clip.isValid();
            if (!item.clip.isValid())
                item.clip = session.findClip(id);
            if (!item.clip.isValid())
                continue;
            item.kind = kindOf(item.clip);
        }
        items.push_back(item);
    }
    return items;
}

std::vector<juce::ValueTree> place(Session& session, EditorState& editor, const std::vector<Item>& items, juce::ValueTree track,
                                   double seconds)
{
    if (items.empty())
        return {};
    session.beginTransaction(items.size() == 1 ? "Place Clip" : "Place Clips");
    std::vector<juce::ValueTree> placed;
    double at = std::max(0.0, seconds);
    for (const auto& item : items)
    {
        if (item.kind == Kind::group)
        {
            auto target = track.isValid() ? track : session.addTrack(session::TrackKind::audio, session.getGroupName(item.groupId));
            const auto copies = session.placeGroupCopy(item.groupId, target, at);
            for (const auto& c : copies)
            {
                placed.push_back(c);
                at = std::max(at, clip::getEnd(c));
            }
            continue;
        }
        auto c = session.placeClipCopy(item.clip, track, at);
        if (!c.isValid())
        {
            // A new track of the kind the clip needs.
            const auto kind = item.kind == Kind::midi    ? session::TrackKind::instrument
                              : item.kind == Kind::video ? session::TrackKind::video
                                                         : session::TrackKind::audio;
            c = session.placeClipCopy(item.clip, session.addTrack(kind, clip::getName(item.clip)), at);
        }
        if (c.isValid())
        {
            placed.push_back(c);
            at = clip::getEnd(c);
        }
    }

    std::set<std::uint32_t> ids;
    for (const auto& c : placed)
        ids.insert(clip::getId(c));
    if (!placed.empty())
    {
        editor.selectTrack(session::track::getId(placed.front().getParent()));
        editor.setSelectedClips(std::move(ids));
        editor.clearTimeSelection();
    }
    return placed;
}

// --- Commands -----------------------------------------------------------------------------------------

std::vector<juce::ValueTree> clipsOf(const std::vector<Item>& items)
{
    std::vector<juce::ValueTree> clips;
    for (const auto& item : items)
        if (item.kind != Kind::group && item.clip.isValid())
            clips.push_back(item.clip);
    return clips;
}

bool autoRename(Session& session, const std::vector<juce::ValueTree>& clips, const juce::String& name, int number, int zeros,
                const juce::String& suffix)
{
    if (clips.empty() || name.trim().isEmpty())
        return false;
    session.beginTransaction("Auto Rename");
    session.autoRenameClips(clips, name.trim(), std::max(0, number), juce::jlimit(0, 9, zeros), suffix);
    return true;
}

bool setUserTimeStamps(Session& session, const std::vector<juce::ValueTree>& clips, double seconds)
{
    if (clips.empty())
        return false;
    const double delta = seconds - session.getUserTimeStamp(clips.front());
    session.beginTransaction("Time Stamp");
    for (auto c : clips)
        session.setUserTimeStamp(c, std::max(0.0, session.getUserTimeStamp(c) + delta));
    return true;
}

int removeEntries(Session& session, const std::vector<Item>& items)
{
    int removed = 0;
    for (const auto& item : items)
        if (item.entry && item.clip.isValid())
        {
            if (removed == 0)
                session.beginTransaction(items.size() == 1 ? "Remove Clip from Clip List" : "Remove Clips from Clip List");
            session.removeClipListEntry(clip::getId(item.clip));
            ++removed;
        }
    return removed;
}

int exportClips(const Session& session, const std::vector<Item>& items, const juce::File& folder, juce::String& error)
{
    if (!folder.createDirectory())
    {
        error = "Could not create " + folder.getFullPathName();
        return 0;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::StringArray skipped;
    int written = 0;

    for (const auto& item : items)
    {
        const auto name = juce::File::createLegalFileName(getName(session, item));
        if (item.kind == Kind::audio)
        {
            std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(clip::getFile(item.clip)));
            if (reader == nullptr)
            {
                skipped.add(getName(session, item) + " (its file cannot be read)");
                continue;
            }
            // The clip's part of the file (one loop of a looped clip), its source channel only if it has one.
            const double rate = reader->sampleRate;
            const auto first = static_cast<juce::int64>(std::floor(session.getStartInParent(item.clip) * rate));
            const auto count = static_cast<int>(std::min<juce::int64>(
                reader->lengthInSamples - first,
                static_cast<juce::int64>(std::ceil((session.getEndInParent(item.clip) - session.getStartInParent(item.clip)) * rate))));
            if (count <= 0)
                continue;
            const int sourceChannel = clip::getSourceChannel(item.clip);
            juce::AudioBuffer<float> all(static_cast<int>(reader->numChannels), count);
            reader->read(&all, 0, count, first, true, true);
            juce::AudioBuffer<float> audio;
            if (sourceChannel >= 0 && sourceChannel < all.getNumChannels())
            {
                audio.setSize(1, count);
                audio.copyFrom(0, 0, all, sourceChannel, 0, count);
            }
            else
                audio.makeCopyOf(all);

            const auto target = folder.getNonexistentChildFile(name, ".wav", false);
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(target);
            juce::WavAudioFormat wav;
            auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                          .withSampleRate(rate)
                                                          .withNumChannels(audio.getNumChannels())
                                                          .withBitsPerSample(32)
                                                          .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
            if (writer == nullptr || !writer->writeFromAudioSampleBuffer(audio, 0, count))
            {
                skipped.add(getName(session, item) + " (could not write " + target.getFileName() + ")");
                continue;
            }
            ++written;
        }
        else if (item.kind == Kind::midi)
        {
            // The notes the clip plays, from its start, at the session tempo.
            constexpr int ticksPerBeat = 960;
            const double from = clip::getOffset(item.clip);
            const double to = from + clip::getLength(item.clip) * session.getTempo() / 60.0;
            juce::MidiMessageSequence sequence;
            sequence.addEvent(juce::MidiMessage::tempoMetaEvent(juce::roundToInt(60'000'000.0 / session.getTempo())), 0.0);
            for (const auto& n : item.clip)
            {
                if (!n.hasType(session::ids::note))
                    continue;
                const double start = session::note::getStart(n);
                if (start < from - kTolerance || start >= to)
                    continue;
                const double end = std::min(to, session::note::getEnd(n));
                const int channel = juce::jlimit(1, 16, session::note::getChannel(n));
                sequence.addEvent(juce::MidiMessage::noteOn(channel, session::note::getNote(n), session::note::getVelocity(n)),
                                  (start - from) * ticksPerBeat);
                sequence.addEvent(juce::MidiMessage::noteOff(channel, session::note::getNote(n)), (end - from) * ticksPerBeat);
            }
            sequence.updateMatchedPairs();
            juce::MidiFile midi;
            midi.setTicksPerQuarterNote(ticksPerBeat);
            midi.addTrack(sequence);
            const auto target = folder.getNonexistentChildFile(name, ".mid", false);
            juce::FileOutputStream out(target);
            if (!out.openedOk() || !midi.writeTo(out))
            {
                skipped.add(getName(session, item) + " (could not write " + target.getFileName() + ")");
                continue;
            }
            ++written;
        }
        else
            skipped.add(getName(session, item) + " (" + getFileTypeName(item).toLowerCase() + " clips are not exported)");
    }

    if (!skipped.isEmpty())
        error = "Not exported:\n" + skipped.joinIntoString("\n");
    return written;
}

} // namespace dojo::ui::cliplist

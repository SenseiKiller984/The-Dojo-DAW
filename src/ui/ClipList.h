#pragma once

#include "session/MediaInfo.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <juce_data_structures/juce_data_structures.h>

#include <map>
#include <vector>

/**
    The Clip List's model: which clips it lists (Show options), in what order (Sort By), what Select
    picks, and the commands that work on its rows (place, Auto Rename, Time Stamp, export).

    It lists:
      - whole-file clips: one per imported or recorded file (a clip on a track that plays the whole
        file is that clip, not a row of its own), shown bold
      - captured clips (Clip > Capture)
      - user-defined clips: clips on tracks that were renamed
      - auto-created clips (only with Show > Auto-Created): clips made by editing, i.e. split, copied,
        pasted or unlooped clips, and audio clips that play part of a file
      - the other MIDI clips on tracks (drawn or recorded)
      - clip groups
*/
namespace dojo::ui::cliplist
{

enum class Kind
{
    audio,
    midi,
    video,
    group,
};

struct Item
{
    juce::ValueTree clip;      ///< a Clip List entry or a clip on a track (invalid for a group)
    std::uint32_t groupId = 0; ///< a clip group
    Kind kind = Kind::audio;
    bool entry = false;       ///< kept in the Clip List itself (captured or whole-file)
    bool wholeFile = false;   ///< plays a whole file (bold)
    bool userDefined = false; ///< captured or renamed
    bool autoCreated = false; ///< made by editing
    bool used = true;         ///< placed on a track
    bool offline = false;     ///< its file cannot be found (italic, dimmed)
};

/** Show options: which clips, and which information about them. */
struct Show
{
    bool audio = true;
    bool midi = true;
    bool video = true;
    bool groups = true;
    bool autoCreated = false;

    bool colour = true;     ///< the colour of the track the clip is on
    bool fileType = true;   ///< audio, MIDI or video
    bool timebase = false;  ///< tick-based (MIDI) or sample-based
    bool processing = true; ///< the Elastic Audio mark: time-stretched or pitch-shifted
    bool fileName = true;
    bool diskName = false;
};

enum class SortKey
{
    none, ///< the order in which clips came into the session
    clipType,
    name,
    length,
    originalTimeStamp,
    userTimeStamp,
    timebase,
    startInParent,
    endInParent,
    fileName,
    fileLength,
    fileCreationDate,
    fileModificationDate,
    diskName,
    trackFormat,
    channelName,
    sceneAndTake,
};
inline constexpr int kNumSortKeys = static_cast<int>(SortKey::sceneAndTake) + 1;
[[nodiscard]] juce::String getSortKeyName(SortKey key);
/** Keys that also sort MIDI clips (the others are about files). */
[[nodiscard]] bool appliesToMidi(SortKey key);

struct Options
{
    Show show;
    SortKey sort = SortKey::none;
    bool ascending = true;

    /** Remembered between sessions (as one string in the user settings). */
    [[nodiscard]] juce::String toString() const;
    static Options fromString(const juce::String& text);
};

/** Media file information, read once per file. */
class FileInfoCache
{
public:
    FileInfoCache();
    const session::MediaFileInfo& get(const juce::File& file);
    void clear() { cache_.clear(); }

private:
    juce::AudioFormatManager formats_;
    std::map<juce::String, session::MediaFileInfo> cache_;
};

/** Every row the options show, sorted. */
[[nodiscard]] std::vector<Item> getItems(const session::Session& session, const Options& options, FileInfoCache& files);
/** The rows before sorting (Show options applied). */
[[nodiscard]] std::vector<Item> collectItems(const session::Session& session, const Show& show);
void sortItems(std::vector<Item>& items, const session::Session& session, SortKey key, bool ascending, FileInfoCache& files);

// --- Information shown in the columns --------------------------------------------------------------
[[nodiscard]] juce::String getName(const session::Session& session, const Item& item);
[[nodiscard]] double getLength(const session::Session& session, const Item& item);
[[nodiscard]] juce::String getFileTypeName(const Item& item); ///< "Audio", "MIDI", "Video" or "Group"
[[nodiscard]] bool isTickBased(const Item& item);
/** Elastic Audio: an audio clip that is time-stretched or pitch-shifted. */
[[nodiscard]] bool isProcessed(const Item& item);
/** The colour of the track the clip (or, for an entry, a copy of it) is on; `fallback` if none. */
[[nodiscard]] juce::Colour getColour(const session::Session& session, const Item& item, juce::Colour fallback);
[[nodiscard]] juce::File getFile(const session::Session& session, const Item& item);
[[nodiscard]] juce::String getDiskName(const session::Session& session, const Item& item, FileInfoCache& files);
[[nodiscard]] int getChannelCount(const Item& item, FileInfoCache& files);
[[nodiscard]] juce::String getChannelName(const Item& item, FileInfoCache& files);
[[nodiscard]] double getOriginalTimeStamp(const session::Session& session, const Item& item);
[[nodiscard]] double getUserTimeStamp(const session::Session& session, const Item& item);

// --- Select ------------------------------------------------------------------------------------------
enum class Selection
{
    all,
    unused,                 ///< not placed on any track
    unusedExceptWholeFiles, ///< ... leaving whole-file clips out
    offline,                ///< their files cannot be found
};
/** Row indices of `items` that `selection` picks. */
[[nodiscard]] std::vector<int> select(const std::vector<Item>& items, Selection selection);

// --- Drag and drop -----------------------------------------------------------------------------------
inline constexpr const char* kDragPrefix = "DojoClipList:";
[[nodiscard]] juce::var describeDrag(const std::vector<Item>& items);
/** The rows a drag description names (clip ids and group ids), or none. */
[[nodiscard]] std::vector<Item> findDragged(const session::Session& session, const juce::var& description);
/** Places copies of `items` on `track` one after the other from `seconds` (a clip that the track cannot
    hold goes on a new track of the right kind; groups keep their layout). Selects them. One undo
    step. Returns the clips made. */
std::vector<juce::ValueTree> place(session::Session& session, EditorState& editor, const std::vector<Item>& items,
                                   juce::ValueTree track, double seconds);

// --- Commands -----------------------------------------------------------------------------------------
/** The clips of `items` (groups and their clips left out) that commands such as Rename work on. */
[[nodiscard]] std::vector<juce::ValueTree> clipsOf(const std::vector<Item>& items);

/** Auto Rename: "<name><number, zero-padded to `zeros` digits><suffix>" in order, counting up; the
    clips become user-defined and clips later made from them by editing continue the numbering. One
    undo step. False if there is no clip or no name. */
bool autoRename(session::Session& session, const std::vector<juce::ValueTree>& clips, const juce::String& name, int number,
                int zeros, const juce::String& suffix);

/** Time Stamp: the first clip's user time stamp becomes `seconds`; the others move by as much (they
    keep their distance from it). One undo step. */
bool setUserTimeStamps(session::Session& session, const std::vector<juce::ValueTree>& clips, double seconds);

/** Removes Clip List entries (captured and whole-file clips) from the list; clips on tracks stay. One
    undo step. Returns how many were removed. */
int removeEntries(session::Session& session, const std::vector<Item>& items);

/** Export Clips as Files: each audio clip's material as a 32-bit float WAV, each MIDI clip as a
    Standard MIDI File, into `folder`. Returns how many were written; `error` says what was not. */
int exportClips(const session::Session& session, const std::vector<Item>& items, const juce::File& folder, juce::String& error);

} // namespace dojo::ui::cliplist

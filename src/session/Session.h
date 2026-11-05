#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

#include <array>
#include <cstdint>

namespace dojo::session
{

/**
    Property and type identifiers of the session document.

        DOJO_SESSION  version, tempo, masterVolumeDb, nextId
          TRACK       id, name, colour, volumeDb, pan, muted, soloed, phaseInverted
            CLIP      id, name, file, start, length, offset, fadeIn, fadeOut, gainDb

    All times are in seconds on the timeline; `offset` is where in the source file the clip begins.
*/
namespace ids
{
// clang-format off
inline const juce::Identifier session      {"DOJO_SESSION"};
inline const juce::Identifier track        {"TRACK"};
inline const juce::Identifier clip         {"CLIP"};

inline const juce::Identifier version      {"version"};
inline const juce::Identifier tempo        {"tempo"};
inline const juce::Identifier masterVolume {"masterVolumeDb"};
inline const juce::Identifier nextId       {"nextId"};

inline const juce::Identifier id           {"id"};
inline const juce::Identifier name         {"name"};
inline const juce::Identifier colour       {"colour"};
inline const juce::Identifier volume       {"volumeDb"};
inline const juce::Identifier pan          {"pan"};
inline const juce::Identifier muted        {"muted"};
inline const juce::Identifier soloed       {"soloed"};
inline const juce::Identifier phase        {"phaseInverted"};

inline const juce::Identifier file         {"file"};
inline const juce::Identifier fileRelative {"fileRelative"};
inline const juce::Identifier start        {"start"};
inline const juce::Identifier length       {"length"};
inline const juce::Identifier offset       {"offset"};
inline const juce::Identifier fadeIn       {"fadeIn"};
inline const juce::Identifier fadeOut      {"fadeOut"};
inline const juce::Identifier gain         {"gainDb"};
// clang-format on
} // namespace ids

/** Volume at or below this is treated as -infinity (silence). */
inline constexpr double kMinusInfinityDb = -60.0;
inline constexpr double kMaxVolumeDb = 6.0;
inline constexpr double kMinTempo = 20.0;
inline constexpr double kMaxTempo = 400.0;
inline constexpr double kMinClipLength = 0.005;

/** Default colours offered for tracks (cycled as tracks are created). */
const std::array<juce::Colour, 8>& getTrackPalette();

/** Typed accessors for a TRACK tree. Setters take an UndoManager (nullptr = not undoable). */
namespace track
{
std::uint32_t getId(const juce::ValueTree& t);
juce::String getName(const juce::ValueTree& t);
juce::Colour getColour(const juce::ValueTree& t);
double getVolumeDb(const juce::ValueTree& t);
double getPan(const juce::ValueTree& t);
bool isMuted(const juce::ValueTree& t);
bool isSoloed(const juce::ValueTree& t);
bool isPhaseInverted(const juce::ValueTree& t);

void setName(juce::ValueTree t, const juce::String& name, juce::UndoManager* um);
void setColour(juce::ValueTree t, juce::Colour colour, juce::UndoManager* um);
void setVolumeDb(juce::ValueTree t, double db, juce::UndoManager* um);
void setPan(juce::ValueTree t, double pan, juce::UndoManager* um);
void setMuted(juce::ValueTree t, bool muted, juce::UndoManager* um);
void setSoloed(juce::ValueTree t, bool soloed, juce::UndoManager* um);
void setPhaseInverted(juce::ValueTree t, bool inverted, juce::UndoManager* um);
} // namespace track

/** Typed accessors for a CLIP tree. */
namespace clip
{
std::uint32_t getId(const juce::ValueTree& c);
juce::String getName(const juce::ValueTree& c);
juce::File getFile(const juce::ValueTree& c);
double getStart(const juce::ValueTree& c);
double getLength(const juce::ValueTree& c);
double getEnd(const juce::ValueTree& c);
double getOffset(const juce::ValueTree& c);
double getFadeIn(const juce::ValueTree& c);
double getFadeOut(const juce::ValueTree& c);
double getGainDb(const juce::ValueTree& c);

void setStart(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setLength(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setOffset(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setFadeIn(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setFadeOut(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setName(juce::ValueTree c, const juce::String& name, juce::UndoManager* um);
} // namespace clip

/**
    The document: a juce::ValueTree plus its UndoManager.

    Every structural edit goes through this class and is recorded for undo. UI gestures bracket their
    edits with beginTransaction(), so one drag or one menu action is one undo step. Components observe
    the tree with juce::ValueTree::Listener; the GraphCompiler turns it into what the engine renders.
    Message thread only.
*/
class Session final
{
public:
    Session();

    [[nodiscard]] juce::ValueTree& getState() noexcept { return state_; }
    [[nodiscard]] const juce::ValueTree& getState() const noexcept { return state_; }
    [[nodiscard]] juce::UndoManager& getUndoManager() noexcept { return undo_; }

    /** Starts a new undo step named `name` (shown in the Edit menu). */
    void beginTransaction(const juce::String& name) { undo_.beginNewTransaction(name); }

    // --- Global ----------------------------------------------------------------------------------
    [[nodiscard]] double getTempo() const;
    void setTempo(double bpm);
    [[nodiscard]] double getMasterVolumeDb() const;
    void setMasterVolumeDb(double db);

    // --- Tracks ----------------------------------------------------------------------------------
    [[nodiscard]] int getNumTracks() const { return state_.getNumChildren(); }
    [[nodiscard]] juce::ValueTree getTrack(int index) const { return state_.getChild(index); }
    [[nodiscard]] juce::ValueTree findTrack(std::uint32_t trackId) const;
    [[nodiscard]] int indexOf(const juce::ValueTree& track) const { return state_.indexOf(track); }

    /** Adds an audio track at `index` (-1 = at the end) with an automatic name and colour. */
    juce::ValueTree addTrack(const juce::String& name = {}, int index = -1);
    void removeTrack(const juce::ValueTree& track);
    void moveTrack(int fromIndex, int toIndex);

    // --- Clips -----------------------------------------------------------------------------------
    juce::ValueTree addClip(juce::ValueTree track, const juce::File& file, double start, double length,
                            double offset = 0.0);
    [[nodiscard]] juce::ValueTree findClip(std::uint32_t clipId) const;
    [[nodiscard]] static juce::ValueTree getTrackOf(const juce::ValueTree& clip) { return clip.getParent(); }

    /** Moves a clip in time and (optionally) to another track. */
    void moveClip(juce::ValueTree clip, juce::ValueTree targetTrack, double newStart);
    void removeClip(juce::ValueTree clip);

    /** Copies a clip and places the copy directly after the original. Returns the copy. */
    juce::ValueTree duplicateClip(const juce::ValueTree& clip);

    /** Splits a clip at timeline time `atSeconds`. Returns the new right-hand part, or an invalid
        tree if the split point is not inside the clip. */
    juce::ValueTree splitClip(juce::ValueTree clip, double atSeconds);

    /** End time of the last clip (0 for an empty session). */
    [[nodiscard]] double getEndTime() const;

    // --- Document --------------------------------------------------------------------------------
    /** Replaces everything with an empty session and clears the undo history. */
    void reset();

    /** Writes the session as XML. Clip files are stored both absolute and relative to the file. */
    juce::Result save(const juce::File& file) const;

    /** Loads a session file, replacing the current one and clearing the undo history. */
    juce::Result load(const juce::File& file);

    static constexpr int kFileFormatVersion = 1;

private:
    std::uint32_t allocateId();

    juce::ValueTree state_;
    juce::UndoManager undo_{30000, 300};
};

} // namespace dojo::session

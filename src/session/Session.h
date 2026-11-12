#pragma once

#include "engine/RenderGraph.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_graphics/juce_graphics.h>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace dojo::session
{

using engine::TrackKind;

/**
    Property and type identifiers of the session document (file format version 2).

        DOJO_SESSION  version, tempo, masterVolumeDb, masterFormat, nextId, loopEnabled, loopStart, loopEnd,
                      speed, keepPitch, transpose
          MASTER        (the master bus: devices and automation)
            DEVICE ...  AUTOMATION ...
          TRACK       id, name, colour, kind (audio|instrument|bus), folder, collapsed,
                      volumeDb, pan, muted, soloed, phaseInverted, output, input, inputStereo,
                      monitor, armed, speed, pitch, auxSource, auxChannel, showAutomation,
                      format (mono|stereo|3.1|5.1|7.1|dolbyDigital|dts), panFrontRear, panCentre
            CLIP        id, name, file, start, length, offset, fadeIn, fadeOut, gainDb, gainBypassed, stretch,
                        pitch, sourceChannel, editLocked, timeLocked, rating (1..5), groups (clip group ids,
                        outermost first), looped, loopLength, syncPoint (seconds from the start), userNamed
            VIDEOCLIP   id, name, file, start, length, offset (seconds into the file), editLocked, timeLocked
                        (and rating, groups, looped, loopLength, syncPoint, userNamed)
            MIDICLIP    id, name, start, length, offset (beats) (and rating ... userNamed as above)
              NOTE        start, length (beats, relative to the clip content), note, velocity, channel
              CC          controller (0..127, 128 = pitch bend), beat, value (0..1), channel
            DEVICE      id, type, name, bypassed, state (base64), sidechain (track id), description
            SEND        id, target (bus track id), gainDb, preFader
            AUTOMATION  id, target ("volume", "pan", "send:<id>", "param:<deviceId>:<index>"), visible
              POINT       time (seconds), value (0..1)
            TRACK ...   (folders contain tracks)
          CLIPGROUPS    the clip groups: CLIPGROUP id, name
          CLIPLIST      captured clips (Clip > Capture): CLIP / MIDICLIP / VIDEOCLIP, start 0

    Timeline times are in seconds; MIDI content is in beats (quarter notes). A folder is a bus track
    whose child tracks are summed into it.
*/
namespace ids
{
// clang-format off
inline const juce::Identifier session      {"DOJO_SESSION"};
inline const juce::Identifier master       {"MASTER"};
inline const juce::Identifier track        {"TRACK"};
inline const juce::Identifier clip         {"CLIP"};
inline const juce::Identifier midiClip     {"MIDICLIP"};
inline const juce::Identifier videoClip    {"VIDEOCLIP"};
inline const juce::Identifier note         {"NOTE"};
inline const juce::Identifier control      {"CC"};
inline const juce::Identifier device       {"DEVICE"};
inline const juce::Identifier send         {"SEND"};
inline const juce::Identifier automation   {"AUTOMATION"};
inline const juce::Identifier point        {"POINT"};

inline const juce::Identifier version      {"version"};
inline const juce::Identifier tempo        {"tempo"};
inline const juce::Identifier masterVolume {"masterVolumeDb"};
inline const juce::Identifier nextId       {"nextId"};
inline const juce::Identifier loopEnabled  {"loopEnabled"};
inline const juce::Identifier punchIn      {"punchIn"};
inline const juce::Identifier punchOut     {"punchOut"};
inline const juce::Identifier inputOnly    {"inputOnlyMonitoring"};
inline const juce::Identifier loopStart    {"loopStart"};
inline const juce::Identifier loopEnd      {"loopEnd"};
inline const juce::Identifier speed        {"speed"};
inline const juce::Identifier keepPitch    {"keepPitch"};
inline const juce::Identifier transpose    {"transpose"};

inline const juce::Identifier id           {"id"};
inline const juce::Identifier name         {"name"};
inline const juce::Identifier colour       {"colour"};
inline const juce::Identifier kind         {"kind"};
inline const juce::Identifier folder       {"folder"};
inline const juce::Identifier collapsed    {"collapsed"};
inline const juce::Identifier volume       {"volumeDb"};
inline const juce::Identifier pan          {"pan"};
inline const juce::Identifier muted        {"muted"};
inline const juce::Identifier soloed       {"soloed"};
inline const juce::Identifier phase        {"phaseInverted"};
inline const juce::Identifier output       {"output"};
inline const juce::Identifier input        {"input"};
inline const juce::Identifier inputStereo  {"inputStereo"};
inline const juce::Identifier monitor      {"monitor"};
inline const juce::Identifier armed        {"armed"};
inline const juce::Identifier pitch        {"pitch"};
inline const juce::Identifier auxSource    {"auxSource"};
inline const juce::Identifier auxChannel   {"auxChannel"};
inline const juce::Identifier showAutomation {"showAutomation"};
inline const juce::Identifier format {"format"};
inline const juce::Identifier active {"active"};
inline const juce::Identifier sourceChannel {"sourceChannel"};
inline const juce::Identifier editLocked {"editLocked"};
inline const juce::Identifier masterFormat {"masterFormat"};
inline const juce::Identifier panFrontRear {"panFrontRear"};
inline const juce::Identifier panCentre {"panCentre"};
inline const juce::Identifier timeLocked {"timeLocked"};
inline const juce::Identifier rating {"rating"};
inline const juce::Identifier clipGroups {"groups"};
inline const juce::Identifier looped {"looped"};
inline const juce::Identifier loopLength {"loopLength"};
inline const juce::Identifier syncPoint {"syncPoint"};
inline const juce::Identifier gainBypassed {"gainBypassed"};
inline const juce::Identifier userNamed {"userNamed"};
inline const juce::Identifier clipGroupList {"CLIPGROUPS"};
inline const juce::Identifier clipGroup {"CLIPGROUP"};
inline const juce::Identifier clipList {"CLIPLIST"};
inline const juce::Identifier midiTarget {"midiTarget"};
inline const juce::Identifier vca {"vca"};
inline const juce::Identifier controls {"controls"};

inline const juce::Identifier file         {"file"};
inline const juce::Identifier fileRelative {"fileRelative"};
inline const juce::Identifier start        {"start"};
inline const juce::Identifier length       {"length"};
inline const juce::Identifier offset       {"offset"};
inline const juce::Identifier fadeIn       {"fadeIn"};
inline const juce::Identifier fadeOut      {"fadeOut"};
inline const juce::Identifier gain         {"gainDb"};
inline const juce::Identifier stretch      {"stretch"};

inline const juce::Identifier noteNumber   {"note"};
inline const juce::Identifier velocity     {"velocity"};
inline const juce::Identifier channel      {"channel"};
inline const juce::Identifier controller   {"controller"};
inline const juce::Identifier beat         {"beat"};
inline const juce::Identifier value        {"value"};

inline const juce::Identifier type         {"type"};
inline const juce::Identifier bypassed     {"bypassed"};
inline const juce::Identifier state        {"state"};
inline const juce::Identifier sidechain    {"sidechain"};
inline const juce::Identifier description  {"description"};

inline const juce::Identifier target       {"target"};
inline const juce::Identifier preFader     {"preFader"};
inline const juce::Identifier visible      {"visible"};
inline const juce::Identifier time         {"time"};
// clang-format on
} // namespace ids

/** Volume at or below this is treated as -infinity (silence). */
inline constexpr double kMinusInfinityDb = -60.0;
inline constexpr double kMaxVolumeDb = 6.0;
inline constexpr double kMinTempo = 20.0;
inline constexpr double kMaxTempo = 400.0;
inline constexpr double kMinClipLength = 0.005;
inline constexpr double kMinStretch = 0.25;
inline constexpr double kMaxStretch = 4.0;
inline constexpr double kMinNoteLength = 1.0 / 64.0; ///< beats

/** Default colours offered for tracks (cycled as tracks are created). */
const std::array<juce::Colour, 8>& getTrackPalette();

[[nodiscard]] juce::String toString(TrackKind kind);
[[nodiscard]] TrackKind trackKindFromString(const juce::String& text);
/** Tracks of this kind hold MIDI clips (instrument and MIDI tracks). */
[[nodiscard]] bool holdsMidi(TrackKind kind);
/** Tracks of this kind hold audio clips. */
[[nodiscard]] bool holdsAudio(TrackKind kind);
/** Tracks of this kind hold video clips (video tracks). */
[[nodiscard]] bool holdsVideo(TrackKind kind);
/** Tracks of this kind make sound of their own (and have a place in the render graph). */
[[nodiscard]] bool isRendered(TrackKind kind);

/**
    A track's channel format (audio tracks and busses; instrument tracks are stereo), and the
    master's. The multichannel formats carry one channel per speaker, in this order:
        3.1            L R C LFE
        5.1            L R C LFE Ls Rs   (SMPTE/ITU)
        7.1            L R C LFE Ls Rs Lrs Rrs
        Dolby Digital  L C R Ls Rs LFE   (5.1 in AC-3 order)
        DTS            L R Ls Rs C LFE   (5.1 in DTS order)
*/
enum class TrackFormat
{
    mono,
    stereo,
    lcrLfe,
    surround51,
    surround71,
    dolbyDigital,
    dts,
};
[[nodiscard]] juce::String toString(TrackFormat format);
[[nodiscard]] TrackFormat trackFormatFromString(const juce::String& text);
/** "Mono", "Stereo", "3.1", "5.1", "7.1", "Dolby Digital 5.1", "DTS 5.1". */
[[nodiscard]] juce::String getDisplayName(TrackFormat format);
[[nodiscard]] int getNumChannels(TrackFormat format);
[[nodiscard]] inline bool isMultichannel(TrackFormat format) { return getNumChannels(format) > 2; }
/** Every format, in menu order. */
[[nodiscard]] std::vector<TrackFormat> getAllTrackFormats();
/** The speakers of a format's channels (mono: the centre). */
[[nodiscard]] engine::ChannelLayout getLayout(TrackFormat format);
/**
    Which channel of an audio file (with `fileChannels` channels) a multichannel track's channel
    `trackChannel` plays, or -1: the same channel when the file has as many (recorded on such a
    track), otherwise by speaker (assuming the SMPTE order for 1, 2, 4, 6 and 8 channels; a mono
    file plays from the centre).
*/
[[nodiscard]] int fileChannelFor(int fileChannels, const engine::ChannelLayout& track, int trackChannel);

/** Typed accessors for a TRACK tree. Setters take an UndoManager (nullptr = not undoable). */
namespace track
{
std::uint32_t getId(const juce::ValueTree& t);
juce::String getName(const juce::ValueTree& t);
juce::Colour getColour(const juce::ValueTree& t);
TrackKind getKind(const juce::ValueTree& t);
bool isFolder(const juce::ValueTree& t);
bool isCollapsed(const juce::ValueTree& t);
double getVolumeDb(const juce::ValueTree& t);
double getPan(const juce::ValueTree& t);
bool isMuted(const juce::ValueTree& t);
bool isSoloed(const juce::ValueTree& t);
bool isPhaseInverted(const juce::ValueTree& t);
std::uint32_t getOutput(const juce::ValueTree& t); ///< bus track id; 0 = parent folder / master
int getInput(const juce::ValueTree& t);            ///< first input channel, -1 = none
bool isInputStereo(const juce::ValueTree& t);
engine::MonitorMode getMonitor(const juce::ValueTree& t);
bool isArmed(const juce::ValueTree& t);
double getSpeed(const juce::ValueTree& t);
double getPitch(const juce::ValueTree& t);
std::uint32_t getAuxSource(const juce::ValueTree& t);
int getAuxChannel(const juce::ValueTree& t);
bool isShowingAutomation(const juce::ValueTree& t);
/** Mono tracks play their clips (or one chosen channel of them) and record one input channel;
    multichannel tracks record as many consecutive input channels as they have speakers. */
TrackFormat getFormat(const juce::ValueTree& t);
/** Surround panner, used when a track feeds a multichannel bus or master: front (-1) to rear (+1). */
double getPanFrontRear(const juce::ValueTree& t);
/** Surround panner: how much of the centred front signal plays from the centre speaker (0..1). */
double getPanCentre(const juce::ValueTree& t);
/** Inactive tracks are not played or processed at all (their devices use no CPU). */
bool isActive(const juce::ValueTree& t);
/** MIDI tracks: the instrument track their MIDI goes to (0 = none). */
std::uint32_t getMidiTarget(const juce::ValueTree& t);
/** The VCA Master this track belongs to (0 = none). */
std::uint32_t getVca(const juce::ValueTree& t);
/** Master Faders: the bus they control, 0 = the main output. */
std::uint32_t getControlledBus(const juce::ValueTree& t);

void setName(juce::ValueTree t, const juce::String& name, juce::UndoManager* um);
void setColour(juce::ValueTree t, juce::Colour colour, juce::UndoManager* um);
void setCollapsed(juce::ValueTree t, bool collapsed, juce::UndoManager* um);
void setVolumeDb(juce::ValueTree t, double db, juce::UndoManager* um);
void setPan(juce::ValueTree t, double pan, juce::UndoManager* um);
void setMuted(juce::ValueTree t, bool muted, juce::UndoManager* um);
void setSoloed(juce::ValueTree t, bool soloed, juce::UndoManager* um);
void setPhaseInverted(juce::ValueTree t, bool inverted, juce::UndoManager* um);
void setOutput(juce::ValueTree t, std::uint32_t busId, juce::UndoManager* um);
void setInput(juce::ValueTree t, int firstChannel, bool stereo, juce::UndoManager* um);
void setMonitor(juce::ValueTree t, engine::MonitorMode mode, juce::UndoManager* um);
void setArmed(juce::ValueTree t, bool armed, juce::UndoManager* um);
void setSpeed(juce::ValueTree t, double speed, juce::UndoManager* um);
void setPitch(juce::ValueTree t, double semitones, juce::UndoManager* um);
void setAuxSource(juce::ValueTree t, std::uint32_t instrumentTrackId, int firstChannel, juce::UndoManager* um);
void setShowingAutomation(juce::ValueTree t, bool show, juce::UndoManager* um);
void setFormat(juce::ValueTree t, TrackFormat format, juce::UndoManager* um);
void setPanFrontRear(juce::ValueTree t, double frontRear, juce::UndoManager* um);
void setPanCentre(juce::ValueTree t, double centre, juce::UndoManager* um);
void setActive(juce::ValueTree t, bool active, juce::UndoManager* um);
void setMidiTarget(juce::ValueTree t, std::uint32_t instrumentTrackId, juce::UndoManager* um);
void setVca(juce::ValueTree t, std::uint32_t vcaTrackId, juce::UndoManager* um);
void setControlledBus(juce::ValueTree t, std::uint32_t busId, juce::UndoManager* um);
} // namespace track

/** Typed accessors for a CLIP (audio) or MIDICLIP tree; the timing ones work on both. */
namespace clip
{
std::uint32_t getId(const juce::ValueTree& c);
bool isMidi(const juce::ValueTree& c);
bool isVideo(const juce::ValueTree& c);
juce::String getName(const juce::ValueTree& c);
juce::File getFile(const juce::ValueTree& c);
double getStart(const juce::ValueTree& c);
double getLength(const juce::ValueTree& c);
double getEnd(const juce::ValueTree& c);
double getOffset(const juce::ValueTree& c); ///< seconds of source (audio) or beats of content (MIDI)
double getFadeIn(const juce::ValueTree& c);
double getFadeOut(const juce::ValueTree& c);
double getGainDb(const juce::ValueTree& c);
double getStretch(const juce::ValueTree& c); ///< timeline seconds per source second
double getPitch(const juce::ValueTree& c);   ///< semitones
/** The source file channel an audio clip plays (on both sides), or -1 for all of them. */
int getSourceChannel(const juce::ValueTree& c);

/** Edit Lock: the clip cannot be edited (trimmed, split, faded, its content changed), moved or
    deleted. Copies of it are edit-locked too. */
bool isEditLocked(const juce::ValueTree& c);
/** Time Lock: the clip stays at its time location; it cannot be moved in time or deleted, but it can
    be trimmed, split (the parts stay time-locked) or moved to another track at the same time. */
bool isTimeLocked(const juce::ValueTree& c);
/** What the locks allow. */
inline bool canEdit(const juce::ValueTree& c) { return !isEditLocked(c); }
inline bool canMoveInTime(const juce::ValueTree& c) { return !isEditLocked(c) && !isTimeLocked(c); }
inline bool canChangeTrack(const juce::ValueTree& c) { return !isEditLocked(c); }
inline bool canRemove(const juce::ValueTree& c) { return !isEditLocked(c) && !isTimeLocked(c); }

/** Rating: 0 = none (the default), 1..5 (5 the best). For choosing between takes. */
int getRating(const juce::ValueTree& c);
void setRating(juce::ValueTree c, int rating, juce::UndoManager* um);

/** The clip groups the clip belongs to, outermost (front-most) first; empty if none. */
std::vector<std::uint32_t> getGroups(const juce::ValueTree& c);
/** The outermost group, or 0. */
std::uint32_t getOuterGroup(const juce::ValueTree& c);
void setGroups(juce::ValueTree c, const std::vector<std::uint32_t>& groups, juce::UndoManager* um);
inline bool isGrouped(const juce::ValueTree& c) { return getOuterGroup(c) != 0; }

/** A looped clip repeats its first `getLoopLength()` seconds for its whole length. */
bool isLooped(const juce::ValueTree& c);
double getLoopLength(const juce::ValueTree& c); ///< one iteration (timeline seconds); the length if not looped
void setLoop(juce::ValueTree c, bool looped, double iterationLength, juce::UndoManager* um);

/** The sync point: a position in the clip (seconds from its start) used instead of the start for
    snapping and Quantize to Grid. Absent: none. */
std::optional<double> getSyncPoint(const juce::ValueTree& c);
void setSyncPoint(juce::ValueTree c, std::optional<double> secondsFromStart, juce::UndoManager* um);
/** Where the clip lines up: its sync point, or its start. */
double getReferenceTime(const juce::ValueTree& c);

/** Bypassed clip gain: the clip plays at 0 dB, the setting is kept. */
bool isGainBypassed(const juce::ValueTree& c);
void setGainBypassed(juce::ValueTree c, bool bypassed, juce::UndoManager* um);

/** Renamed by the user (shown in the Clip List as a user-defined clip). */
bool isUserNamed(const juce::ValueTree& c);

void setStart(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setLength(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setOffset(juce::ValueTree c, double value, juce::UndoManager* um);
void setFadeIn(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setFadeOut(juce::ValueTree c, double seconds, juce::UndoManager* um);
void setGainDb(juce::ValueTree c, double db, juce::UndoManager* um);
void setStretch(juce::ValueTree c, double ratio, juce::UndoManager* um);
void setPitch(juce::ValueTree c, double semitones, juce::UndoManager* um);
void setName(juce::ValueTree c, const juce::String& name, juce::UndoManager* um);
void setSourceChannel(juce::ValueTree c, int channel, juce::UndoManager* um);
void setEditLocked(juce::ValueTree c, bool locked, juce::UndoManager* um);
void setTimeLocked(juce::ValueTree c, bool locked, juce::UndoManager* um);
} // namespace clip

/** NOTE accessors (times in beats relative to the clip's content). */
namespace note
{
double getStart(const juce::ValueTree& n);
double getLength(const juce::ValueTree& n);
double getEnd(const juce::ValueTree& n);
int getNote(const juce::ValueTree& n);
float getVelocity(const juce::ValueTree& n);
int getChannel(const juce::ValueTree& n);

void setStart(juce::ValueTree n, double beat, juce::UndoManager* um);
void setLength(juce::ValueTree n, double beats, juce::UndoManager* um);
void setNote(juce::ValueTree n, int noteNumber, juce::UndoManager* um);
void setVelocity(juce::ValueTree n, float velocity, juce::UndoManager* um);
} // namespace note

/** DEVICE accessors. */
namespace device
{
std::uint32_t getId(const juce::ValueTree& d);
juce::String getType(const juce::ValueTree& d);
juce::String getName(const juce::ValueTree& d);
bool isBypassed(const juce::ValueTree& d);
std::uint32_t getSidechain(const juce::ValueTree& d);
void setBypassed(juce::ValueTree d, bool bypassed, juce::UndoManager* um);
void setSidechain(juce::ValueTree d, std::uint32_t trackId, juce::UndoManager* um);
} // namespace device

/** SEND accessors. */
namespace send
{
std::uint32_t getId(const juce::ValueTree& s);
std::uint32_t getTarget(const juce::ValueTree& s);
double getGainDb(const juce::ValueTree& s);
bool isPreFader(const juce::ValueTree& s);
void setGainDb(juce::ValueTree s, double db, juce::UndoManager* um);
void setPreFader(juce::ValueTree s, bool preFader, juce::UndoManager* um);
} // namespace send

/** AUTOMATION accessors. Targets: "volume", "pan", "send:<sendId>", "param:<deviceId>:<index>". */
namespace automation
{
juce::String getTarget(const juce::ValueTree& lane);
bool isVisible(const juce::ValueTree& lane);
void setVisible(juce::ValueTree lane, bool visible, juce::UndoManager* um);

juce::String volumeTarget();
juce::String panTarget();
juce::String sendTarget(std::uint32_t sendId);
juce::String parameterTarget(std::uint32_t deviceId, int parameterIndex);

/** Parses "param:<deviceId>:<index>"; returns false for other targets. */
bool parseParameterTarget(const juce::String& target, std::uint32_t& deviceId, int& parameterIndex);
/** Parses "send:<sendId>". */
bool parseSendTarget(const juce::String& target, std::uint32_t& sendId);

/** Points of a lane as an engine curve (sorted by time). */
engine::AutomationCurve toCurve(const juce::ValueTree& lane);
} // namespace automation

/**
    The document: a juce::ValueTree plus its UndoManager.

    Every structural edit goes through this class and is recorded for undo. UI gestures bracket their
    edits with beginTransaction(), so one drag or one menu action is one undo step. Components observe
    the tree with juce::ValueTree::Listener; the GraphCompiler turns it into what the engine renders.

    Tracks form a tree (folders contain tracks). "Track index" always means the position in the
    depth-first order in which the arranger lists them (including children of collapsed folders).
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
    /** The master's channel format (stereo, or a multichannel format for surround mixes; never mono).
        Undoable. */
    [[nodiscard]] TrackFormat getMasterFormat() const;
    void setMasterFormat(TrackFormat format);

    [[nodiscard]] bool isLoopEnabled() const;
    [[nodiscard]] double getLoopStart() const;
    [[nodiscard]] double getLoopEnd() const;
    void setLoop(bool enabled, double startSeconds, double endSeconds);
    void setLoopEnabled(bool enabled);

    /** Punch recording: with punch-in on, recording starts at the loop start; with punch-out on, it
        ends at the loop end. Playback (and monitoring) before and after is the track as it was. */
    [[nodiscard]] bool isPunchInEnabled() const;
    [[nodiscard]] bool isPunchOutEnabled() const;
    void setPunchInEnabled(bool enabled);
    void setPunchOutEnabled(bool enabled);
    /** The part of the timeline recording keeps: [loop start or 0, loop end or no end). */
    [[nodiscard]] std::pair<double, double> getPunchWindow() const;

    /** How record-enabled tracks (with automatic monitoring) are monitored: Input Only (always the
        input) or, when false, Auto Input (the input while stopped and while punched in; the track's
        clips otherwise). Saved with the session. */
    [[nodiscard]] bool isInputOnlyMonitoring() const;
    void setInputOnlyMonitoring(bool inputOnly);

    /** Global playback speed (varispeed, 0.25..4). With keepPitch, audio keeps its pitch. */
    [[nodiscard]] double getSpeed() const;
    [[nodiscard]] bool getKeepPitch() const;
    void setSpeed(double speed);
    void setKeepPitch(bool keepPitch);
    /** Global transposition in semitones (audio clips and MIDI notes). */
    [[nodiscard]] double getTranspose() const;
    void setTranspose(double semitones);

    /** The MASTER node (devices and automation of the master bus). */
    [[nodiscard]] juce::ValueTree getMaster();
    [[nodiscard]] juce::ValueTree getMaster() const { return state_.getChildWithName(ids::master); }

    // --- Tracks ----------------------------------------------------------------------------------
    [[nodiscard]] std::vector<juce::ValueTree> getAllTracks() const;
    [[nodiscard]] int getNumTracks() const;
    [[nodiscard]] juce::ValueTree getTrack(int index) const;
    [[nodiscard]] int indexOf(const juce::ValueTree& track) const;
    [[nodiscard]] juce::ValueTree findTrack(std::uint32_t trackId) const;

    /** The folder containing `track`, or an invalid tree for top-level tracks. */
    [[nodiscard]] static juce::ValueTree getParentFolder(const juce::ValueTree& track);
    /** Nesting depth (0 = top level). */
    [[nodiscard]] static int getDepth(const juce::ValueTree& track);
    /** True if `track` is `ancestor` or lies anywhere inside it. */
    [[nodiscard]] static bool isWithin(const juce::ValueTree& track, const juce::ValueTree& ancestor);
    /** False if `track` lies inside a collapsed folder. */
    [[nodiscard]] static bool isVisibleInArranger(const juce::ValueTree& track);
    /** False if `track` or a folder around it is inactive. */
    [[nodiscard]] static bool isEffectivelyActive(const juce::ValueTree& track);

    /** Adds an audio track at track index `index` (-1 = at the end of the top level). */
    juce::ValueTree addTrack(const juce::String& name = {}, int index = -1);
    /** Adds a track of any kind. `index` as above: the track goes in front of the track currently at
        that index, inside the same folder. */
    juce::ValueTree addTrack(TrackKind kind, const juce::String& name, int index = -1);
    /** Adds an empty folder (a bus that sums the tracks inside it). */
    juce::ValueTree addFolder(const juce::String& name = {}, int index = -1);
    /** `tracks` in arranger order, without those inside another of them (a folder brings its
        contents along) and without any that are not in the session. */
    [[nodiscard]] std::vector<juce::ValueTree> outermostInOrder(const std::vector<juce::ValueTree>& tracks) const;
    /** Wraps `tracks` in a new folder, placed where the first of them (in arranger order) is. Tracks
        from other folders move into it too; their order is kept. Invalid if there is nothing to group. */
    juce::ValueTree groupIntoFolder(const std::vector<juce::ValueTree>& tracks, const juce::String& name = {});

    /** What a duplicate takes along; the rest of the track (mix, routing, format, ...) always comes. */
    struct DuplicateOptions
    {
        bool clips = true;      ///< audio and MIDI clips
        bool automation = true; ///< automation lanes (those of devices or sends left behind are dropped)
        bool devices = true;    ///< the device chain, with the state stored in the session
        bool sends = true;
        bool vcaAssignment = true; ///< stays a member of its VCA; otherwise the VCA's gain is baked in
    };
    /** Copies `track` (a folder with everything inside it) with fresh ids and puts the copy right after
        `after` (after its contents too), in after's folder. References between the copied tracks
        (outputs, sends, sidechains) point at the copies. The copy is not armed. `name` empty: the
        source's name + ".dup1", ".dup2", ... (the first one not taken). */
    juce::ValueTree duplicateTrack(const juce::ValueTree& track, const DuplicateOptions& options,
                                   const juce::ValueTree& after, const juce::String& name = {});
    /** A track name not used yet: `base`, else `base` + `separator` + 2, 3, ... (or 1, 2, ... with
        startAtOne). */
    [[nodiscard]] juce::String uniqueTrackName(const juce::String& base, const juce::String& separator = " ", bool startAtOne = false) const;

    /** Removes a track (and, for a folder, everything inside it). */
    void removeTrack(const juce::ValueTree& track);

    /** Takes `member` out of `vca` without changing how it sounds: the VCA's fader and volume
        automation are added (in dB) to the member's own (its automation, or its fader when neither
        has automation); a muted VCA mutes it. The member moves to the VCA's own VCA, if it has one. */
    void coalesceVca(juce::ValueTree member, const juce::ValueTree& vca);
    /** The tracks whose VCA is `vca`. */
    [[nodiscard]] std::vector<juce::ValueTree> getVcaMembers(const juce::ValueTree& vca) const;
    /** Moves the track at index `fromIndex` (with its contents) to sit in front of the track at
        `toIndex`, inside that track's folder (or to the end of the top level for toIndex >= count). */
    void moveTrack(int fromIndex, int toIndex);
    /** Moves several tracks (each with its contents) in front of the track at `toIndex`, inside that
        track's folder, keeping their order (toIndex >= count: to the end of the top level). Dropping
        them onto themselves changes nothing. */
    void moveTracks(const std::vector<juce::ValueTree>& tracks, int toIndex);
    /** Moves `track` (with its contents) to right after `after` (after its contents too), in after's folder. */
    bool moveAfter(const juce::ValueTree& track, const juce::ValueTree& after);
    /** Moves a track into `folder` (at its end), or to the top level if `folder` is invalid. */
    bool moveIntoFolder(const juce::ValueTree& track, const juce::ValueTree& folder);

    /** Every bus track (group busses, return busses and folders). */
    [[nodiscard]] std::vector<juce::ValueTree> getBusses() const;

    /** Where `track`'s output goes: the bus it routes to, its folder, or invalid for the master. */
    [[nodiscard]] juce::ValueTree getDestination(const juce::ValueTree& track) const;
    /** True if `source` may feed `bus` (output, send or sidechain) without creating a feedback loop. */
    [[nodiscard]] bool canFeed(const juce::ValueTree& source, const juce::ValueTree& bus) const;

    // --- Clips (audio and MIDI) ------------------------------------------------------------------
    juce::ValueTree addClip(juce::ValueTree track, const juce::File& file, double start, double length,
                            double offset = 0.0);
    juce::ValueTree addMidiClip(juce::ValueTree track, double start, double length, const juce::String& name = {});
    [[nodiscard]] juce::ValueTree findClip(std::uint32_t clipId) const;
    [[nodiscard]] static juce::ValueTree getTrackOf(const juce::ValueTree& clip) { return clip.getParent(); }
    [[nodiscard]] static bool isClip(const juce::ValueTree& tree)
    {
        return tree.hasType(ids::clip) || tree.hasType(ids::midiClip) || tree.hasType(ids::videoClip);
    }
    /** A video clip on a video track: `length` seconds of `file` from `offset` seconds in. */
    juce::ValueTree addVideoClip(juce::ValueTree track, const juce::File& file, double start, double length, double offset = 0.0);

    /** Moves a clip in time and (optionally) to another track of a compatible kind. An edit-locked
        clip does not move; a time-locked one only changes track (it keeps its start). */
    void moveClip(juce::ValueTree clip, juce::ValueTree targetTrack, double newStart);
    /** Removes a clip, unless it is edit- or time-locked. Returns true if it was removed. */
    bool removeClip(juce::ValueTree clip);

    /** Copies a clip and places the copy directly after the original. Returns the copy. */
    juce::ValueTree duplicateClip(const juce::ValueTree& clip);

    /** Inserts a copy of `clip` (from anywhere, e.g. the clipboard) on `track` at `start`. */
    juce::ValueTree pasteClip(const juce::ValueTree& clip, juce::ValueTree track, double start);

    /** Splits a clip at timeline time `atSeconds`. Returns the new right-hand part, or an invalid
        tree if the split point is not inside the clip or the clip is edit-locked. The parts of a
        time-locked clip are time-locked too. */
    juce::ValueTree splitClip(juce::ValueTree clip, double atSeconds);

    /** End time of the last clip (0 for an empty session). */
    [[nodiscard]] double getEndTime() const;

    // --- Clip groups, loops, sync points -----------------------------------------------------------
    /** Every clip (on any track) of group `groupId`, at any depth. */
    [[nodiscard]] std::vector<juce::ValueTree> getGroupMembers(std::uint32_t groupId) const;
    /** `clips` plus every clip of their outermost groups (what selecting them really selects). */
    [[nodiscard]] std::vector<juce::ValueTree> withWholeGroups(const std::vector<juce::ValueTree>& clips) const;
    /** Makes a new clip group of `clips` (each joins it as its outermost group). Returns its id. */
    std::uint32_t groupClips(const std::vector<juce::ValueTree>& clips, const juce::String& name = {});
    /** Removes group `groupId` where it is the outermost layer; the clips (and nested groups) inside
        are free again. Returns them. */
    std::vector<juce::ValueTree> ungroup(std::uint32_t groupId);
    /** Removes every layer of grouping from the clips of `groupId`'s outermost group. */
    std::vector<juce::ValueTree> ungroupAll(std::uint32_t groupId);
    /** Puts `clips` back into group `groupId` (named `name`) as their outermost layer. */
    void regroup(std::uint32_t groupId, const std::vector<juce::ValueTree>& clips, const juce::String& name);
    [[nodiscard]] juce::String getGroupName(std::uint32_t groupId) const;
    /** Gives the copies in `copies` fresh group ids (consistently), so they form groups of their own. */
    void renewGroups(const std::vector<juce::ValueTree>& copies);

    /** Loops a clip: `iterations` repeats of its current length (or of its loop if looped already). */
    void loopClip(juce::ValueTree clip, double iterations);
    /** Unloops a clip into one standard clip per iteration (the last one partial). Returns them, the
        original first. */
    std::vector<juce::ValueTree> unloopClip(juce::ValueTree clip);

    /** Sync point at timeline time `seconds` (inside the clip). False if outside. */
    bool identifySyncPoint(juce::ValueTree clip, double seconds);

    // --- Clip List: captured clips -------------------------------------------------------------------
    /** The captured clips (clip trees with start 0, in the order captured). */
    [[nodiscard]] std::vector<juce::ValueTree> getClipListEntries() const;
    [[nodiscard]] juce::ValueTree findClipListEntry(std::uint32_t entryId) const;
    /**
        Clip > Capture: the material on `track` between `start` and `end` becomes a new Clip List entry
        called `name`. It must be one clip, or clips that play contiguous material of the same file one
        after the other (no gaps, same stretch and pitch). Returns the entry, or an invalid tree with
        `error` saying why.
    */
    juce::ValueTree captureClip(const juce::ValueTree& track, double start, double end, const juce::String& name,
                                juce::String& error);
    /** Removes a captured clip from the Clip List. */
    void removeClipListEntry(std::uint32_t entryId);
    /** A copy of `source` (a Clip List entry or a clip) placed on `track` at `seconds`, with new ids and
        no groups, locks or sync point carried over beyond what a fresh clip has. Invalid if the
        track cannot hold that kind of clip. */
    juce::ValueTree placeClipCopy(const juce::ValueTree& source, juce::ValueTree track, double seconds);

    /** Removes everything on `track` between `start` and `end` (seconds): clips inside go, clips across
        the edges are trimmed (or split, when they span the whole range). For recording over. Locked
        clips are left as they are. */
    void clearRange(juce::ValueTree track, double start, double end);

    // --- MIDI --------------------------------------------------------------------------------------
    juce::ValueTree addNote(juce::ValueTree midiClip, double startBeat, double lengthBeats, int noteNumber,
                            float velocity = 0.8f, int channel = 1);
    void removeNote(juce::ValueTree note);
    juce::ValueTree addControl(juce::ValueTree midiClip, int controller, double beat, float value, int channel = 1);
    void removeControl(juce::ValueTree control);
    /** Quantises the notes of `notes` (or of the whole clip if empty) to `gridBeats` with `strength`
        (0..1); with `ends`, note ends are quantised too. */
    void quantise(juce::ValueTree midiClip, const std::vector<juce::ValueTree>& notes, double gridBeats,
                  double strength, bool ends);
    /** The note covering `beat` at `noteNumber`, if any. */
    [[nodiscard]] static juce::ValueTree findNoteAt(const juce::ValueTree& midiClip, double beat, int noteNumber);

    // --- Devices -----------------------------------------------------------------------------------
    /** Adds a device to a track (or the MASTER node) at chain position `index` (-1 = end). */
    juce::ValueTree addDevice(juce::ValueTree owner, const juce::String& type, const juce::String& name,
                              int index = -1);
    void removeDevice(juce::ValueTree device);
    void moveDevice(juce::ValueTree device, int newIndex);
    [[nodiscard]] juce::ValueTree findDevice(std::uint32_t deviceId) const;
    [[nodiscard]] static std::vector<juce::ValueTree> getDevices(const juce::ValueTree& owner);

    // --- Sends -------------------------------------------------------------------------------------
    juce::ValueTree addSend(juce::ValueTree track, const juce::ValueTree& bus, double gainDb = -6.0,
                            bool preFader = false);
    void removeSend(juce::ValueTree send);
    [[nodiscard]] static std::vector<juce::ValueTree> getSends(const juce::ValueTree& track);

    // --- Automation --------------------------------------------------------------------------------
    /** The lane for `target` on `owner` (a track or MASTER), created (with one point holding
        `initialValue`) if `create` is set. */
    juce::ValueTree getAutomation(juce::ValueTree owner, const juce::String& target, bool create,
                                  double initialValue = 0.5);
    [[nodiscard]] static std::vector<juce::ValueTree> getAutomationLanes(const juce::ValueTree& owner);
    juce::ValueTree addAutomationPoint(juce::ValueTree lane, double time, double value);
    void moveAutomationPoint(juce::ValueTree point, double time, double value);
    void removeAutomationPoint(juce::ValueTree point);
    void removeAutomation(juce::ValueTree lane);

    // --- Document --------------------------------------------------------------------------------
    /** Replaces everything with an empty session and clears the undo history. */
    void reset();

    /** Writes the session as XML. Clip files are stored both absolute and relative to the file. */
    juce::Result save(const juce::File& file) const;

    /** Loads a session file, replacing the current one and clearing the undo history. */
    juce::Result load(const juce::File& file);

    /** Every audio file referenced by audio clips. */
    [[nodiscard]] std::vector<juce::File> getReferencedFiles() const;

    static constexpr int kFileFormatVersion = 2;

private:
    std::uint32_t allocateId();
    void reassignIds(juce::ValueTree tree);
    juce::ValueTree insertTrackTree(juce::ValueTree track, int index);

    juce::ValueTree state_;
    juce::UndoManager undo_{30000, 300};
};

} // namespace dojo::session

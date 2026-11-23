#pragma once

#include "session/AudioAssetCache.h"
#include "session/Session.h"
#include "session/audioedit/PitchRender.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

namespace dojo::session
{

/**
    Native audio editing of clips: pitch editing (the clip editor's PITCH view) - and the renders that
    make it heard.

    A clip being edited keeps its own file as the AUDIOEDIT's `source`; analysing it reads the part
    of the source the clip plays (plus 0.2 s either side: `from`, `to`) and writes what was found
    (PITCH: one PNOTE per note) as one undo step. Every edit after that (a PNOTE's new pitch, drift,
    gain or timing; the formants switch) is undoable and starts a render in the background: the
    edited spans are resynthesised (audioedit::pitch, cached span by span), written with the rest of
    the source into a new 32-bit float WAV in the render folder, loaded, and then the clip plays it
    (its `file`, set without undo: undoing an edit renders again). Bypass plays the source (A/B).
    Removing the edit (undoable) plays the source again.

    Message thread only (the work runs on two background threads).
*/
class AudioEdits final : public juce::ChangeBroadcaster,
                         private juce::ValueTree::Listener,
                         private juce::Timer
{
public:
    AudioEdits(Session& session, AudioAssetCache& assets);
    ~AudioEdits() override;

    enum class Status
    {
        idle,
        analysing,
        rendering,
        failed,
    };
    struct State
    {
        Status status = Status::idle;
        float progress = 0.0f; ///< of the analysis, 0..1
        juce::String problem;  ///< why it failed
    };
    [[nodiscard]] State getState(const juce::ValueTree& clip) const;

    /** Analyses the notes of an audio clip in the background (replacing an earlier analysis). False
        (and why) if it cannot start. */
    bool analysePitch(const juce::ValueTree& clip, audioedit::pitch::Mode mode, juce::String& error);
    /** The clip plays its source again, without its pitch edits (undoable). */
    void removePitch(juce::ValueTree clip);
    /** A/B: the clip plays its source while bypassed (not undoable; kept in the session). */
    void setBypassed(juce::ValueTree clip, bool bypassed);
    [[nodiscard]] static bool isBypassed(const juce::ValueTree& clip);

    /** Analyses and renders running or waiting. */
    [[nodiscard]] bool isBusy() const;
    /** Pumps the message loop until nothing is running or waiting (e.g. before an export); false on
        timeout. */
    bool waitUntilIdle(int timeoutMs);

    /** Where renders are written. */
    void setRenderFolder(const juce::File& folder) { renderFolder_ = folder; }
    [[nodiscard]] const juce::File& getRenderFolder() const noexcept { return renderFolder_; }
    /** After a session is opened: clips whose render is missing play their source and render again. */
    void sessionLoaded();
    /** Stops everything (a new session). */
    void reset();

private:
    struct Material;
    struct Entry
    {
        Status status = Status::idle;
        juce::String problem;
        std::shared_ptr<std::atomic<float>> progress = std::make_shared<std::atomic<float>>(0.0f);
        std::shared_ptr<std::atomic<bool>> cancel;
        int generation = 0;
        bool dirty = false;     ///< edited since the last render started
        bool rendering = false;
        std::shared_ptr<Material> material;
    };

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeRedirected(juce::ValueTree&) override { reset(); }
    void timerCallback() override;

    void markDirty(const juce::ValueTree& clip);
    void startRender(const juce::ValueTree& clip, Entry& entry);
    void analysisDone(std::uint32_t clipId, int generation, std::shared_ptr<Material> material, std::vector<audioedit::pitch::Note> notes,
                      audioedit::pitch::Mode mode, juce::String error);
    void renderDone(std::uint32_t clipId, int generation, juce::File target, juce::String error);
    void play(const juce::ValueTree& clip, const juce::File& file);
    void deleteIfUnused(const juce::File& file);
    [[nodiscard]] static juce::ValueTree clipOf(juce::ValueTree tree);

    Session& session_;
    AudioAssetCache& assets_;
    juce::File renderFolder_;
    std::map<std::uint32_t, Entry> entries_;
    juce::ThreadPool pool_{juce::ThreadPoolOptions{}.withThreadName("Dojo audio edits").withNumberOfThreads(2)};
    int pendingLoads_ = 0;

    JUCE_DECLARE_WEAK_REFERENCEABLE(AudioEdits)
    JUCE_DECLARE_NON_COPYABLE(AudioEdits)
};

/** The edits of a clip in the session tree (see AudioEdits). */
namespace audioedits
{
[[nodiscard]] juce::ValueTree getEdit(const juce::ValueTree& clip);
[[nodiscard]] juce::ValueTree getPitch(const juce::ValueTree& clip);
[[nodiscard]] bool hasPitch(const juce::ValueTree& clip);
/** The clip's own file: its source while edited, else its file. */
[[nodiscard]] juce::File getSource(const juce::ValueTree& clip);
/** The second of the source that note times count from. */
[[nodiscard]] double getFrom(const juce::ValueTree& clip);
[[nodiscard]] audioedit::pitch::Mode getPitchMode(const juce::ValueTree& clip);
[[nodiscard]] bool keepsFormants(const juce::ValueTree& clip);
void setKeepsFormants(juce::ValueTree clip, bool keep, juce::UndoManager* um);

[[nodiscard]] std::vector<juce::ValueTree> getPitchNotes(const juce::ValueTree& clip);
[[nodiscard]] audioedit::pitch::Edit toEdit(const juce::ValueTree& note);
[[nodiscard]] juce::ValueTree makeNote(const audioedit::pitch::Note& note);
/** Seconds of the clip (from its start) where a note time (from `from`) plays, and back. */
[[nodiscard]] double clipTimeOf(const juce::ValueTree& clip, double noteTime);
[[nodiscard]] double noteTimeOf(const juce::ValueTree& clip, double clipTime);

void setNotePitch(juce::ValueTree note, double midi, juce::UndoManager* um);
void setNoteDrift(juce::ValueTree note, double drift, juce::UndoManager* um);
void setNoteGain(juce::ValueTree note, double gainDb, juce::UndoManager* um);
/** New start and end (seconds from `from`), melodic material: kept 20 ms long and inside its
    neighbours; a neighbour it touched (as analysed) is stretched or squeezed to keep touching. */
void setNoteTiming(juce::ValueTree note, double newStart, double newEnd, juce::UndoManager* um);
/** Back to how it was analysed. */
void resetNote(juce::ValueTree note, juce::UndoManager* um);

enum class ScaleKind
{
    chromatic,
    major,
    naturalMinor,
    harmonicMinor,
    majorPentatonic,
    minorPentatonic,
};
struct Scale
{
    int root = 0; ///< pitch class, 0 = C
    ScaleKind kind = ScaleKind::chromatic;
};
/** The nearest pitch of the scale (a whole MIDI note). */
[[nodiscard]] int nearestInScale(double midi, const Scale& scale);
/** Pitch correction: each note's centre moved `pitchAmount` (0..1) of the way to the nearest note of
    the scale, its drift reduced by `driftAmount` (0..1). */
void correctPitch(const std::vector<juce::ValueTree>& notes, double pitchAmount, double driftAmount, const Scale& scale, juce::UndoManager* um);
} // namespace audioedits

} // namespace dojo::session

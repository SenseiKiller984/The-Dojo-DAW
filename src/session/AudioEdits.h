#pragma once

#include "session/AudioAssetCache.h"
#include "session/Session.h"
#include "session/audioedit/PitchRender.h"
#include "session/audioedit/Spectral.h"

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
    Native audio editing of clips: pitch editing (the clip editor's PITCH view) and spectral editing
    (SPECTRAL) - and the renders that make them heard.

    A clip being edited keeps its own file as the AUDIOEDIT's `source`; analysing it reads the part
    of the source the clip plays (plus 0.2 s either side: `from`, `to`) and writes what was found
    (PITCH: one PNOTE per note) as one undo step. Every edit after that (a PNOTE's new pitch, drift,
    gain or timing; the formants switch) is undoable and starts a render in the background: the
    edited spans are resynthesised (audioedit::pitch, cached span by span), written with the rest of
    the source into a new 32-bit float WAV in the render folder, loaded, and then the clip plays it
    (its `file`, set without undo: undoing an edit renders again). Bypass plays the source (A/B).
    Removing the edit (undoable) plays the source again.

    Spectral operations (SPECTRAL: one SPOP each, at seconds of the source) are applied first; pitch
    analysis and pitch edits work on their result. The spectrogram shown is of that result too.

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
    /** The clip loses its pitch edits (undoable); without spectral edits it plays its source again. */
    void removePitch(juce::ValueTree clip);
    /** A spectral operation removed (undoable); the last edit of the clip gone, it plays its source again. */
    void removeSpectralOperation(juce::ValueTree op);
    /** A/B: the clip plays its source while bypassed (not undoable; kept in the session). */
    void setBypassed(juce::ValueTree clip, bool bypassed);
    [[nodiscard]] static bool isBypassed(const juce::ValueTree& clip);

    // --- Spectral editing --------------------------------------------------------------------------------
    /** Adds a spectral operation (its times in seconds of the clip), creating the clip's edit if it has
        none; one undo step named `undoName`. Returns the SPOP. */
    juce::ValueTree addSpectralOperation(juce::ValueTree clip, audioedit::spectral::Operation op, const juce::String& undoName);
    /** The noise profile of a stretch of the clip (seconds of the clip), read from its source now. */
    [[nodiscard]] std::vector<float> learnNoise(const juce::ValueTree& clip, double clipStart, double clipEnd, juce::String& error);
    /** The spectrogram of the clip's source with its spectral edits, made in the background: the last
        one made (possibly older, or null) until the current one is ready (a change message then).
        Its time 0 is `getSpectrogramStart()` seconds into the source. */
    [[nodiscard]] std::shared_ptr<const audioedit::spectral::Spectrogram> getSpectrogram(const juce::ValueTree& clip);
    [[nodiscard]] double getSpectrogramStart(const juce::ValueTree& clip) const;

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
    struct Region
    {
        juce::File source;
        double from = 0.0;
        double to = 0.0;
    };
    /** The part of the source an edit covers (the clip's played part and 0.2 s either side). */
    [[nodiscard]] bool regionOf(const juce::ValueTree& clip, Region& region, juce::String& error) const;
    [[nodiscard]] std::shared_ptr<Material> materialFor(std::uint32_t clipId, const Region& region);
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
        std::shared_ptr<const audioedit::spectral::Spectrogram> spectrogram;
        std::uint64_t spectrogramKey = 0;   ///< what it shows
        std::uint64_t spectrogramWanted = 0; ///< being made
        double spectrogramStart = 0.0;
    };

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeRedirected(juce::ValueTree&) override { reset(); }
    void timerCallback() override;

    void markDirty(const juce::ValueTree& clip);
    void removePart(juce::ValueTree clip, juce::ValueTree part);
    void startRender(const juce::ValueTree& clip, Entry& entry);
    void analysisDone(std::uint32_t clipId, int generation, std::shared_ptr<Material> material, std::vector<audioedit::pitch::Note> notes,
                      audioedit::pitch::Mode mode, juce::String error);
    [[nodiscard]] static std::vector<audioedit::spectral::Operation> spectralOperationsOf(const juce::ValueTree& clip);
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
    int pendingSpectrograms_ = 0;

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

[[nodiscard]] juce::ValueTree getSpectral(const juce::ValueTree& clip);
[[nodiscard]] std::vector<juce::ValueTree> getSpectralOperations(const juce::ValueTree& clip);
/** A SPOP as an operation, its times from `from` (seconds of the source the processed audio starts at). */
[[nodiscard]] audioedit::spectral::Operation toOperation(const juce::ValueTree& op, double from);
/** A SPOP of an operation whose times are seconds of the source. */
[[nodiscard]] juce::ValueTree makeOperation(const audioedit::spectral::Operation& op);
[[nodiscard]] juce::String describe(const juce::ValueTree& op);
[[nodiscard]] juce::String kindName(audioedit::spectral::Kind kind);
void setOperationEnabled(juce::ValueTree op, bool enabled, juce::UndoManager* um);
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

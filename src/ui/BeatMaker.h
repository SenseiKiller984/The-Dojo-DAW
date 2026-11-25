#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace dojo::time
{
class TempoMap;
}
namespace dojo::session
{
class Session;
}

namespace dojo::ui
{

/**
    Beat Maker Mode (Preferences > General): the computer keyboard plays a Drum Rack like a beat
    maker's pads.

    Keys A to P play the 16 pads in order: A the first pad (MIDI note 36: C1 as The Dojo names it,
    C2 in scientific pitch names and some DAWs), B the next, and so on to P. A key plays at velocity 100, with
    Shift at 127 (an accent); its release ends the note. The notes go to the selected track (the
    track of the selected MIDI clip when one is selected): its Drum Rack, or whatever instrument
    it has, through its MIDI effects. Holding a key does not retrigger it.

    Record with a MIDI clip selected records what is played into that clip (MainComponent: the
    recorder stays out of it, armed or not; a take there would replace the clip): each note
    where the playhead was when its key went down (what was heard: delay compensation and the
    output latency taken off), as long as it was held, at its velocity. A looped clip takes the
    notes into its loop; outside the clip nothing is recorded. The notes of one recording pass are
    one undo step ("Record Beats").

    Other keys, and A to P with Ctrl, Alt or Cmd, pass on (shortcuts); a text field with the focus
    keeps its letters. A key released while another component has the focus, or the window losing
    it, is noticed by polling, so no note is left hanging.

    Message thread only.
*/
class BeatMaker final : public juce::KeyListener, private juce::Timer
{
public:
    static constexpr int kPads = 16;
    static constexpr int kFirstNote = 36;  ///< the Drum Rack's first pad
    static constexpr int kVelocity = 100;
    static constexpr int kAccentVelocity = 127;
    static constexpr const char* kUndoName = "Record Beats";

    struct Host
    {
        std::function<bool()> isEnabled;
        /** The track the pads play (0: none). */
        std::function<std::uint32_t()> getTargetTrack;
        /** The MIDI clip to record into: the selected one while recording, else an invalid tree. */
        std::function<juce::ValueTree()> getRecordingClip;
        /** Where the playhead is, as heard (seconds), while the transport runs; nothing when stopped. */
        std::function<std::optional<double>()> getPlayheadSeconds;
        /** Plays a message on a track (live, not recorded by the recorder). */
        std::function<void(std::uint32_t trackId, const juce::MidiMessage&)> sendMidi;
        /** Says something to the user (the status bar), e.g. why a note was not recorded. */
        std::function<void(const juce::String&)> onMessage;
        /** Whether a key is down (unset: the keyboard's state, juce::KeyPress::isKeyCurrentlyDown). */
        std::function<bool(int keyCode)> isKeyDown;
    };

    BeatMaker(session::Session& session, Host host);
    ~BeatMaker() override;

    /** The pad (0-15) a key plays: A to P without Ctrl, Alt or Cmd; -1 for any other. */
    [[nodiscard]] static int padForKey(const juce::KeyPress& key) noexcept;
    /** The key that plays a pad ("A" to "P"). */
    [[nodiscard]] static juce::String keyForPad(int pad);

    /**
        Where a timeline time falls in a MIDI clip's content, in beats (as the engine plays it: the
        clip's offset plus the beats since the start of the clip, or of its loop iteration, `rate`
        times as fast). Nothing when the time is outside the clip.
    */
    [[nodiscard]] static std::optional<double> contentBeatAt(const juce::ValueTree& midiClip, const time::TempoMap& map, double seconds,
                                                             double rate = 1.0);

    // --- What keys do (also for tests) -------------------------------------------------------------
    /** Plays a pad (its note-on; while recording, starts recording a note). Ignored while held. */
    void padDown(int pad, bool accent, int keyCode = 0);
    /** Ends a pad's note (its note-off; while recording, adds the note to the clip). */
    void padUp(int pad);
    /** Ends every held pad's note. */
    void releaseAll();
    /** A new recording pass starts: its notes become one undo step of their own. */
    void startPass() noexcept { newPass_ = true; }
    /** Replaces how key states are read (tests, where there is no keyboard). */
    void setKeyQuery(std::function<bool(int keyCode)> isKeyDown) { host_.isKeyDown = std::move(isKeyDown); }

    [[nodiscard]] bool isHeld(int pad) const noexcept;
    /** Notes recorded into clips since this was created. */
    [[nodiscard]] int getNotesRecorded() const noexcept { return notesRecorded_; }

    // juce::KeyListener
    bool keyPressed(const juce::KeyPress& key, juce::Component* originator) override;
    bool keyStateChanged(bool isKeyDown, juce::Component* originator) override;

private:
    struct Held
    {
        bool down = false;
        int keyCode = 0;
        int velocity = kVelocity;
        std::uint32_t trackId = 0;
        // Recording (when `clip` is valid): where and when the note started.
        juce::ValueTree clip;
        double startBeat = 0.0;
        double startSeconds = 0.0;
        double startMs = 0.0;
    };

    void timerCallback() override;
    void record(const Held& held, int pad);

    session::Session& session_;
    Host host_;
    std::array<Held, kPads> held_{};
    bool newPass_ = true;
    int notesRecorded_ = 0;
};

} // namespace dojo::ui

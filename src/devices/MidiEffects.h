#pragma once

#include "devices/BuiltinProcessor.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace dojo::devices
{

/** Musical scales and keys, shared by the Scale device and the Chord Generator. */
namespace scales
{
struct Scale
{
    const char* name;
    std::uint16_t mask; ///< bit n set: the note n semitones above the root is in the scale
};
[[nodiscard]] const std::vector<Scale>& all();
[[nodiscard]] juce::StringArray names();
/** "C", "C#", ... "B". */
[[nodiscard]] juce::StringArray keyNames();
/** The scale's notes, as semitones above the root (ascending); returns how many. */
int stepsOf(std::uint16_t mask, std::array<int, 12>& steps) noexcept;

enum Snap
{
    nearest = 0, ///< the closer scale note (a tie goes down)
    up,
    down
};
/** `note` moved onto the scale (`mask` from `root`, 0 = C), or -1 if none in range. */
[[nodiscard]] int snap(int note, int root, std::uint16_t mask, Snap direction) noexcept;
/** A note of the scale moved by `degrees` scale degrees (a note off the scale is snapped first). */
[[nodiscard]] int shiftDegrees(int note, int root, std::uint16_t mask, int degrees) noexcept;
/** The white key `note` as the scale's degree: C4 (60) is the root in octave 4 and each white key
    up is the next degree; black keys give -1. */
[[nodiscard]] int whiteKey(int note, int root, std::uint16_t mask) noexcept;
} // namespace scales

/**
    Base of the MIDI effects: devices that change the MIDI on its way to the instrument. They sit
    before the instrument in an instrument track's chain (or on a MIDI track, ahead of the instrument
    track it plays) and pass audio through untouched. Each block's events go in, the effect writes its
    events out (sample-accurate, without allocating).
*/
class MidiEffect : public BuiltinProcessor
{
public:
    MidiEffect(juce::String typeId, juce::String name);

    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return true; }
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlock(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) override;

protected:
    void prepareDsp(double sampleRate, int maximumBlockSize) final;
    void processDsp(juce::AudioBuffer<double>& buffer, juce::MidiBuffer& midi) noexcept final;
    /** The sample rate is known (and the state should start afresh). */
    virtual void prepareMidi() {}
    /** Writes the block's output for its input events (`numSamples` long) into `out`. */
    virtual void processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples) noexcept = 0;

    /** The transport: tempo, position in beats, and whether it plays. */
    struct Transport
    {
        double bpm = 120.0;
        double ppq = 0.0;
        bool playing = false;
    };
    [[nodiscard]] Transport transport() const noexcept;

private:
    void run(juce::MidiBuffer& midi, int numSamples) noexcept;

    juce::MidiBuffer out_;
};

//==================================================================================================
/**
    Arpeggiator: plays the held notes one after another, in a pattern.
    - Style: Up, Down, Up/Down, Down/Up, Converge, Diverge, As Played, Random, Chord.
    - Rate: a note value synced to the tempo (straight, dotted or triplet), or free in Hz. Steps land
      on the song's grid while it plays; without the transport the clock runs on its own.
    - Gate (length of each note), Octaves (the pattern repeated up to 4 octaves higher), Swing (every
      second step later), Repeats (each step played again).
    - Velocity as played or fixed; Hold keeps the notes playing after the keys are let go (a new chord
      replaces them); Retrigger restarts the pattern on every new note.
*/
class Arpeggiator final : public MidiEffect
{
public:
    static constexpr const char* kTypeId = "builtin:arpeggiator";
    enum Style
    {
        up = 0,
        down,
        upDown,
        downUp,
        converge,
        diverge,
        asPlayed,
        random,
        chord
    };
    static constexpr int kMaxHeld = 32;
    static constexpr int kMaxSequence = 256;
    static constexpr int kMaxSounding = 64;
    static constexpr int kShownSteps = 32;

    Arpeggiator();

    [[nodiscard]] static juce::StringArray styleNames();
    [[nodiscard]] static juce::StringArray rateNames();
    /** A Rate choice's step length in beats (quarter notes). */
    [[nodiscard]] static double rateBeats(int index) noexcept;

    struct Held
    {
        int note = 0;
        int velocity = 100;
        int channel = 1;
        int octave = 0; ///< (in the expanded pattern: which octave of it)
    };
    /**
        The pattern for `held` (in the order played): indices into the octave-expanded notes (see
        expanded()), one per step; for Chord each step is an octave (all its notes at once). Returns
        the number of steps.
    */
    static int buildSequence(int style, const Held* held, int count, int octaves, std::array<Held, kMaxSequence>& expanded,
                             int& expandedCount, std::array<int, kMaxSequence>& steps) noexcept;

    /** What the display shows: the pattern (as notes) and the step playing. */
    [[nodiscard]] int getShownLength() const noexcept { return shownLength_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getShownNote(int step) const noexcept;
    [[nodiscard]] int getShownStep() const noexcept { return shownStep_.load(std::memory_order_relaxed); }

    juce::AudioParameterChoice* style_ = nullptr;
    juce::AudioParameterChoice* rate_ = nullptr;
    juce::AudioParameterBool* sync_ = nullptr;
    juce::AudioParameterFloat* freeRate_ = nullptr; ///< Hz
    juce::AudioParameterFloat* gate_ = nullptr;     ///< % of a step
    juce::AudioParameterChoice* octaves_ = nullptr; ///< 1 .. 4
    juce::AudioParameterFloat* swing_ = nullptr;    ///< %
    juce::AudioParameterChoice* repeats_ = nullptr; ///< 1 .. 8
    juce::AudioParameterChoice* velocityMode_ = nullptr;
    juce::AudioParameterFloat* velocity_ = nullptr;
    juce::AudioParameterBool* hold_ = nullptr;
    juce::AudioParameterBool* retrigger_ = nullptr;

protected:
    void prepareMidi() override;
    void processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples) noexcept override;

private:
    struct Sounding
    {
        int note = 0;
        int channel = 1;
        double samplesLeft = 0.0;
    };

    void noteOn(int note, int velocity, int channel, bool locked) noexcept;
    void noteOff(int note) noexcept;
    void releaseAll(juce::MidiBuffer& out, int sample) noexcept;
    void step(juce::MidiBuffer& out, int sample, double stepSamples) noexcept;
    void startNote(juce::MidiBuffer& out, int sample, const Held& note, double gateSamples) noexcept;
    void rebuild() noexcept;
    [[nodiscard]] double stepStart(long long grid, double stepBeats) const noexcept;

    std::array<Held, kMaxHeld> held_{};
    int heldCount_ = 0;
    int keysDown_ = 0;
    std::array<Held, kMaxSequence> expanded_{};
    int expandedCount_ = 0;
    std::array<int, kMaxSequence> sequence_{};
    int sequenceLength_ = 0;
    bool dirty_ = true;
    int builtStyle_ = -1, builtOctaves_ = -1;
    std::array<Sounding, kMaxSounding> sounding_{};
    int soundingCount_ = 0;
    long long stepCount_ = 0;
    int lastRandom_ = -1;
    std::uint32_t randomState_ = 0x9e3779b9u;
    double untilNext_ = 0.0;        ///< samples (from this block's start) until the next step
    long long nextGrid_ = 0;        ///< (on the song's grid) the step it will be
    double lastFired_ = -1.0e300;   ///< (on the grid) where the last step was, in beats
    double expectedPpq_ = -1.0;     ///< where the transport should be at the next block (else it jumped)
    double lockedBeats_ = 0.0, lockedSwing_ = -1.0, lockedBpm_ = 0.0;
    bool wasLocked_ = false;
    bool startNow_ = false;         ///< (free-running) the first note starts the pattern at once
    int freeParity_ = 0;

    std::atomic<int> shownLength_{0}, shownStep_{-1};
    std::array<std::atomic<int>, kShownSteps> shownNotes_{};
};

//==================================================================================================
/**
    Chord Generator: every note plays a chord.
    - Shape: a chord built on the note (major, minor, sevenths, ninths, sus, power, ...) or Custom:
      up to six shifts of -24 to +24 semitones.
    - Diatonic: chords built from the key's own notes (triads, sevenths, ninths, sus2, sus4, power):
      each note gets the chord its scale degree has in the key.
    - Inversion (0 - 3) and Voicing (close, drop 2, spread); Velocity of the added notes (% of the
      played note's); Strum (time between the chord's notes, upwards or downwards).
    Releasing a note releases exactly the chord it played; notes two chords share sound until both
    are released.
*/
class ChordGenerator final : public MidiEffect
{
public:
    static constexpr const char* kTypeId = "builtin:chord";
    static constexpr int kMaxChord = 8;
    static constexpr int kShifts = 6;
    static constexpr int kMaxPending = 128;
    enum Mode
    {
        shapeMode = 0,
        diatonicMode
    };

    ChordGenerator();

    [[nodiscard]] static juce::StringArray shapeNames();
    [[nodiscard]] static juce::StringArray diatonicNames();
    /**
        The chord `note` plays with the current settings (ascending unless strummed downwards):
        returns how many notes. Message thread or audio thread (no allocation).
    */
    int chordFor(int note, std::array<int, kMaxChord>& notes) const noexcept;

    /** The last chord played (for the display). */
    [[nodiscard]] int getShownCount() const noexcept { return shownCount_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getShownNote(int index) const noexcept;

    juce::AudioParameterChoice* mode_ = nullptr;
    juce::AudioParameterChoice* shape_ = nullptr;
    std::array<juce::AudioParameterFloat*, kShifts> shift_{};
    juce::AudioParameterChoice* key_ = nullptr;
    juce::AudioParameterChoice* scale_ = nullptr;
    juce::AudioParameterChoice* diatonic_ = nullptr;
    juce::AudioParameterChoice* inversion_ = nullptr;
    juce::AudioParameterChoice* voicing_ = nullptr;
    juce::AudioParameterFloat* added_ = nullptr; ///< % velocity of the added notes
    juce::AudioParameterFloat* strum_ = nullptr; ///< ms between the notes
    juce::AudioParameterChoice* strumDirection_ = nullptr;

protected:
    void prepareMidi() override;
    void processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples) noexcept override;

private:
    struct Chord
    {
        std::array<std::int8_t, kMaxChord> notes{};
        std::uint8_t count = 0;
    };
    struct Pending
    {
        int note = 0, channel = 1, velocity = 0, source = 0;
        double samplesLeft = 0.0;
    };

    void play(juce::MidiBuffer& out, int sample, int channel, int note, int velocity) noexcept;
    void release(juce::MidiBuffer& out, int sample, int channel, int note) noexcept;
    void noteOnOut(juce::MidiBuffer& out, int sample, int channel, int note, int velocity) noexcept;
    void noteOffOut(juce::MidiBuffer& out, int sample, int channel, int note) noexcept;
    void releaseAll(juce::MidiBuffer& out, int sample) noexcept;

    std::array<std::array<Chord, 128>, 16> chords_{};
    std::array<std::array<std::uint8_t, 128>, 16> sounding_{}; ///< how many chords hold each output note
    std::array<Pending, kMaxPending> pending_{};
    int pendingCount_ = 0;
    std::atomic<int> shownCount_{0};
    std::array<std::atomic<int>, kMaxChord> shownNotes_{};
};

//==================================================================================================
/**
    Scale: keeps notes in a key. Root and Scale (19 scales, major and minor modes, pentatonics, blues,
    whole tone, diminished, ... chromatic). Mode: Nearest (a note off the scale goes to the closer
    scale note, a tie down), Up, Down, Remove (notes off the scale are dropped), or White Keys (the
    white keys play the scale's degrees in order, C4 the root; black keys are silent). Transpose
    (semitones, before the mapping, so the result stays in key), Shift (scale degrees, after it) and
    Octave. A note's release and aftertouch follow wherever its note went.
*/
class ScaleDevice final : public MidiEffect
{
public:
    static constexpr const char* kTypeId = "builtin:scale";
    enum Mode
    {
        nearestMode = 0,
        upMode,
        downMode,
        removeMode,
        whiteKeysMode
    };

    ScaleDevice();

    [[nodiscard]] static juce::StringArray modeNames();
    /** Where `note` goes with the current settings, or -1 (dropped). */
    [[nodiscard]] int map(int note) const noexcept;
    [[nodiscard]] std::uint16_t getMask() const noexcept;

    /** The last note mapped and where it went (-1: none / dropped), for the display. */
    [[nodiscard]] int getLastIn() const noexcept { return lastIn_.load(std::memory_order_relaxed); }
    [[nodiscard]] int getLastOut() const noexcept { return lastOut_.load(std::memory_order_relaxed); }

    juce::AudioParameterChoice* root_ = nullptr;
    juce::AudioParameterChoice* scale_ = nullptr;
    juce::AudioParameterChoice* mode_ = nullptr;
    juce::AudioParameterFloat* transpose_ = nullptr; ///< semitones
    juce::AudioParameterFloat* shift_ = nullptr;     ///< scale degrees
    juce::AudioParameterFloat* octave_ = nullptr;

protected:
    void prepareMidi() override;
    void processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples) noexcept override;

private:
    std::array<std::array<std::int16_t, 128>, 16> mapped_{};   ///< where each held note went (-1: dropped)
    std::array<std::array<std::uint8_t, 128>, 16> sounding_{}; ///< how many held notes play each output note
    std::atomic<int> lastIn_{-1}, lastOut_{-1};
};

//==================================================================================================
/**
    MIDI Filter: lets through only what is wanted.
    - Notes in a range (Lowest - Highest, or outside it), with velocities in a range; their velocity
      scaled and offset.
    - One input channel (or all); every message out on one channel (or its own).
    - Message types on or off: notes, control changes, pitch bend, aftertouch, program changes,
      others (system messages). Channel-mode messages (all notes off) always pass.
    A note let through is always released, whatever changes while it is held.
*/
class MidiFilter final : public MidiEffect
{
public:
    static constexpr const char* kTypeId = "builtin:midifilter";
    enum Kind
    {
        noteKind = 0,
        controlKind,
        pitchBendKind,
        aftertouchKind,
        programKind,
        otherKind,
        kindCount
    };

    MidiFilter();

    /** Messages seen of each kind, and how many were let through (the display's lamps). */
    [[nodiscard]] int getSeen(int kind) const noexcept { return seen_[static_cast<std::size_t>(kind)].load(std::memory_order_relaxed); }
    [[nodiscard]] int getPassed(int kind) const noexcept { return passed_[static_cast<std::size_t>(kind)].load(std::memory_order_relaxed); }

    juce::AudioParameterFloat* lowNote_ = nullptr;
    juce::AudioParameterFloat* highNote_ = nullptr;
    juce::AudioParameterBool* outside_ = nullptr;
    juce::AudioParameterFloat* velocityLow_ = nullptr;
    juce::AudioParameterFloat* velocityHigh_ = nullptr;
    juce::AudioParameterFloat* velocityScale_ = nullptr;  ///< %
    juce::AudioParameterFloat* velocityOffset_ = nullptr; ///< added
    juce::AudioParameterChoice* channel_ = nullptr;        ///< All, 1 .. 16
    juce::AudioParameterChoice* outChannel_ = nullptr;     ///< Keep, 1 .. 16
    std::array<juce::AudioParameterBool*, kindCount> allow_{};

protected:
    void prepareMidi() override;
    void processMidi(const juce::MidiBuffer& in, juce::MidiBuffer& out, int numSamples) noexcept override;

private:
    std::array<std::array<std::int8_t, 128>, 16> held_{}; ///< the channel each let-through note went out on (0: none)
    std::array<std::atomic<int>, kindCount> seen_{}, passed_{};
};

} // namespace dojo::devices

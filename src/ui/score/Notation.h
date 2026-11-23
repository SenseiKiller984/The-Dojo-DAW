#pragma once

#include "core/TempoMap.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace dojo::score
{

/**
    Staff notation of MIDI notes: what the score editor draws and MusicXML export writes.

    The notes are read in timeline beats (quarter notes) on the tempo map's bars. Their starts and
    ends are rounded to the display grid (a sixteenth by default; the notes themselves are not
    changed), each staff is read as one voice (notes starting together are a chord; a note lasts at
    most until the next onset), gaps become rests, and everything is cut at bar lines (tied across)
    and written as notatable values (whole to 64th, dotted where it fits the beat), beamed by beat
    groups (dotted quarters in compound meters). Pitches are spelled in the key (sharps in sharp keys,
    flats in flat keys), with accidentals shown where they differ from the key signature or from an
    earlier note on that line in the bar. Plain C++: no JUCE, no allocation beyond the result.
*/

enum class Clef : std::uint8_t
{
    treble,
    bass,
};

enum class ClefMode : std::uint8_t
{
    automatic, ///< treble, bass, or both (a grand staff) depending on the notes
    treble,
    bass,
    grand, ///< treble from middle C up, bass below
};

enum class Accidental : std::uint8_t
{
    none,
    natural,
    sharp,
    flat,
    doubleSharp,
    doubleFlat,
};

/** A note to notate, in timeline beats. `id` comes back with its heads (for selection). */
struct SourceNote
{
    double start = 0.0;
    double end = 0.0;
    int midi = 60;
    std::uint32_t id = 0;
};

/** A pitch's name: step (0 C ... 6 B), octave (4: the one from middle C), alteration (-2..2). */
struct Spelling
{
    int step = 0;
    int octave = 4;
    int alter = 0;
};

[[nodiscard]] Spelling spell(int midi, int keyFifths) noexcept;
/** The key signature's alteration of a step (+1 sharp, -1 flat, 0). */
[[nodiscard]] int keyAlteration(int step, int keyFifths) noexcept;
/** A diatonic number: octave * 7 + step (middle C, C4, is 28). */
[[nodiscard]] inline int diatonic(const Spelling& s) noexcept { return s.octave * 7 + s.step; }
/** The MIDI note of a spelling. */
[[nodiscard]] int midiOf(const Spelling& s) noexcept;
/** The key (fifths, -7..7: major, or its relative minor) the notes most likely are in
    (Krumhansl-Kessler profiles weighted by duration); C with fewer than six notes or four pitch
    classes (too little to tell). */
[[nodiscard]] int detectKey(const std::vector<SourceNote>& notes) noexcept;
/** A key's name, e.g. "G major / E minor". */
[[nodiscard]] const char* keyName(int keyFifths) noexcept;

/** A note value: type 0 whole, 1 half, 2 quarter, 3 eighth, 4 sixteenth, 5 32nd, 6 64th. */
[[nodiscard]] double valueLength(int type, int dots) noexcept;

struct Head
{
    int midi = 60;
    Spelling spelling;
    Accidental accidental = Accidental::none;
    bool tieStart = false; ///< tied to the same note in the next event
    bool tieEnd = false;   ///< tied from the previous event
    std::uint32_t id = 0;
};

struct Event
{
    double beat = 0.0;   ///< timeline beat it starts at
    double length = 0.0; ///< beats
    int type = 2;
    int dots = 0;
    bool rest = false;
    bool wholeBarRest = false; ///< a rest filling its bar (drawn centred, whatever the meter)
    std::vector<Head> heads;   ///< low to high
    bool stemUp = true;
    int beam = -1; ///< its beam group in the staff (-1: none)
    int bar = 0;   ///< index into Score::bars
};

struct Staff
{
    Clef clef = Clef::treble;
    std::vector<Event> events;
};

struct Bar
{
    std::int64_t number = 0; ///< the timeline's bar (0-based)
    double start = 0.0;      ///< timeline beats
    double end = 0.0;
    time::TimeSignature signature;
    bool showSignature = false; ///< the first bar, or a change
};

struct Score
{
    int keyFifths = 0;
    std::vector<Bar> bars;
    std::vector<Staff> staves;
};

struct Options
{
    ClefMode clef = ClefMode::automatic;
    std::optional<int> key; ///< fifths; none: detected
    double grid = 0.25;     ///< quarter notes (a sixteenth)
};

/** The notation of `notes` over the bars from the one holding `from` to the one holding `to`. */
[[nodiscard]] Score layout(const std::vector<SourceNote>& notes, double from, double to, const time::TempoMap& map, const Options& options);

/** The middle line of a clef, as a diatonic number (B4 treble, D3 bass). */
[[nodiscard]] inline int middleLine(Clef clef) noexcept { return clef == Clef::treble ? 34 : 22; }
/** The top line of a clef (F5 treble, A3 bass). */
[[nodiscard]] inline int topLine(Clef clef) noexcept { return clef == Clef::treble ? 38 : 26; }

} // namespace dojo::score

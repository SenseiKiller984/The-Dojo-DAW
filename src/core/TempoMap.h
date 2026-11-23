#pragma once

#include "core/MusicalTime.h"

#include <cstdint>
#include <vector>

namespace dojo::time
{

/** How the tempo goes from one tempo point to the next. */
enum class TempoShape : std::uint8_t
{
    jump,   ///< holds until the next point, then jumps to it
    linear, ///< ramps from this point's tempo to the next one's, evenly over the beats between them
    curve,  ///< ramps along a curve: `bend` > 0 changes late (slow start), < 0 early (fast start)
};

/** A tempo at a musical position (quarter notes from the start of the timeline). */
struct TempoPoint
{
    double beat = 0.0;
    double bpm = kDefaultTempoBpm;
    TempoShape shape = TempoShape::jump;
    double bend = 0.0; ///< -1..1, for TempoShape::curve
};

/** A time signature from the start of a bar (0-based bar index) on. */
struct SignaturePoint
{
    std::int64_t bar = 0;
    TimeSignature signature;
};

/**
    The timeline's musical time: tempo points (changes, linear ramps and curves) and time signatures.

    Positions in beats are quarter notes from the start of the timeline (second 0). A ramp changes
    the tempo evenly per beat, so its time is closed-form (seconds = 60 / k * ln(tempo / t0) for a
    tempo slope k per beat); a curve is a chain of short ramps along its shape (up to 256, evenly in beats
    and in tempo), which stays within a fraction of a millisecond of the exact integral. Signatures change at bar
    starts; a bar holds `numerator` beats of 4 / `denominator` quarter notes each.

    Immutable once built: queries are const, lock-free and allocation-free, so the audio thread can
    read a map that the message thread hands it (the render graph holds one).
*/
class TempoMap
{
public:
    /** 120 BPM and 4/4 throughout. */
    TempoMap();
    /** `points` in any order (the first at beat 0 sets the start; without one, the earliest point's
        tempo holds from the start); `signatures` likewise (4/4 from bar 0 unless one is given there). */
    TempoMap(std::vector<TempoPoint> points, std::vector<SignaturePoint> signatures);

    [[nodiscard]] const std::vector<TempoPoint>& getPoints() const noexcept { return points_; }
    [[nodiscard]] const std::vector<SignaturePoint>& getSignatures() const noexcept { return signatures_; }
    /** One tempo throughout and 4/4 (the map is the plain project tempo). */
    [[nodiscard]] bool isConstant() const noexcept;

    // --- Time ------------------------------------------------------------------------------------
    [[nodiscard]] double secondsAt(double beat) const noexcept;
    [[nodiscard]] double beatAt(double seconds) const noexcept;
    [[nodiscard]] double bpmAtBeat(double beat) const noexcept;
    [[nodiscard]] double bpmAt(double seconds) const noexcept { return bpmAtBeat(beatAt(seconds)); }
    /** The tempo point in effect at `beat` (its index in getPoints()). */
    [[nodiscard]] int pointIndexAt(double beat) const noexcept;

    // --- Bars --------------------------------------------------------------------------------------
    [[nodiscard]] TimeSignature signatureAt(double beat) const noexcept;
    [[nodiscard]] TimeSignature signatureOfBar(std::int64_t bar) const noexcept;
    /** The 0-based bar `beat` falls in (bars before the start count back in the first signature). */
    [[nodiscard]] std::int64_t barAt(double beat) const noexcept;
    /** Where a 0-based bar starts, in beats. */
    [[nodiscard]] double beatOfBar(std::int64_t bar) const noexcept;
    /** A bar's length in beats (quarter notes). */
    [[nodiscard]] double barLength(std::int64_t bar) const noexcept;
    /** A beat's bar | beat | tick (bars and beats 1-based; beats are the signature's beat unit). */
    [[nodiscard]] BarsBeatsTicks toBarsBeatsTicks(double beat) const noexcept;
    /** The beat of a bar | beat | tick (the inverse of toBarsBeatsTicks). */
    [[nodiscard]] double fromBarsBeatsTicks(BarsBeatsTicks position) const noexcept;

private:
    /** Tempo linear in beats over [beat, beat + beats): bpm0 at its start, bpm1 at its end. */
    struct Segment
    {
        double beat = 0.0;
        double seconds = 0.0;
        double beats = 0.0; ///< (the last one runs on forever)
        double bpm0 = kDefaultTempoBpm;
        double bpm1 = kDefaultTempoBpm;
    };
    struct BarRun
    {
        std::int64_t bar = 0;
        double beat = 0.0;
        double length = 4.0; ///< beats per bar
        TimeSignature signature;
    };

    [[nodiscard]] std::size_t segmentAtBeat(double beat) const noexcept;
    [[nodiscard]] std::size_t segmentAtSeconds(double seconds) const noexcept;
    [[nodiscard]] std::size_t runAtBeat(double beat) const noexcept;
    [[nodiscard]] std::size_t runOfBar(std::int64_t bar) const noexcept;
    static double secondsInto(const Segment& s, double beats) noexcept;
    static double beatsInto(const Segment& s, double seconds) noexcept;

    std::vector<TempoPoint> points_;
    std::vector<SignaturePoint> signatures_;
    std::vector<Segment> segments_;
    std::vector<int> segmentPoint_; ///< the tempo point each segment belongs to
    std::vector<BarRun> runs_;
};

/** The curve shape of TempoShape::curve: 0..1 -> 0..1 (bend 0 is a straight line). */
[[nodiscard]] double tempoCurve(double x, double bend) noexcept;

} // namespace dojo::time

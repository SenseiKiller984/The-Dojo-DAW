#pragma once

#include "core/Types.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dojo::time
{

/** Resolution of musical positions: ticks per quarter note (PPQN). 960 divides evenly by every
    common tuplet (2, 3, 4, 5, 6, 8, 10, 12, 15, 16, 20, 24, 32 ...). */
inline constexpr int kTicksPerQuarterNote = 960;

/** Project defaults until the tempo map arrives with the arranger. */
inline constexpr double kDefaultTempoBpm = 120.0;

struct TimeSignature
{
    int numerator = 4;
    int denominator = 4;
};

/** A position as shown in a DAW's bar|beat|tick display. Bars and beats are 1-based. */
struct BarsBeatsTicks
{
    std::int64_t bar = 1;
    int beat = 1;
    int tick = 0;
};

/** Wall-clock style display of a duration. */
struct ClockTime
{
    std::int64_t hours = 0;
    int minutes = 0;
    int seconds = 0;
    int milliseconds = 0;
};

[[nodiscard]] constexpr double samplesToSeconds(SampleCount samples, double sampleRate) noexcept
{
    return sampleRate > 0.0 ? static_cast<double>(samples) / sampleRate : 0.0;
}

[[nodiscard]] inline SampleCount secondsToSamples(double seconds, double sampleRate) noexcept
{
    return static_cast<SampleCount>(std::llround(seconds * sampleRate));
}

[[nodiscard]] constexpr double secondsToQuarterNotes(double seconds, double tempoBpm) noexcept
{
    return seconds * tempoBpm / 60.0;
}

[[nodiscard]] constexpr double quarterNotesToSeconds(double quarterNotes, double tempoBpm) noexcept
{
    return tempoBpm > 0.0 ? quarterNotes * 60.0 / tempoBpm : 0.0;
}

/** Length of one beat (the signature's denominator note) in quarter notes. */
[[nodiscard]] constexpr double beatLengthInQuarterNotes(TimeSignature signature) noexcept
{
    return 4.0 / static_cast<double>(std::max(1, signature.denominator));
}

[[nodiscard]] inline BarsBeatsTicks quarterNotesToBarsBeatsTicks(double quarterNotes,
                                                                 TimeSignature signature) noexcept
{
    const int beatsPerBar = std::max(1, signature.numerator);
    const double beatLength = beatLengthInQuarterNotes(signature);
    const double totalBeats = std::max(0.0, quarterNotes) / beatLength;
    const auto wholeBeats = static_cast<std::int64_t>(std::floor(totalBeats));

    const auto ticksPerBeat = static_cast<int>(std::lround(kTicksPerQuarterNote * beatLength));
    const double fraction = totalBeats - static_cast<double>(wholeBeats);
    const int tick =
        std::clamp(static_cast<int>(std::floor(fraction * ticksPerBeat + 1.0e-9)), 0, ticksPerBeat - 1);

    BarsBeatsTicks result;
    result.bar = wholeBeats / beatsPerBar + 1;
    result.beat = static_cast<int>(wholeBeats % beatsPerBar) + 1;
    result.tick = tick;
    return result;
}

[[nodiscard]] inline ClockTime secondsToClockTime(double seconds) noexcept
{
    const auto totalMs = static_cast<std::int64_t>(std::floor(std::max(0.0, seconds) * 1000.0 + 1.0e-6));

    ClockTime result;
    result.milliseconds = static_cast<int>(totalMs % 1000);
    result.seconds = static_cast<int>((totalMs / 1000) % 60);
    result.minutes = static_cast<int>((totalMs / 60'000) % 60);
    result.hours = totalMs / 3'600'000;
    return result;
}

} // namespace dojo::time

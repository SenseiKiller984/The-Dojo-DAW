#include "core/MusicalTime.h"

#include <catch2/catch_test_macros.hpp>

using namespace dojo::time;

TEST_CASE("Quarter notes map onto 1-based bars and beats in 4/4", "[time]")
{
    const TimeSignature fourFour{4, 4};

    auto bbt = quarterNotesToBarsBeatsTicks(0.0, fourFour);
    CHECK(bbt.bar == 1);
    CHECK(bbt.beat == 1);
    CHECK(bbt.tick == 0);

    bbt = quarterNotesToBarsBeatsTicks(5.5, fourFour); // bar 2, beat 2, half a beat
    CHECK(bbt.bar == 2);
    CHECK(bbt.beat == 2);
    CHECK(bbt.tick == kTicksPerQuarterNote / 2);

    bbt = quarterNotesToBarsBeatsTicks(16.0, fourFour);
    CHECK(bbt.bar == 5);
    CHECK(bbt.beat == 1);
    CHECK(bbt.tick == 0);
}

TEST_CASE("Compound meters use the denominator as the beat unit", "[time]")
{
    const TimeSignature sixEight{6, 8}; // six eighth-note beats per bar = 3 quarter notes

    auto bbt = quarterNotesToBarsBeatsTicks(3.0, sixEight);
    CHECK(bbt.bar == 2);
    CHECK(bbt.beat == 1);

    bbt = quarterNotesToBarsBeatsTicks(1.0, sixEight); // 2 eighths in
    CHECK(bbt.bar == 1);
    CHECK(bbt.beat == 3);
    CHECK(bbt.tick == 0);
}

TEST_CASE("Tempo conversions round-trip", "[time]")
{
    CHECK(secondsToQuarterNotes(1.0, 120.0) == 2.0);
    CHECK(quarterNotesToSeconds(2.0, 120.0) == 1.0);
    CHECK(secondsToSamples(samplesToSeconds(123'456, 48'000.0), 48'000.0) == 123'456);
}

TEST_CASE("Clock time formatting fields", "[time]")
{
    const auto t = secondsToClockTime(3723.5049); // 1h 2m 3.504s
    CHECK(t.hours == 1);
    CHECK(t.minutes == 2);
    CHECK(t.seconds == 3);
    CHECK(t.milliseconds == 504);

    CHECK(secondsToClockTime(-5.0).seconds == 0); // negative positions clamp to zero
}

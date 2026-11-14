#include "devices/Modulators.h"
#include "EffectTestHelpers.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
Stereo constant(std::size_t n, double value)
{
    Stereo s(n);
    std::fill(s.left.begin(), s.left.end(), value);
    std::fill(s.right.begin(), s.right.end(), value);
    return s;
}

double correlation(const std::vector<double>& a, const std::vector<double>& b, std::size_t begin, std::size_t end)
{
    double ab = 0.0, aa = 0.0, bb = 0.0;
    for (std::size_t i = begin; i < end; ++i)
    {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    return ab / std::sqrt(std::max(1.0e-30, aa * bb));
}
} // namespace

//==================================================================================================
// Shifter

TEST_CASE("Shifter transposes by semitones without changing the timing", "[devices][shifter]")
{
    auto [node, shifter] = makeNode<Shifter>();
    setParameter(*shifter, "mix", 100.0f);
    const auto input = tone(440.0, at(0.6), 0.4);

    setParameter(*shifter, "pitch", 12.0f);
    auto out = run(*node, input);
    CHECK_THAT(test::estimateFrequency(out.left, at(0.2), at(0.6), kRate), WithinRel(880.0, 0.01));
    // (the grains restart with unrelated phases, so the level is measured as RMS, not one long
    // coherent sine)
    CHECK(rmsOf(out.left, at(0.2), at(0.6)) > 0.6 * rmsOf(input.left, at(0.2), at(0.6)));
    CHECK(amplitudeAt(out.left, 440.0, at(0.2), at(0.6)) < 0.03);
    CHECK_THAT(shifter->getCurrentShift(), WithinAbs(12.0, 1e-6));

    setParameter(*shifter, "pitch", -7.0f);
    setParameter(*shifter, "fine", -2.0f);
    out = run(*node, input);
    CHECK_THAT(test::estimateFrequency(out.left, at(0.2), at(0.6), kRate), WithinRel(440.0 * std::exp2(-7.02 / 12.0), 0.01));

    // The LFO moves the pitch (here +-6 semitones), shown live.
    setParameter(*shifter, "fine", 0.0f);
    setParameter(*shifter, "pitch", 0.0f);
    setParameter(*shifter, "lfoamt", 50.0f);
    setParameter(*shifter, "lforate", 4.0f);
    setParameter(*shifter, "lfoshape", 4.0f); // square
    float highest = -100.0f, lowest = 100.0f;
    for (int block = 0; block < 40; ++block)
    {
        run(*node, Stereo(kBlock));
        highest = std::max(highest, shifter->getCurrentShift());
        lowest = std::min(lowest, shifter->getCurrentShift());
    }
    CHECK_THAT(highest, WithinAbs(6.0, 0.01));
    CHECK_THAT(lowest, WithinAbs(-6.0, 0.01));
}

TEST_CASE("Shifter frequency shifts one sideband, ring modulates both", "[devices][shifter]")
{
    auto [node, shifter] = makeNode<Shifter>();
    setParameter(*shifter, "mix", 100.0f);
    setParameter(*shifter, "mode", 1.0f); // frequency
    setParameter(*shifter, "shift", 100.0f);
    const auto input = sum(tone(440.0, at(0.6), 0.3), tone(1000.0, at(0.6), 0.3));
    auto out = run(*node, input);
    // Every partial moves by the same 100 Hz (inharmonic), the lower sideband is suppressed.
    CHECK(amplitudeAt(out.left, 540.0, at(0.2), at(0.6)) > 0.25);
    CHECK(amplitudeAt(out.left, 1100.0, at(0.2), at(0.6)) > 0.25);
    CHECK(amplitudeAt(out.left, 340.0, at(0.2), at(0.6)) < 0.01);
    CHECK(amplitudeAt(out.left, 900.0, at(0.2), at(0.6)) < 0.01);
    CHECK(amplitudeAt(out.left, 440.0, at(0.2), at(0.6)) < 0.01);

    // Negative: down.
    setParameter(*shifter, "shift", -200.0f);
    out = run(*node, input);
    CHECK(amplitudeAt(out.left, 240.0, at(0.2), at(0.6)) > 0.25);
    CHECK(amplitudeAt(out.left, 640.0, at(0.2), at(0.6)) < 0.01);

    // Spread: left and right shifted apart (+-20 Hz at 100 %).
    setParameter(*shifter, "shift", 100.0f);
    setParameter(*shifter, "spread", 100.0f);
    out = run(*node, tone(440.0, at(0.6), 0.3));
    CHECK(amplitudeAt(out.left, 560.0, at(0.2), at(0.6)) > 0.25);
    CHECK(amplitudeAt(out.right, 520.0, at(0.2), at(0.6)) > 0.25);
    setParameter(*shifter, "spread", 0.0f);

    // Ring modulation: both sidebands, half each.
    setParameter(*shifter, "mode", 2.0f);
    out = run(*node, tone(440.0, at(0.6), 0.4));
    CHECK_THAT(amplitudeAt(out.left, 340.0, at(0.2), at(0.6)), WithinAbs(0.2, 0.01));
    CHECK_THAT(amplitudeAt(out.left, 540.0, at(0.2), at(0.6)), WithinAbs(0.2, 0.01));
    CHECK(amplitudeAt(out.left, 440.0, at(0.2), at(0.6)) < 0.005);

    // Dry/wet 0: the input.
    setParameter(*shifter, "mix", 0.0f);
    const auto dry = tone(440.0, at(0.1), 0.4);
    out = run(*node, dry);
    for (std::size_t i = 0; i < out.left.size(); ++i)
        REQUIRE(out.left[i] == dry.left[i]);
}

TEST_CASE("Shifter feedback spirals: each repeat is shifted again", "[devices][shifter]")
{
    auto [node, shifter] = makeNode<Shifter>();
    setParameter(*shifter, "mix", 100.0f);
    setParameter(*shifter, "mode", 1.0f);
    setParameter(*shifter, "shift", 200.0f);
    setParameter(*shifter, "feedback", 70.0f);
    setParameter(*shifter, "delay", 100.0f);
    const auto burst = concat({tone(1000.0, at(0.05), 0.3), Stereo(at(0.35))});
    const auto out = run(*node, burst);
    CHECK(amplitudeAt(out.left, 1200.0, at(0.0), at(0.05)) > 0.2);   // shifted once
    CHECK(amplitudeAt(out.left, 1400.0, at(0.1), at(0.15)) > 0.08);  // twice, 100 ms later
    CHECK(amplitudeAt(out.left, 1600.0, at(0.2), at(0.25)) > 0.03);  // three times
    REQUIRE(allFinite(out));
}

//==================================================================================================
// Chorus-Ensemble

TEST_CASE("Chorus-Ensemble widens a mono signal; Vibrato bends the pitch", "[devices][chorus]")
{
    CHECK_THAT(ChorusEnsemble::voiceDelayMs(ChorusEnsemble::Mode::classic, 1.0, 1.0), WithinAbs(11.0, 1e-9));
    CHECK_THAT(ChorusEnsemble::voiceDelayMs(ChorusEnsemble::Mode::classic, -1.0, 1.0), WithinAbs(3.0, 1e-9));
    CHECK_THAT(ChorusEnsemble::voiceDelayMs(ChorusEnsemble::Mode::vibrato, 0.3, 0.0), WithinAbs(6.0, 1e-9));

    auto [node, chorus] = makeNode<ChorusEnsemble>();
    setParameter(*chorus, "mix", 100.0f);
    setParameter(*chorus, "amount", 100.0f);
    setParameter(*chorus, "rate", 2.0f);
    const auto input = sum(tone(700.0, at(1.0), 0.2), tone(1900.0, at(1.0), 0.2));

    for (const int mode : {0, 1})
    {
        INFO("mode " << mode);
        setParameter(*chorus, "mode", static_cast<float>(mode));
        const auto out = run(*node, input);
        CHECK(correlation(out.left, out.right, at(0.2), at(1.0)) < 0.9);
        CHECK(rmsOf(out.left, at(0.2), at(1.0)) > 0.1);
    }
    // Width 0: the wet signal is mono.
    setParameter(*chorus, "width", 0.0f);
    auto out = run(*node, input);
    CHECK(correlation(out.left, out.right, at(0.2), at(1.0)) > 0.9999);
    setParameter(*chorus, "width", 100.0f);

    // Vibrato: the pitch of a steady tone swings (here about +-31 Hz at 1 kHz).
    setParameter(*chorus, "mode", 2.0f);
    setParameter(*chorus, "rate", 1.0f);
    out = run(*node, tone(1000.0, at(1.2), 0.3));
    double lowest = 1.0e9, highest = 0.0;
    for (double t = 0.1; t < 1.1; t += 0.05)
    {
        const double f = test::estimateFrequency(out.left, at(t), at(t + 0.05), kRate);
        lowest = std::min(lowest, f);
        highest = std::max(highest, f);
    }
    CHECK(highest - lowest > 30.0);
    CHECK(highest - lowest < 80.0);

    // Dry/wet 0 is the input.
    setParameter(*chorus, "mix", 0.0f);
    const auto dry = tone(500.0, at(0.1), 0.3);
    out = run(*node, dry);
    for (std::size_t i = 0; i < out.left.size(); ++i)
        REQUIRE(out.left[i] == dry.left[i]);
}

TEST_CASE("Chorus-Ensemble feedback, warmth and the wet high-pass", "[devices][chorus]")
{
    auto [node, chorus] = makeNode<ChorusEnsemble>();
    setParameter(*chorus, "mix", 100.0f);
    setParameter(*chorus, "amount", 0.0f); // a fixed 7 ms delay

    // Feedback: an impulse comes back every 7 ms.
    setParameter(*chorus, "feedback", 80.0f);
    Stereo click(at(0.1));
    click.left[0] = click.right[0] = 1.0;
    auto out = run(*node, click);
    const auto first = static_cast<std::size_t>(0.007 * kRate);
    CHECK(peakOf(out.left, first - 2, first + 3) > 0.9);
    CHECK_THAT(peakOf(out.left, 2 * first - 2, 2 * first + 3), WithinAbs(0.8, 0.05));
    CHECK(peakOf(out.left, 3 * first - 2, 3 * first + 3) > 0.55);
    setParameter(*chorus, "feedback", 0.0f);

    // Warmth darkens the wet signal.
    const auto bright = tone(10000.0, at(0.3), 0.2);
    const double clean = amplitudeAt(run(*node, bright).left, 10000.0, at(0.1), at(0.3));
    setParameter(*chorus, "warmth", 100.0f);
    CHECK(toDb(amplitudeAt(run(*node, bright).left, 10000.0, at(0.1), at(0.3)) / clean) < -6.0);
    setParameter(*chorus, "warmth", 0.0f);

    // The high-pass keeps the lows out of the wet signal.
    const auto bass = tone(50.0, at(0.5), 0.2);
    const double full = amplitudeAt(run(*node, bass).left, 50.0, at(0.2), at(0.5));
    setParameter(*chorus, "highpass", 500.0f);
    CHECK(toDb(amplitudeAt(run(*node, bass).left, 50.0, at(0.2), at(0.5)) / full) < -20.0);
}

//==================================================================================================
// Auto Pan

TEST_CASE("Auto Pan pans from side to side at 180 degrees, tremolos at 0", "[devices][autopan]")
{
    auto [node, pan] = makeNode<AutoPan>();
    setParameter(*pan, "amount", 100.0f);
    setParameter(*pan, "rate", 1.0f);
    const auto input = constant(at(1.0), 0.5);

    // Sine, phase 180: left loudest a quarter of the way in, right silent; then the reverse. The
    // total stays constant.
    auto out = run(*node, input);
    CHECK_THAT(out.left[at(0.25)], WithinAbs(0.5, 0.01));
    CHECK_THAT(out.right[at(0.25)], WithinAbs(0.0, 0.01));
    CHECK_THAT(out.left[at(0.75)], WithinAbs(0.0, 0.01));
    CHECK_THAT(out.right[at(0.75)], WithinAbs(0.5, 0.01));
    for (std::size_t i = at(0.05); i < out.left.size(); i += 480)
        CHECK_THAT(out.left[i] + out.right[i], WithinAbs(0.5, 0.01));

    // Phase 0: a tremolo (both channels together); half the amount, half the dip.
    setParameter(*pan, "phase", 0.0f);
    setParameter(*pan, "amount", 50.0f);
    out = run(*node, input);
    for (std::size_t i = at(0.05); i < out.left.size(); i += 480)
        CHECK_THAT(out.left[i], WithinAbs(out.right[i], 1e-9));
    CHECK_THAT(out.left[at(0.25)], WithinAbs(0.5, 0.01));
    CHECK_THAT(out.left[at(0.75)], WithinAbs(0.25, 0.01));

    // Invert flips the cycle; Square pushes it to hard switching.
    setParameter(*pan, "invert", 1.0f);
    out = run(*node, input);
    CHECK_THAT(out.left[at(0.25)], WithinAbs(0.25, 0.01));
    setParameter(*pan, "invert", 0.0f);
    setParameter(*pan, "amount", 100.0f);
    setParameter(*pan, "square", 100.0f);
    CHECK(pan->gainFor(0.2) > 0.95);
    CHECK(pan->gainFor(-0.2) < 0.05);
}

TEST_CASE("Auto Pan synced to the song: one cycle per beat at 1/4", "[devices][autopan]")
{
    auto [node, pan] = makeNode<AutoPan>();
    setParameter(*pan, "amount", 100.0f);
    setParameter(*pan, "phase", 0.0f);
    setParameter(*pan, "sync", 1.0f);
    setParameter(*pan, "division", 9.0f); // 1/4
    std::vector<std::pair<double, float>> gains;
    runPlaying(*node, *pan, constant(at(2.0), 0.5), 120.0, true,
               [&](std::size_t done) { gains.emplace_back(static_cast<double>(done) / kRate, pan->getGain(0)); });
    std::vector<double> peaks;
    for (std::size_t i = 1; i + 1 < gains.size(); ++i)
        if (gains[i].second >= gains[i - 1].second && gains[i].second > gains[i + 1].second && gains[i].second > 0.95f)
            peaks.push_back(gains[i].first);
    REQUIRE(peaks.size() >= 3);
    for (std::size_t p = 1; p < peaks.size(); ++p)
        CHECK_THAT(peaks[p] - peaks[p - 1], WithinAbs(0.5, 0.012));
}

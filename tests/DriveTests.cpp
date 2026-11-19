#include "devices/Drive.h"
#include "devices/Roar.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;

namespace
{
/** Harmonic `k` of `f0` relative to the fundamental, in dB. */
double harmonicDb(const std::vector<double>& x, double f0, int k, std::size_t begin, std::size_t end)
{
    return toDb(amplitudeAt(x, f0 * k, begin, end) / std::max(1.0e-12, amplitudeAt(x, f0, begin, end)));
}

/** Plucks: a 220 Hz tone decaying with time constant `tau`, restarted every `period` seconds. */
Stereo plucks(double seconds, double period, double tau, double amplitude)
{
    Stereo s(at(seconds));
    for (std::size_t i = 0; i < s.left.size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        const double local = std::fmod(t, period);
        s.left[i] = s.right[i] = amplitude * std::exp(-local / tau) * std::sin(2.0 * kTestPi * 220.0 * t);
    }
    return s;
}

/** Output equals the input delayed by `latency` samples. */
bool isDelayedCopy(const Stereo& out, const Stereo& in, int latency, double tolerance)
{
    for (std::size_t i = static_cast<std::size_t>(latency); i < out.left.size(); ++i)
        if (std::abs(out.left[i] - in.left[i - static_cast<std::size_t>(latency)]) > tolerance)
            return false;
    return true;
}
} // namespace

//==================================================================================================
// Pedal

TEST_CASE("Pedal: more gain is more distortion; the fuzz is asymmetric, the overdrive symmetric", "[devices][pedal]")
{
    auto [node, pedal] = makeNode<Pedal>();
    const auto input = tone(200.0, at(0.5), 0.3);
    const auto begin = at(0.2), end = at(0.5);

    setParameter(*pedal, "gain", 10.0f);
    const double gentle = harmonicDb(run(*node, input).left, 200.0, 3, begin, end);
    setParameter(*pedal, "gain", 90.0f);
    const auto driven = run(*node, input);
    CHECK(harmonicDb(driven.left, 200.0, 3, begin, end) > gentle + 10.0);
    // Symmetric clipping: odd harmonics, (almost) no even ones.
    CHECK(harmonicDb(driven.left, 200.0, 2, begin, end) < harmonicDb(driven.left, 200.0, 3, begin, end) - 20.0);

    setParameter(*pedal, "type", 2.0f); // fuzz
    const auto fuzz = run(*node, input);
    CHECK(harmonicDb(fuzz.left, 200.0, 2, begin, end) > -20.0); // biased: strong even harmonics
    CHECK(Pedal::shape(Pedal::Type::fuzz, 3.0) < -Pedal::shape(Pedal::Type::fuzz, -3.0)); // clips harder on one side

    // Every type, full gain, loud input: finite and bounded.
    setParameter(*pedal, "gain", 100.0f);
    for (int type = 0; type < 3; ++type)
    {
        setParameter(*pedal, "type", static_cast<float>(type));
        const auto out = run(*node, tone(80.0, at(0.3), 1.0));
        REQUIRE(allFinite(out));
        CHECK(peakOf(out.left) < 2.0);
        CHECK(peakOf(out.left, at(0.1)) > 0.05);
    }
}

TEST_CASE("Pedal EQ, Sub and a latency-aligned dry signal", "[devices][pedal]")
{
    auto [node, pedal] = makeNode<Pedal>();
    setParameter(*pedal, "gain", 0.0f);
    const auto measure = [&](double f)
    {
        const auto out = run(*node, tone(f, at(0.4), 0.005));
        return toDb(amplitudeAt(out.left, f, at(0.2), at(0.4)));
    };
    const double flatTreble = measure(6000.0);
    setParameter(*pedal, "treble", 100.0f);
    CHECK(measure(6000.0) - flatTreble > 8.0);
    setParameter(*pedal, "treble", 0.0f);

    const double flatLow = measure(60.0);
    setParameter(*pedal, "sub", 1.0f);
    CHECK(measure(60.0) - flatLow > 4.0);
    setParameter(*pedal, "sub", 0.0f);

    const double flatMid = measure(500.0);
    setParameter(*pedal, "mid", -100.0f);
    CHECK(measure(500.0) - flatMid < -9.0);

    // The oversampling latency is reported, and the dry signal is aligned to it.
    const int latency = node->getLatencySamples();
    CHECK(latency > 0);
    setParameter(*pedal, "mix", 0.0f);
    const auto input = tone(440.0, at(0.2), 0.4);
    CHECK(isDelayedCopy(run(*node, input), input, latency, 1.0e-9));
}

//==================================================================================================
// Overdrive

TEST_CASE("Overdrive: drive, the band-pass in front, tone", "[devices][overdrive]")
{
    auto [node, od] = makeNode<Overdrive>();
    const auto input = tone(220.0, at(0.5), 0.2);
    const auto begin = at(0.2), end = at(0.5);

    setParameter(*od, "drive", 0.0f);
    const double clean = harmonicDb(run(*node, input).left, 220.0, 3, begin, end);
    setParameter(*od, "drive", 80.0f);
    const auto driven = run(*node, input);
    CHECK(harmonicDb(driven.left, 220.0, 3, begin, end) > clean + 15.0);
    CHECK(harmonicDb(driven.left, 220.0, 2, begin, end) < harmonicDb(driven.left, 220.0, 3, begin, end) - 20.0);

    // Tone: brighter keeps more of the upper harmonics.
    setParameter(*od, "tone", 0.0f);
    const double dark = harmonicDb(run(*node, input).left, 220.0, 9, begin, end);
    setParameter(*od, "tone", 100.0f);
    CHECK(harmonicDb(run(*node, input).left, 220.0, 9, begin, end) > dark + 6.0);

    // A narrow band around 1.1 kHz: a 100 Hz input is mostly filtered out before the drive.
    setParameter(*od, "drive", 0.0f);
    const auto low = tone(100.0, at(0.4), 0.01);
    setParameter(*od, "width", 9.0f);
    const double wide = amplitudeAt(run(*node, low).left, 100.0, at(0.2), at(0.4));
    setParameter(*od, "width", 1.0f);
    const double narrow = amplitudeAt(run(*node, low).left, 100.0, at(0.2), at(0.4));
    CHECK(toDb(narrow / wide) < -20.0);
}

TEST_CASE("Overdrive Dynamics lets the input's attacks back through the clipping", "[devices][overdrive]")
{
    auto [node, od] = makeNode<Overdrive>();
    setParameter(*od, "drive", 90.0f);
    const auto input = plucks(2.0, 0.5, 0.12, 0.5);
    // Level early in each pluck (attack) over late in it (decay), in dB.
    const auto attackOverDecay = [&]
    {
        const auto out = run(*node, input);
        double attack = 0.0, decay = 0.0;
        for (int k = 1; k < 4; ++k)
        {
            const double t = 0.5 * k;
            attack += rmsOf(out.left, at(t + 0.01), at(t + 0.05));
            decay += rmsOf(out.left, at(t + 0.3), at(t + 0.4));
        }
        return toDb(attack / decay);
    };
    setParameter(*od, "dynamics", 0.0f);
    const double squashed = attackOverDecay();
    setParameter(*od, "dynamics", 100.0f);
    const double restored = attackOverDecay();
    CHECK(restored > squashed + 4.0);
}

//==================================================================================================
// Redux

TEST_CASE("Redux quantises with shape and DC shift", "[devices][redux]")
{
    CHECK_THAT(Redux::quantise(0.3, 8.0, 0.0, false), WithinAbs(38.0 / 128.0, 1e-12));
    CHECK_THAT(Redux::quantise(-0.3, 8.0, 0.0, false), WithinAbs(-38.0 / 128.0, 1e-12));
    // DC shift truncates (towards -infinity): asymmetric steps.
    CHECK_THAT(Redux::quantise(0.3, 8.0, 0.0, true), WithinAbs(38.0 / 128.0, 1e-12));
    CHECK_THAT(Redux::quantise(-0.3, 8.0, 0.0, true), WithinAbs(-39.0 / 128.0, 1e-12));
    // Shape keeps detail near zero that plain 4-bit loses.
    CHECK(Redux::quantise(0.001, 4.0, 0.0, false) == 0.0);
    CHECK(Redux::quantise(0.001, 4.0, 1.0, false) > 0.0);
    // Fewer bits, more noise.
    double coarse = 0.0, fine = 0.0;
    for (int i = 0; i < 1000; ++i)
    {
        const double x = 0.5 * std::sin(i * 0.0137);
        coarse += std::pow(Redux::quantise(x, 4.0, 0.0, false) - x, 2.0);
        fine += std::pow(Redux::quantise(x, 12.0, 0.0, false) - x, 2.0);
    }
    CHECK(coarse > fine * 1000.0);
}

TEST_CASE("Redux downsampling aliases, its filters tame it, full settings are transparent", "[devices][redux]")
{
    auto [node, redux] = makeNode<Redux>();
    setParameter(*redux, "bits", 16.0f);
    setParameter(*redux, "rate", 4000.0f);

    // 3 kHz held at 4 kHz folds down to 1 kHz...
    setParameter(*redux, "prefilter", 0.0f);
    const auto input = tone(3000.0, at(0.4), 0.4);
    auto out = run(*node, input);
    CHECK(amplitudeAt(out.left, 1000.0, at(0.1), at(0.4)) > 0.15);
    // ... unless the pre-filter removes it first.
    setParameter(*redux, "prefilter", 1.0f);
    out = run(*node, input);
    CHECK(amplitudeAt(out.left, 1000.0, at(0.1), at(0.4)) < 0.01);

    // The post-filter smooths the steps: the images of a 500 Hz tone (3.5 kHz) shrink.
    const auto low = tone(500.0, at(0.4), 0.4);
    const double stepped = amplitudeAt(run(*node, low).left, 3500.0, at(0.1), at(0.4));
    setParameter(*redux, "postfilter", 1.0f);
    const double smoothed = amplitudeAt(run(*node, low).left, 3500.0, at(0.1), at(0.4));
    CHECK(toDb(smoothed / stepped) < -20.0);

    // Jitter changes the clock but stays finite.
    setParameter(*redux, "jitter", 100.0f);
    REQUIRE(allFinite(run(*node, low)));

    // Full rate and 16 bits: nearly transparent. Dry/wet 0: exactly the input.
    setParameter(*redux, "jitter", 0.0f);
    setParameter(*redux, "postfilter", 0.0f);
    setParameter(*redux, "rate", 48000.0f);
    out = run(*node, low);
    double error = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i)
        error = std::max(error, std::abs(out.left[i] - low.left[i]));
    CHECK(error < 1.0e-4);
    setParameter(*redux, "mix", 0.0f);
    setParameter(*redux, "bits", 2.0f);
    CHECK(isDelayedCopy(run(*node, low), low, 0, 1.0e-12));
}

//==================================================================================================
// Roar

TEST_CASE("Roar: every shape and routing is stable; idle stages pass the signal", "[devices][roar]")
{
    auto [node, roar] = makeNode<Roar>();
    const int latency = node->getLatencySamples();
    CHECK(latency > 0);

    // No drive and every stage off: serial, parallel and multiband give back the input.
    setParameter(*roar, "drive", 0.0f);
    for (int s = 1; s <= Roar::kStages; ++s)
        setParameter(*roar, "s" + juce::String(s) + "on", 0.0f);
    const auto input = sum(sum(tone(100.0, at(0.4), 0.2), tone(1000.0, at(0.4), 0.2)), tone(8000.0, at(0.4), 0.2));
    for (const int routing : {1, 2, 4})
    {
        setParameter(*roar, "routing", static_cast<float>(routing));
        const auto out = run(*node, input);
        for (const double f : {100.0, 1000.0, 8000.0})
        {
            INFO("routing " << routing << " at " << f);
            CHECK_THAT(toDb(amplitudeAt(out.left, f, at(0.2), at(0.4)) / 0.2), WithinAbs(0.0, 0.2));
        }
    }

    // Every shape in every routing, hard: finite and bounded.
    for (int s = 1; s <= Roar::kStages; ++s)
    {
        setParameter(*roar, "s" + juce::String(s) + "on", 1.0f);
        setParameter(*roar, "s" + juce::String(s) + "amount", 100.0f);
        setParameter(*roar, "s" + juce::String(s) + "bias", 60.0f);
        setParameter(*roar, "s" + juce::String(s) + "filter", 5.0f);
        setParameter(*roar, "s" + juce::String(s) + "res", 100.0f);
    }
    setParameter(*roar, "drive", 24.0f);
    setParameter(*roar, "fbamount", 100.0f);
    setParameter(*roar, "fbtime", 5.0f);
    const auto loud = tone(110.0, at(0.25), 0.9);
    for (int routing = 0; routing < 5; ++routing)
        for (int shape = 0; shape < Roar::getShapeNames().size(); ++shape)
        {
            setParameter(*roar, "routing", static_cast<float>(routing));
            for (int s = 1; s <= Roar::kStages; ++s)
                setParameter(*roar, "s" + juce::String(s) + "shape", static_cast<float>(shape));
            const auto out = run(*node, loud);
            INFO("routing " << routing << " shape " << shape);
            REQUIRE(allFinite(out));
            CHECK(peakOf(out.left) < 20.0);
        }
}

TEST_CASE("Roar: amount adds harmonics, filters shape them, mid/side keeps a mono signal mono", "[devices][roar]")
{
    auto [node, roar] = makeNode<Roar>();
    setParameter(*roar, "drive", 0.0f);
    const auto input = tone(200.0, at(0.4), 0.4);
    const auto begin = at(0.2), end = at(0.4);

    setParameter(*roar, "s1amount", 0.0f);
    const double soft = harmonicDb(run(*node, input).left, 200.0, 3, begin, end);
    setParameter(*roar, "s1amount", 80.0f);
    const auto hard = run(*node, input);
    CHECK(harmonicDb(hard.left, 200.0, 3, begin, end) > soft + 15.0);

    // A low-pass after the shape takes the upper harmonics away.
    setParameter(*roar, "s1filter", 1.0f);
    setParameter(*roar, "s1freq", 400.0f);
    CHECK(harmonicDb(run(*node, input).left, 200.0, 7, begin, end) < harmonicDb(hard.left, 200.0, 7, begin, end) - 15.0);
    setParameter(*roar, "s1filter", 0.0f);

    // Asymmetric shapes make even harmonics.
    setParameter(*roar, "s1amount", 40.0f);
    setParameter(*roar, "s1shape", static_cast<float>(Roar::Shape::diodeClipper));
    CHECK(harmonicDb(run(*node, input).left, 200.0, 2, begin, end) > -25.0);

    // Mid/Side: the side of a mono signal is silent, so the output stays mono (once the filters have
    // forgotten the stereo routing before).
    setParameter(*roar, "routing", 3.0f);
    const auto out = run(*node, input);
    double difference = 0.0;
    for (std::size_t i = at(0.25); i < out.left.size(); ++i)
        difference = std::max(difference, std::abs(out.left[i] - out.right[i]));
    CHECK(difference < 1.0e-6);
    CHECK(peakOf(out.left, at(0.2)) > 0.1);

    // Dry/wet 0: the untouched input, latency-aligned.
    setParameter(*roar, "mix", 0.0f);
    CHECK(isDelayedCopy(run(*node, input), input, node->getLatencySamples(), 1.0e-9));
}

TEST_CASE("Roar: the modulation matrix, feedback echoes and the compressor", "[devices][roar]")
{
    auto [node, roar] = makeNode<Roar>();
    setParameter(*roar, "drive", 0.0f);

    // LFO 1 (2 Hz square) on Stage 1 Amount: the distortion comes and goes with it.
    setParameter(*roar, "s1amount", 0.0f);
    setParameter(*roar, "l1shape", 4.0f);
    setParameter(*roar, "l1rate", 2.0f);
    setParameter(*roar, "m1src", static_cast<float>(Roar::srcLfo1));
    setParameter(*roar, "m1dst", static_cast<float>(Roar::dstStage1Amount));
    setParameter(*roar, "m1amt", 100.0f);
    const auto input = tone(200.0, at(1.0), 0.4);
    const auto out = run(*node, input);
    const double driven = harmonicDb(out.left, 200.0, 3, at(0.05), at(0.2));  // LFO high
    const double clean = harmonicDb(out.left, 200.0, 3, at(0.3), at(0.45));   // LFO low
    CHECK(driven > clean + 15.0);

    // The envelope follows the input level.
    setParameter(*roar, "m1src", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    run(*node, tone(200.0, at(0.2), 0.001));
    const float quiet = roar->getSourceValue(Roar::srcEnvelope);
    run(*node, tone(200.0, at(0.2), 0.8));
    CHECK(roar->getSourceValue(Roar::srcEnvelope) > quiet + 0.5f);

    // Feedback: a click comes back every 100 ms, fading.
    setParameter(*roar, "s1on", 0.0f);
    setParameter(*roar, "fbamount", 80.0f);
    setParameter(*roar, "fbtime", 100.0f);
    setParameter(*roar, "fbfreq", 1000.0f);
    node->prepare(kRate, kBlock, nullptr);
    Stereo click(at(0.5));
    for (std::size_t i = 0; i < 48; ++i)
        click.left[i] = click.right[i] = 0.8 * std::sin(2.0 * kTestPi * 1000.0 * static_cast<double>(i) / kRate);
    const auto echoes = run(*node, click);
    const int latency = node->getLatencySamples();
    const auto window = [&](double t) { return rmsOf(echoes.left, at(t) + static_cast<std::size_t>(latency), at(t + 0.01) + static_cast<std::size_t>(latency)); };
    CHECK(window(0.1) > 0.02);
    CHECK(window(0.2) > 0.005);
    CHECK(window(0.2) < window(0.1));
    CHECK(window(0.05) < window(0.1) * 0.1);

    // The compressor holds loud material down.
    setParameter(*roar, "fbamount", 0.0f);
    setParameter(*roar, "comp", 100.0f);
    run(*node, tone(200.0, at(0.3), 0.9));
    CHECK(roar->takeCompressionDb() > 6.0f);
}

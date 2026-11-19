#include "core/rt/RealtimeGuard.h"
#include "devices/AmpCabinet.h"
#include "devices/Echo.h"
#include "devices/Utility.h"
#include "DeviceTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kPi = 3.14159265358979323846;

Stereo tone(double frequency, std::size_t n, double amplitude = 0.5, double rightFrequency = 0.0, double rightAmplitude = -1.0)
{
    Stereo s(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        s.left[i] = amplitude * std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kRate);
        s.right[i] = (rightAmplitude < 0.0 ? amplitude : rightAmplitude) *
                     std::sin(2.0 * kPi * (rightFrequency > 0.0 ? rightFrequency : frequency) * static_cast<double>(i) / kRate);
    }
    return s;
}

/** Amplitude of `frequency` in `x[begin, end)` (Goertzel). */
double amplitudeAt(const std::vector<double>& x, double frequency, std::size_t begin, std::size_t end)
{
    const double w = 2.0 * kPi * frequency / kRate;
    const double coefficient = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = begin; i < end; ++i)
    {
        const double s0 = x[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    return 2.0 * std::sqrt(std::max(0.0, power)) / static_cast<double>(end - begin);
}

double rmsOf(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
}

double peakOf(const std::vector<double>& x, std::size_t begin = 0, std::size_t end = static_cast<std::size_t>(-1))
{
    double result = 0.0;
    for (std::size_t i = begin; i < std::min(end, x.size()); ++i)
        result = std::max(result, std::abs(x[i]));
    return result;
}

bool allFinite(const Stereo& s)
{
    for (std::size_t i = 0; i < s.left.size(); ++i)
        if (!std::isfinite(s.left[i]) || !std::isfinite(s.right[i]))
            return false;
    return true;
}

Stereo impulse(std::size_t n, double amplitude = 0.1, bool left = true, bool right = true)
{
    Stereo s(n);
    s.left[0] = left ? amplitude : 0.0;
    s.right[0] = right ? amplitude : 0.0;
    return s;
}

std::size_t indexOfPeak(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    std::size_t best = begin;
    for (std::size_t i = begin; i < std::min(end, x.size()); ++i)
        if (std::abs(x[i]) > std::abs(x[best]))
            best = i;
    return best;
}
} // namespace

//==================================================================================================
// Utility

TEST_CASE("Utility gain, balance and mute", "[devices][utility]")
{
    auto [node, utility] = makeNode<Utility>();
    const auto input = tone(1000.0, 9600);

    setParameter(*utility, "gain", 6.0f);
    node->prepare(kRate, kBlock, nullptr);
    auto out = run(*node, input);
    CHECK_THAT(peakOf(out.left, 4800), WithinRel(0.5 * std::pow(10.0, 6.0 / 20.0), 0.01));

    setParameter(*utility, "gain", -35.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = run(*node, input);
    CHECK_THAT(peakOf(out.left, 4800), WithinRel(0.5 * std::pow(10.0, -35.0 / 20.0), 0.02));

    setParameter(*utility, "gain", 0.0f);
    setParameter(*utility, "balance", 100.0f); // hard right: left silent, right untouched
    node->prepare(kRate, kBlock, nullptr);
    out = run(*node, input);
    CHECK(peakOf(out.left, 4800) < 1.0e-6);
    CHECK_THAT(peakOf(out.right, 4800), WithinRel(0.5, 0.01));

    setParameter(*utility, "balance", 0.0f);
    setParameter(*utility, "mute", 1.0f);
    out = run(*node, input); // fades out (no click), then silence
    CHECK(peakOf(out.left, 2400) < 1.0e-6);
    CHECK(std::abs(out.left[0]) < 0.5); // not an instant cut
}

TEST_CASE("Utility width, mono, phase and channel modes", "[devices][utility]")
{
    auto [node, utility] = makeNode<Utility>();
    // Different signals left and right.
    const auto input = tone(500.0, 4800, 0.5, 700.0, 0.3);

    setParameter(*utility, "width", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    auto out = run(*node, input);
    for (std::size_t i = 2400; i < 4800; i += 97)
    {
        CHECK_THAT(out.left[i], WithinAbs(out.right[i], 1.0e-9));
        CHECK_THAT(out.left[i], WithinAbs(0.5 * (input.left[i] + input.right[i]), 1.0e-6));
    }

    setParameter(*utility, "width", 200.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = run(*node, input);
    for (std::size_t i = 2400; i < 4800; i += 97)
    {
        const double mid = 0.5 * (input.left[i] + input.right[i]);
        const double side = 0.5 * (input.left[i] - input.right[i]);
        CHECK_THAT(out.left[i], WithinAbs(mid + 2.0 * side, 1.0e-6));
        CHECK_THAT(out.right[i], WithinAbs(mid - 2.0 * side, 1.0e-6));
    }

    setParameter(*utility, "width", 100.0f);
    setParameter(*utility, "mono", 1.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = run(*node, input);
    CHECK_THAT(out.left[3000], WithinAbs(out.right[3000], 1.0e-9));

    setParameter(*utility, "mono", 0.0f);
    setParameter(*utility, "phasel", 1.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = run(*node, input);
    CHECK_THAT(out.left[3000], WithinAbs(-input.left[3000], 1.0e-9));
    CHECK_THAT(out.right[3000], WithinAbs(input.right[3000], 1.0e-9));

    setParameter(*utility, "phasel", 0.0f);
    setParameter(*utility, "channel", 3.0f); // swap
    out = run(*node, input);
    CHECK_THAT(out.left[3000], WithinAbs(input.right[3000], 1.0e-9));
    CHECK_THAT(out.right[3000], WithinAbs(input.left[3000], 1.0e-9));

    setParameter(*utility, "channel", 1.0f); // left on both sides
    out = run(*node, input);
    CHECK_THAT(out.right[3000], WithinAbs(input.left[3000], 1.0e-9));
    setParameter(*utility, "channel", 2.0f); // right on both sides
    out = run(*node, input);
    CHECK_THAT(out.left[3000], WithinAbs(input.right[3000], 1.0e-9));
}

TEST_CASE("Utility Bass Mono removes low side content and keeps the highs wide", "[devices][utility]")
{
    auto [node, utility] = makeNode<Utility>();
    setParameter(*utility, "bassmono", 1.0f);
    setParameter(*utility, "bassfreq", 200.0f);
    node->prepare(kRate, kBlock, nullptr);

    // Pure side signals: 50 Hz (below) and 3 kHz (above the crossover).
    const std::size_t n = 48'000;
    Stereo low(n), high(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        low.left[i] = 0.5 * std::sin(2.0 * kPi * 50.0 * static_cast<double>(i) / kRate);
        low.right[i] = -low.left[i];
        high.left[i] = 0.5 * std::sin(2.0 * kPi * 3000.0 * static_cast<double>(i) / kRate);
        high.right[i] = -high.left[i];
    }
    const auto lowOut = run(*node, low);
    node->prepare(kRate, kBlock, nullptr);
    const auto highOut = run(*node, high);

    CHECK(amplitudeAt(lowOut.left, 50.0, 24'000, n) < 0.5 * 0.05); // > 26 dB down
    CHECK_THAT(amplitudeAt(highOut.left, 3000.0, 24'000, n), WithinRel(0.5, 0.03));

    // The mid channel is unaffected in level at any frequency (Linkwitz-Riley all-pass).
    node->prepare(kRate, kBlock, nullptr);
    const auto midOut = run(*node, tone(200.0, n));
    CHECK_THAT(amplitudeAt(midOut.left, 200.0, 24'000, n), WithinRel(0.5, 0.02));
}

TEST_CASE("Utility DC filter removes an offset", "[devices][utility]")
{
    auto [node, utility] = makeNode<Utility>();
    setParameter(*utility, "dc", 1.0f);
    node->prepare(kRate, kBlock, nullptr);
    Stereo offset(48'000);
    for (std::size_t i = 0; i < offset.left.size(); ++i)
        offset.left[i] = offset.right[i] = 0.3 + 0.1 * std::sin(2.0 * kPi * 440.0 * static_cast<double>(i) / kRate);
    const auto out = run(*node, offset);
    double mean = 0.0;
    for (std::size_t i = 38'400; i < 48'000; ++i)
        mean += out.left[i];
    mean /= 9600.0;
    CHECK(std::abs(mean) < 0.005);
    CHECK_THAT(amplitudeAt(out.left, 440.0, 38'400, 48'000), WithinRel(0.1, 0.02));
}

//==================================================================================================
// Echo

TEST_CASE("Echo repeats at the left and right times with feedback", "[devices][echo]")
{
    auto [node, echo] = makeNode<Echo>();
    setParameter(*echo, "sync", 0.0f);
    setParameter(*echo, "link", 0.0f);
    setParameter(*echo, "timel", 100.0f);
    setParameter(*echo, "timer", 150.0f);
    setParameter(*echo, "feedback", 50.0f);
    setParameter(*echo, "filter", 0.0f);
    setParameter(*echo, "mix", 100.0f);
    node->prepare(kRate, kBlock, nullptr);

    const auto out = run(*node, impulse(30'000));
    CHECK(indexOfPeak(out.left, 1, 7000) == 4800);
    CHECK(indexOfPeak(out.right, 1, 9000) == 7200);
    CHECK_THAT(out.left[4800], WithinRel(0.1, 0.01));
    CHECK_THAT(out.left[9600] / out.left[4800], WithinRel(0.5, 0.02));
    CHECK_THAT(out.right[14'400] / out.right[7200], WithinRel(0.5, 0.02));
    CHECK(peakOf(out.left, 1, 4790) < 1.0e-9); // nothing before the first echo (100 % wet)
}

TEST_CASE("Echo ping-pong alternates sides and sync follows the tempo", "[devices][echo]")
{
    auto [node, echo] = makeNode<Echo>();
    setParameter(*echo, "mode", 1.0f);
    setParameter(*echo, "sync", 0.0f);
    setParameter(*echo, "timel", 100.0f);
    setParameter(*echo, "feedback", 70.0f);
    setParameter(*echo, "filter", 0.0f);
    setParameter(*echo, "mix", 100.0f);
    node->prepare(kRate, kBlock, nullptr);

    const auto out = run(*node, impulse(20'000, 0.1, true, false));
    CHECK(std::abs(out.left[4800]) > 0.04);
    CHECK(std::abs(out.right[4800]) < 1.0e-9);
    CHECK(std::abs(out.right[9600]) > 0.02);
    CHECK(std::abs(out.left[9600]) < 1.0e-9);
    CHECK(std::abs(out.left[14'400]) > 0.01);

    // Synced: 1/4 at 120 BPM (no play head) = 0.5 s.
    setParameter(*echo, "mode", 0.0f);
    setParameter(*echo, "sync", 1.0f);
    setParameter(*echo, "divl", 8.0f); // "1/4"
    setParameter(*echo, "feedback", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    const auto synced = run(*node, impulse(30'000));
    CHECK(indexOfPeak(synced.left, 1, 30'000) == 24'000);
    CHECK(indexOfPeak(synced.right, 1, 30'000) == 24'000); // linked
}

TEST_CASE("Echo dry/wet, ducking and reverb", "[devices][echo]")
{
    auto [node, echo] = makeNode<Echo>();
    setParameter(*echo, "mix", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    const auto input = tone(300.0, 9600);
    auto out = run(*node, input);
    for (std::size_t i = 0; i < input.left.size(); i += 101)
        CHECK_THAT(out.left[i], WithinAbs(input.left[i], 1.0e-12));

    // Ducking: while the input plays, the echoes are pushed down.
    const auto longTone = tone(300.0, 96'000, 0.3);
    setParameter(*echo, "mix", 100.0f);
    setParameter(*echo, "duck", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    const double plain = rmsOf(run(*node, longTone).left, 48'000, 96'000);
    setParameter(*echo, "duck", 100.0f);
    node->prepare(kRate, kBlock, nullptr);
    const double ducked = rmsOf(run(*node, longTone).left, 48'000, 96'000);
    CHECK(ducked < plain * 0.3);

    // Reverb fills the gaps between echoes.
    setParameter(*echo, "duck", 0.0f);
    setParameter(*echo, "sync", 0.0f);
    setParameter(*echo, "timel", 200.0f);
    setParameter(*echo, "feedback", 0.0f);
    setParameter(*echo, "reverb", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    const double dryGap = rmsOf(run(*node, impulse(24'000)).left, 10'500, 20'000);
    setParameter(*echo, "reverb", 100.0f);
    node->prepare(kRate, kBlock, nullptr);
    const double wetGap = rmsOf(run(*node, impulse(24'000)).left, 10'500, 20'000);
    CHECK(dryGap < 1.0e-6);
    CHECK(wetGap > 1.0e-5);
}

TEST_CASE("Echo stays bounded at full feedback, resonance and drive", "[devices][echo]")
{
    auto [node, echo] = makeNode<Echo>();
    setParameter(*echo, "sync", 0.0f);
    setParameter(*echo, "timel", 20.0f);
    setParameter(*echo, "feedback", 100.0f);
    setParameter(*echo, "res", 100.0f);
    setParameter(*echo, "drive", 100.0f);
    setParameter(*echo, "moddepth", 100.0f);
    setParameter(*echo, "modfilter", 100.0f);
    setParameter(*echo, "wobble", 100.0f);
    setParameter(*echo, "noise", 100.0f);
    setParameter(*echo, "reverb", 100.0f);
    setParameter(*echo, "reverbloc", 2.0f);
    setParameter(*echo, "mix", 100.0f);
    node->prepare(kRate, kBlock, nullptr);

    const auto violationsBefore = rt::getRealtimeViolationCount();
    const auto out = run(*node, tone(220.0, 6 * 48'000, 0.9));
    if (rt::isAllocationTrapInstalled())
        CHECK(rt::getRealtimeViolationCount() == violationsBefore);
    CHECK(allFinite(out));
    // The resonant filter may boost, but the loop never runs away: no growth over time.
    const double early = std::max(peakOf(out.left, 48'000, 2 * 48'000), peakOf(out.right, 48'000, 2 * 48'000));
    const double late = std::max(peakOf(out.left, 5 * 48'000, 6 * 48'000), peakOf(out.right, 5 * 48'000, 6 * 48'000));
    CHECK(late < early * 1.5);
    CHECK(late < 8.0);
}

//==================================================================================================
// Amp & Cabinet

TEST_CASE("Amp & Cabinet: more gain means more distortion", "[devices][amp]")
{
    auto [node, amp] = makeNode<AmpCabinet>();
    setParameter(*amp, "amp", 4.0f); // Lead
    setParameter(*amp, "cabinet", 0.0f);
    const auto input = tone(220.0, 48'000, 0.3);
    const auto distortion = [&](float gain)
    {
        setParameter(*amp, "gain", gain);
        node->prepare(kRate, kBlock, nullptr);
        const auto out = run(*node, input);
        const double fundamental = amplitudeAt(out.left, 220.0, 24'000, 48'000);
        const double total = rmsOf(out.left, 24'000, 48'000) * std::sqrt(2.0);
        return std::sqrt(std::max(0.0, total * total - fundamental * fundamental)) / fundamental;
    };
    const double low = distortion(0.5f);
    const double high = distortion(9.5f);
    CHECK(high > low * 2.0);
    CHECK(high > 0.2);
}

TEST_CASE("Amp & Cabinet: the cabinet rolls off the highs, tone controls work", "[devices][amp]")
{
    auto [node, amp] = makeNode<AmpCabinet>();
    setParameter(*amp, "amp", 0.0f); // Clean
    setParameter(*amp, "gain", 0.0f);
    const auto levels = [&](double frequency)
    {
        node->prepare(kRate, kBlock, nullptr);
        const auto out = run(*node, tone(frequency, 24'000, 0.05));
        return amplitudeAt(out.left, frequency, 12'000, 24'000);
    };

    setParameter(*amp, "cabinet", 0.0f);
    const double openRatio = levels(10'000.0) / levels(1000.0);
    setParameter(*amp, "cabinet", 3.0f); // 4x12
    const double cabRatio = levels(10'000.0) / levels(1000.0);
    CHECK(cabRatio < openRatio * 0.1); // > 20 dB more roll-off

    setParameter(*amp, "cabinet", 0.0f);
    setParameter(*amp, "treble", 0.0f);
    const double dull = levels(4000.0);
    setParameter(*amp, "treble", 10.0f);
    const double bright = levels(4000.0);
    CHECK(bright > dull * 3.0);

    setParameter(*amp, "treble", 5.0f);
    setParameter(*amp, "bass", 0.0f);
    const double thin = levels(80.0);
    setParameter(*amp, "bass", 10.0f);
    const double full = levels(80.0);
    CHECK(full > thin * 3.0);
}

TEST_CASE("Amp & Cabinet: every model is stable, silent in silence, and dry is latency-aligned", "[devices][amp]")
{
    auto [node, amp] = makeNode<AmpCabinet>();
    for (int model = 0; model < AmpCabinet::ampNames().size(); ++model)
        for (int cabinet = 0; cabinet < AmpCabinet::cabinetNames().size(); ++cabinet)
        {
            setParameter(*amp, "amp", static_cast<float>(model));
            setParameter(*amp, "cabinet", static_cast<float>(cabinet));
            setParameter(*amp, "mic", static_cast<float>((model + cabinet) % 3));
            setParameter(*amp, "gain", 10.0f);
            node->prepare(kRate, kBlock, nullptr);
            const auto out = run(*node, tone(110.0, 9600, 1.0));
            CHECK(allFinite(out));
            CHECK(peakOf(out.left) < 2.0);
            const auto silence = run(*node, Stereo(14'400));
            CHECK(peakOf(silence.left, 7200) < 1.0e-3);
        }

    // Dry only: the input, delayed by exactly the reported latency.
    setParameter(*amp, "mix", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    const int latency = amp->getLatencySamples();
    CHECK(latency > 0);
    const auto input = tone(440.0, 4800);
    const auto out = run(*node, input);
    for (std::size_t i = static_cast<std::size_t>(latency); i < input.left.size(); i += 53)
        CHECK_THAT(out.left[i], WithinAbs(input.left[i - static_cast<std::size_t>(latency)], 1.0e-12));
}

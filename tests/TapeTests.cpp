#include "devices/TapeA800.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using devices::TapeA800;

namespace
{
double level(double db)
{
    return std::pow(10.0, db / 20.0);
}

/** Steady-state gain (dB) of a tone through a fresh machine set up by `setup`. */
template <typename Setup>
double gainDb(double frequency, double levelDb, Setup&& setup)
{
    auto [node, tape] = makeNode<TapeA800>();
    setup(*tape);
    const auto n = at(1.0);
    const auto in = tone(frequency, n, level(levelDb));
    const auto out = run(*node, in);
    return toDb(amplitudeAt(out.left, frequency, n / 2, n) / amplitudeAt(in.left, frequency, n / 2, n));
}

/** THD (%) of a 1 kHz tone (harmonics 2 to 7). */
template <typename Setup>
double thd(double levelDb, Setup&& setup)
{
    auto [node, tape] = makeNode<TapeA800>();
    setup(*tape);
    const auto n = at(1.0);
    const auto out = run(*node, tone(1000.0, n, level(levelDb)));
    double harmonics = 0.0;
    for (int k = 2; k <= 7; ++k)
        harmonics += std::pow(amplitudeAt(out.left, 1000.0 * k, n / 2, n), 2.0);
    return 100.0 * std::sqrt(harmonics) / amplitudeAt(out.left, 1000.0, n / 2, n);
}

auto withSettings(std::initializer_list<std::pair<const char*, float>> settings)
{
    std::vector<std::pair<juce::String, float>> list;
    for (const auto& [id, value] : settings)
        list.emplace_back(id, value);
    return [list](TapeA800& tape)
    {
        for (const auto& [id, value] : list)
            setParameter(tape, id, value);
    };
}
} // namespace

TEST_CASE("Tape: the reproduce response at each speed follows the machine's specification", "[devices][tape]")
{
    struct Spec
    {
        float speed;
        double low, high, tolerance;
    };
    // 30 ips: ~50 Hz - 23 kHz (+-1 dB); 15 ips: ~25 Hz - 22 kHz (+-1 dB); 7.5 ips: ~30 Hz - 16 kHz (+-1.5 dB).
    for (const auto& spec : {Spec{2.0f, 50.0, 23000.0, 1.0}, Spec{1.0f, 25.0, 22000.0, 1.0}, Spec{0.0f, 30.0, 16000.0, 1.5}})
    {
        INFO("speed " << spec.speed);
        const auto setup = withSettings({{"speed", spec.speed}});
        for (double f = spec.low; f <= spec.high * 1.001; f *= 1.6)
        {
            INFO(f << " Hz");
            CHECK_THAT(gainDb(f, -40.0, setup), WithinAbs(0.0, spec.tolerance));
        }
        CHECK_THAT(gainDb(spec.high, -40.0, setup), WithinAbs(0.0, spec.tolerance));
        // ... and it falls away outside.
        CHECK(gainDb(spec.low * 0.6, -40.0, setup) < -1.5);
    }
    CHECK(gainDb(22000.0, -40.0, withSettings({{"speed", 0.0f}})) < -2.0);
    // The head bump: a little low-end lift at 15 ips.
    CHECK(gainDb(55.0, -40.0, withSettings({})) > 0.4);
}

TEST_CASE("Tape: EQ standards, pre-emphasis and where the tape saturates first", "[devices][tape]")
{
    const auto aes = TapeA800::emphasisOf(2, 0);
    CHECK(juce::String(aes.name) == "AES");
    CHECK_THAT(aes.highSeconds, WithinAbs(35.0e-6, 1e-12));
    CHECK(aes.lowSeconds == 0.0);
    for (const int speed : {0, 1})
    {
        const auto nab = TapeA800::emphasisOf(speed, 0);
        CHECK(juce::String(nab.name) == "NAB");
        CHECK_THAT(nab.highSeconds, WithinAbs(50.0e-6, 1e-12));
        CHECK_THAT(nab.lowSeconds, WithinAbs(3180.0e-6, 1e-12));
        CHECK(juce::String(TapeA800::emphasisOf(speed, 1).name) == "CCIR");
    }
    CHECK_THAT(TapeA800::emphasisOf(1, 1).highSeconds, WithinAbs(35.0e-6, 1e-12));
    CHECK_THAT(TapeA800::emphasisOf(0, 1).highSeconds, WithinAbs(70.0e-6, 1e-12));

    // Small signals: record and playback EQ cancel, NAB and CCIR alike.
    for (const double f : {30.0, 1000.0, 10000.0})
    {
        INFO(f);
        CHECK_THAT(gainDb(f, -40.0, withSettings({{"eq", 0.0f}})), WithinAbs(gainDb(f, -40.0, withSettings({{"eq", 1.0f}})), 0.1));
    }
    // Loud: the pre-emphasised highs saturate first, and with NAB the deep lows too.
    CHECK(gainDb(10000.0, -6.0, withSettings({})) < gainDb(1000.0, -6.0, withSettings({})) - 3.0);
    CHECK(gainDb(30.0, -6.0, withSettings({{"eq", 0.0f}})) < gainDb(30.0, -6.0, withSettings({{"eq", 1.0f}})) - 1.5);
    // At 30 ips the standard is AES whatever the switch says.
    CHECK_THAT(gainDb(10000.0, -6.0, withSettings({{"speed", 2.0f}, {"eq", 0.0f}})),
               WithinAbs(gainDb(10000.0, -6.0, withSettings({{"speed", 2.0f}, {"eq", 1.0f}})), 1e-9));
}

TEST_CASE("Tape: the Input, Sync, Repro and Thru signal paths", "[devices][tape]")
{
    // Input: the electronics only: unity at 0 VU, flat, clean.
    const auto inputPath = withSettings({{"path", 0.0f}});
    CHECK_THAT(gainDb(1000.0, -18.0, inputPath), WithinAbs(0.0, 0.05));
    CHECK_THAT(gainDb(20000.0, -40.0, inputPath), WithinAbs(0.0, 0.3));
    CHECK_THAT(gainDb(20.0, -40.0, inputPath), WithinAbs(0.0, 0.3));
    CHECK(thd(-6.0, inputPath) < 0.1);
    // Repro: tape at 0 VU colours a little; Sync (off the record head) is darker.
    const double reproThd = thd(-18.0, withSettings({}));
    CHECK(reproThd > 0.3);
    CHECK(reproThd < 2.0);
    CHECK(gainDb(16000.0, -40.0, withSettings({{"path", 1.0f}})) < gainDb(16000.0, -40.0, withSettings({})) - 2.0);
    CHECK_THAT(gainDb(1000.0, -40.0, withSettings({{"path", 1.0f}})), WithinAbs(0.0, 0.1));

    // Thru: the input, untouched, delayed by the latency only.
    {
        auto [node, tape] = makeNode<TapeA800>();
        setParameter(*tape, "path", 3.0f);
        setParameter(*tape, "input", 12.0f); // (bypassed too)
        const auto latency = static_cast<std::size_t>(tape->getLatencySamples());
        const auto in = sum(tone(97.0, at(0.3), 0.7), tone(13000.0, at(0.3), 0.2));
        const auto out = run(*node, in);
        double error = 0.0;
        for (std::size_t i = at(0.05); i < in.left.size(); ++i) // (after the crossfade from the default path)
            error = std::max(error, std::abs(out.left[i] - in.left[i - latency]));
        CHECK(error < 1e-12);
    }

    // The output stage: +24 dBm into 600 ohms, +2 dBFS: pushed hard, it rounds off there.
    {
        auto [node, tape] = makeNode<TapeA800>();
        setParameter(*tape, "path", 0.0f);
        setParameter(*tape, "output", 12.0f);
        const auto out = run(*node, tone(220.0, at(0.3), level(-3.0)));
        CHECK(peakOf(out.left, at(0.1)) > 1.0);
        CHECK(peakOf(out.left, at(0.1)) < level(TapeA800::kOutputClipDbfs + 1.0));
    }
}

TEST_CASE("Tape: formulas, calibration, input level and bias shape the saturation", "[devices][tape]")
{
    // More level, more distortion.
    CHECK(thd(-24.0, withSettings({})) < thd(-12.0, withSettings({})));
    CHECK(thd(-12.0, withSettings({})) < thd(-6.0, withSettings({})));
    // About 3 % THD at the formula's maximum output level (Classic, +6 dB: 5 dB over 0 VU).
    CHECK_THAT(thd(-13.0, withSettings({})), WithinAbs(3.0, 1.2));
    // Formulas: Vintage saturates first, Mastering last.
    double previous = 1.0e9;
    for (int formula = 0; formula < 4; ++formula)
    {
        INFO(TapeA800::formulaOf(formula).name);
        const double distortion = thd(-12.0, withSettings({{"formula", static_cast<float>(formula)}}));
        CHECK(distortion < previous);
        previous = distortion;
    }
    // Hotter calibration and more input: harder on the tape.
    CHECK(thd(-18.0, withSettings({{"cal", 2.0f}})) > thd(-18.0, withSettings({{"cal", 0.0f}})) * 2.0);
    CHECK(thd(-18.0, withSettings({{"input", 6.0f}})) > thd(-18.0, withSettings({})) * 2.0);

    // Bias: over is darker and cleaner, under brighter and dirtier.
    const double optimal = gainDb(10000.0, -40.0, withSettings({}));
    CHECK(gainDb(10000.0, -40.0, withSettings({{"bias", 3.0f}})) < optimal - 2.0);
    CHECK(gainDb(10000.0, -40.0, withSettings({{"bias", -3.0f}})) > optimal + 0.5);
    CHECK(thd(-15.0, withSettings({{"bias", -3.0f}})) > thd(-15.0, withSettings({})) * 1.5);
    CHECK(thd(-15.0, withSettings({{"bias", 3.0f}})) < thd(-15.0, withSettings({})));

    // Repro HF / LF fine trims.
    CHECK_THAT(gainDb(15000.0, -40.0, withSettings({{"hf", 3.0f}})) - gainDb(15000.0, -40.0, withSettings({})), WithinAbs(2.8, 0.4));
    CHECK_THAT(gainDb(40.0, -40.0, withSettings({{"lf", -3.0f}})) - gainDb(40.0, -40.0, withSettings({})), WithinAbs(-2.8, 0.4));
}

TEST_CASE("Tape: hiss from the tape when Noise is on", "[devices][tape]")
{
    const auto hiss = [](float speed, float formula)
    {
        auto [node, tape] = makeNode<TapeA800>();
        setParameter(*tape, "noise", 1.0f);
        setParameter(*tape, "speed", speed);
        setParameter(*tape, "formula", formula);
        const auto out = run(*node, Stereo(at(1.0)));
        return toDb(rmsOf(out.left, at(0.2), at(1.0)));
    };
    const double standard = hiss(1.0f, 1.0f);
    CHECK(standard > -95.0);
    CHECK(standard < -80.0);
    CHECK(hiss(0.0f, 1.0f) > hiss(2.0f, 1.0f) + 3.0); // slower tape, more hiss
    CHECK(hiss(1.0f, 0.0f) > hiss(1.0f, 3.0f) + 4.0); // older formula, more hiss
    // Off: silence stays silence; the Input path has no tape hiss either.
    {
        auto [node, tape] = makeNode<TapeA800>();
        const auto out = run(*node, Stereo(at(0.5)));
        CHECK(peakOf(out.left) == 0.0);
    }
    {
        auto [node, tape] = makeNode<TapeA800>();
        setParameter(*tape, "noise", 1.0f);
        setParameter(*tape, "path", 0.0f);
        const auto out = run(*node, Stereo(at(0.5)));
        CHECK(peakOf(out.left, at(0.05)) < 1e-12); // (after the crossfade from the default path)
    }
}

TEST_CASE("Tape: Auto Input follows the transport; path changes crossfade without clicks", "[devices][tape]")
{
    // Auto Input: stopped, the tape paths play Input; rolling, Repro.
    for (const bool playing : {false, true})
    {
        auto [node, tape] = makeNode<TapeA800>();
        setParameter(*tape, "autoinput", 1.0f);
        const auto n = at(1.0);
        const auto in = tone(10000.0, n, level(-6.0));
        const auto out = runPlaying(*node, *tape, in, 120.0, playing, [](std::size_t) {});
        const double gain = toDb(amplitudeAt(out.left, 10000.0, n / 2, n) / amplitudeAt(in.left, 10000.0, n / 2, n));
        CHECK(tape->getActivePath() == (playing ? TapeA800::repro : TapeA800::input));
        CHECK(tape->isRolling() == playing);
        if (playing)
            CHECK(gain < -3.0); // (the tape's HF saturation)
        else
            CHECK_THAT(gain, WithinAbs(0.0, 0.2));
    }

    // Switching path while playing a tone: no jump bigger than the tone's own steps.
    const auto biggestStep = [](float from, float to)
    {
        auto [node, tape] = makeNode<TapeA800>();
        setParameter(*tape, "path", from);
        const auto in = tone(200.0, at(0.6), level(-12.0));
        std::size_t done = 0;
        const auto out = runPlaying(*node, *tape, in, 120.0, true,
                                    [&](std::size_t samples)
                                    {
                                        done = samples;
                                        if (done >= at(0.3) && done < at(0.3) + static_cast<std::size_t>(kBlock))
                                            setParameter(*tape, "path", to);
                                    });
        double step = 0.0;
        for (std::size_t i = at(0.2); i < out.left.size(); ++i)
            step = std::max(step, std::abs(out.left[i] - out.left[i - 1]));
        return step;
    };
    const double toneStep = 2.0 * 3.14159265358979 * 200.0 / kRate * level(-12.0);
    for (const auto& [from, to] : {std::pair{2.0f, 0.0f}, std::pair{0.0f, 3.0f}, std::pair{3.0f, 1.0f}, std::pair{1.0f, 2.0f}})
    {
        INFO(from << " -> " << to);
        CHECK(biggestStep(from, to) < toneStep * 1.6);
    }
}

TEST_CASE("Tape: machines in a group move together", "[devices][tape]")
{
    juce::MessageManager::getInstance();
    TapeA800 a, b, c, d;
    setParameter(a, "group", 1.0f);
    setParameter(b, "group", 1.0f);
    setParameter(a, "speed", 2.0f);
    CHECK(b.speed_->getIndex() == 2);
    setParameter(b, "bias", 1.5f);
    CHECK_THAT(a.bias_->get(), WithinAbs(1.5, 1e-4));
    CHECK(c.speed_->getIndex() == 1); // (not in the group)

    // Joining a group takes its settings.
    setParameter(d, "formula", 3.0f);
    setParameter(d, "group", 1.0f);
    CHECK(d.speed_->getIndex() == 2);
    CHECK(d.formula_->getIndex() == a.formula_->getIndex());

    // Leaving it: changes stay local.
    setParameter(b, "group", 0.0f);
    setParameter(b, "speed", 0.0f);
    CHECK(a.speed_->getIndex() == 2);

    // Loading a saved machine does not pass its settings to the group.
    juce::MemoryBlock state;
    c.getStateInformation(state); // (speed 15 ips, no group)
    setParameter(c, "group", 1.0f); // (joins: takes 30 ips)
    CHECK(c.speed_->getIndex() == 2);
    TapeA800 e;
    setParameter(e, "group", 1.0f);
    e.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(a.speed_->getIndex() == 2);
}

TEST_CASE("Tape: oversampling latency, in real time and offline", "[devices][tape]")
{
    auto [node, tape] = makeNode<TapeA800>();
    const int realtime = tape->getLatencySamples();
    CHECK(realtime == tape->latencyFor(false));
    CHECK(realtime > 0);
    CHECK(realtime < 10); // (minimum-phase filters)
    tape->setNonRealtime(true);
    CHECK(tape->getLatencySamples() == tape->latencyFor(true));
    tape->setNonRealtime(false);
    setParameter(*tape, "osrealtime", 0.0f);
    CHECK(tape->getLatencySamples() == 0);
    // Without oversampling it still meets the specification at 15 ips.
    const auto plain = withSettings({{"osrealtime", 0.0f}});
    for (const double f : {25.0, 1000.0, 16000.0})
    {
        INFO(f);
        CHECK_THAT(gainDb(f, -40.0, plain), WithinAbs(0.0, 1.0));
    }
}

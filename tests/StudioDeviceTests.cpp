#include "devices/PlateReverb.h"
#include "devices/DeviceRegistry.h"
#include "devices/StudioDynamics.h"
#include "devices/dsp/PhaseOversampler.h"
#include "ui/devices/StudioDeviceEditors.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numeric>

using namespace dojo;
using namespace dojo::test::devices;
namespace dsp = dojo::devices::dsp;
using Catch::Matchers::WithinAbs;

namespace
{
double level(double db)
{
    return std::pow(10.0, db / 20.0);
}

/** Steady-state gain change (dB) of a tone through a fresh device set up by `setup`. */
template <typename Device, typename Setup>
double toneGainDb(double frequency, double levelDb, Setup&& setup, double seconds = 1.0)
{
    auto [node, device] = makeNode<Device>();
    setup(*device);
    const auto n = at(seconds);
    const auto in = tone(frequency, n, level(levelDb));
    const auto out = run(*node, in);
    return toDb(amplitudeAt(out.left, frequency, n / 2, n) / amplitudeAt(in.left, frequency, n / 2, n));
}

Stereo whiteNoise(std::size_t n, double amplitude, std::uint32_t seed = 3)
{
    Stereo s(n);
    std::uint32_t state = seed;
    for (std::size_t i = 0; i < n; ++i)
    {
        state = state * 1664525u + 1013904223u;
        s.left[i] = amplitude * (static_cast<double>(state) / 4294967295.0 * 2.0 - 1.0);
        state = state * 1664525u + 1013904223u;
        s.right[i] = amplitude * (static_cast<double>(state) / 4294967295.0 * 2.0 - 1.0);
    }
    return s;
}
} // namespace

//==================================================================================================
TEST_CASE("Phase oversampler: every factor and phase has a whole-sample latency that lines up", "[devices][oversampler]")
{
    using devices::dsp::PhaseOversampler;
    PhaseOversampler oversampler;
    oversampler.prepare(kBlock);
    CHECK(oversampler.latencyOf(0, 2) == 0);
    for (int factor = 1; factor < PhaseOversampler::kFactorChoices; ++factor)
    {
        INFO("factor " << factor);
        // Minimum phase is the shortest, linear phase the longest.
        CHECK(oversampler.latencyOf(factor, 0) < oversampler.latencyOf(factor, 1));
        CHECK(oversampler.latencyOf(factor, 1) < oversampler.latencyOf(factor, 2));
    }

    /** The error (dB under the signal) of the output against the input delayed by the latency. */
    const auto alignmentErrorDb = [](int factor, int phase, double frequency)
    {
        PhaseOversampler o;
        o.prepare(kBlock);
        o.select(factor, phase);
        const auto n = at(0.5);
        const auto in = tone(frequency, n, 0.5);
        std::vector<double> out(n), dryOut(n);
        juce::AudioBuffer<double> buffer(2, kBlock);
        for (std::size_t offset = 0; offset < n; offset += kBlock)
        {
            const int count = static_cast<int>(std::min<std::size_t>(kBlock, n - offset));
            for (int i = 0; i < count; ++i)
                buffer.setSample(0, i, in.left[offset + static_cast<std::size_t>(i)]), buffer.setSample(1, i, in.right[offset + static_cast<std::size_t>(i)]);
            o.process(buffer.getArrayOfWritePointers(), count, [](double**, std::size_t) {});
            for (int i = 0; i < count; ++i)
            {
                out[offset + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
                dryOut[offset + static_cast<std::size_t>(i)] = o.dry(0, i);
            }
        }
        const auto latency = static_cast<std::size_t>(o.getLatency());
        double error = 0.0, dryError = 0.0;
        for (std::size_t i = at(0.1); i < n; ++i)
        {
            error = std::max(error, std::abs(out[i] - in.left[i - latency]));
            dryError = std::max(dryError, std::abs(dryOut[i] - in.left[i - latency]));
        }
        CHECK(dryError < 1e-12); // the dry copy is delayed by exactly the latency
        return toDb(error / 0.5);
    };
    for (int factor = 1; factor < PhaseOversampler::kFactorChoices; ++factor)
        for (int phase = 0; phase < PhaseOversampler::kPhaseChoices; ++phase)
        {
            INFO("factor " << factor << ", phase " << phase);
            // At low frequencies every setting lines up with its reported latency.
            CHECK(alignmentErrorDb(factor, phase, 60.0) < -35.0);
        }
    // Upper frequencies: linear phase stays aligned, minimum phase bends.
    CHECK(alignmentErrorDb(1, 2, 10000.0) < -30.0);
    CHECK(alignmentErrorDb(1, 0, 10000.0) > -20.0);
}

//==================================================================================================
TEST_CASE("The Glue: a feedback bus compressor with stepped ratios, attack and release", "[devices][glue]")
{
    using devices::TheGlue;
    const auto setUp = [](TheGlue& c, float threshold, int ratio, int attack = 0, int release = 0)
    {
        setParameter(c, "threshold", threshold);
        setParameter(c, "ratio", static_cast<float>(ratio));
        setParameter(c, "attack", static_cast<float>(attack));
        setParameter(c, "release", static_cast<float>(release));
    };
    CHECK(devices::findBuiltinDevice(TheGlue::kTypeId)->name == "The Glue");
    {
        TheGlue glue;
        CHECK(glue.getName() == "The Glue");
    }

    // The ratios: above the threshold the output rises by 1/ratio of the input (2:1 and 4:1, well
    // over the knee).
    for (const auto& [ratio, slope] : {std::pair{0, 0.5}, std::pair{1, 0.25}})
    {
        INFO("ratio " << ratio);
        const double low = toneGainDb<TheGlue>(1000.0, -25.0, [&](auto& c) { setUp(c, -40.0f, ratio); }, 1.5);
        const double high = toneGainDb<TheGlue>(1000.0, -5.0, [&](auto& c) { setUp(c, -40.0f, ratio); }, 1.5);
        CHECK_THAT((20.0 + high - low) / 20.0, WithinAbs(slope, 0.02));
    }
    // The static curve (feedback: the detector hears the compressed signal) predicts the level.
    for (int ratio = 0; ratio < 3; ++ratio)
        for (const double input : {-20.0, -10.0, -3.0})
        {
            INFO("ratio " << ratio << ", input " << input);
            TheGlue reference;
            setUp(reference, -30.0f, ratio);
            const double expected = reference.staticOutputDb(input) - input;
            CHECK_THAT(toneGainDb<TheGlue>(1000.0, input, [&](auto& c) { setUp(c, -30.0f, ratio); }, 1.5), WithinAbs(expected, 0.6));
        }
    // Under the threshold: untouched.
    CHECK_THAT(toneGainDb<TheGlue>(1000.0, -40.0, [&](auto& c) { setUp(c, -30.0f, 1); }), WithinAbs(0.0, 0.05));

    // The fastest attack at 10:1: the loop is solved each sample, so a sudden loud tone neither
    // overshoots (no dip under the settled level) nor chatters.
    {
        auto [node, glue] = makeNode<TheGlue>();
        setUp(*glue, -30.0f, 2, 0, 0);
        const auto in = concat({tone(1000.0, at(0.2), level(-40.0)), tone(1000.0, at(0.5), level(-6.0))});
        const auto out = run(*node, in);
        const double settled = rmsOf(out.left, at(0.5), at(0.7));
        for (double t = 0.21; t < 0.68; t += 0.01)
        {
            INFO("t " << t);
            CHECK(toDb(rmsOf(out.left, at(t), at(t + 0.01)) / settled) > -1.0);
        }
    }
    // Attack: a slower attack lets more of the start of a hit through.
    {
        const auto onset = [&](int attack)
        {
            auto [node, glue] = makeNode<TheGlue>();
            setUp(*glue, -30.0f, 2, attack, 0);
            const auto in = concat({Stereo(at(0.1)), tone(1000.0, at(0.2), level(-6.0))});
            const auto out = run(*node, in);
            return peakOf(out.left, at(0.1), at(0.105));
        };
        CHECK(toDb(onset(6) / onset(0)) > 6.0);
    }
    // Release: after a loud passage, a quiet tone comes back up quickly at 0.1 s, slowly at 1.2 s.
    const auto recoveryDb = [&](int release, double loudSeconds)
    {
        auto [node, glue] = makeNode<TheGlue>();
        setUp(*glue, -30.0f, 1, 3, release);
        const auto in = concat({tone(1000.0, at(loudSeconds), level(-6.0)), tone(1000.0, at(0.6), level(-40.0))});
        const auto out = run(*node, in);
        const auto from = at(loudSeconds + 0.45);
        return toDb(rmsOf(out.left, from, from + at(0.05)) / rmsOf(in.left, from, from + at(0.05)));
    };
    const double fast = recoveryDb(0, 1.0), slow = recoveryDb(5, 1.0);
    CHECK(fast > -0.5);
    CHECK(slow < -2.0);
    // Auto: fast after a short burst, slow after sustained compression.
    const double shortBurst = recoveryDb(6, 0.01), sustained = recoveryDb(6, 2.0);
    CHECK(shortBurst > -1.5);
    CHECK(sustained < shortBurst - 6.0);
}

TEST_CASE("The Glue: range, sidechain high pass, external sidechain, mix in dB and peak clip", "[devices][glue]")
{
    using devices::TheGlue;
    const auto loud = [](TheGlue& c)
    {
        setParameter(c, "threshold", -30.0f);
        setParameter(c, "ratio", 2.0f); // 10:1
        setParameter(c, "attack", 2.0f);
    };

    // Range saturates the sidechain: at most that much compression, however loud.
    {
        const double full = toneGainDb<TheGlue>(1000.0, 0.0, loud);
        CHECK(full < -20.0);
        const double six = toneGainDb<TheGlue>(1000.0, 0.0, [&](auto& c) { loud(c); setParameter(c, "range", 6.0f); });
        CHECK(six > -6.3);
        CHECK(six < -3.5);
        // ... and well within the range nothing changes (4:1, Range 12 dB, about 4 dB of compression).
        const auto four = [&](TheGlue& c)
        {
            loud(c);
            setParameter(c, "ratio", 1.0f);
        };
        CHECK_THAT(toneGainDb<TheGlue>(1000.0, -24.0, [&](auto& c) { four(c); setParameter(c, "range", 12.0f); }),
                   WithinAbs(toneGainDb<TheGlue>(1000.0, -24.0, four), 0.2));
        TheGlue glue;
        CHECK(glue.getParameterById("range")->getCurrentValueAsText() == "Full");
    }

    // Sidechain high pass (-6 dB/oct): bass pumps less.
    {
        const double open = toneGainDb<TheGlue>(50.0, -6.0, loud);
        const double filtered = toneGainDb<TheGlue>(50.0, -6.0, [&](auto& c) { loud(c); setParameter(c, "schp", 400.0f); });
        CHECK(filtered > open + 6.0);
        // (a gentle slope: an octave above the corner is barely affected)
        const double above = toneGainDb<TheGlue>(1600.0, -6.0, [&](auto& c) { loud(c); setParameter(c, "schp", 400.0f); });
        CHECK_THAT(above, WithinAbs(toneGainDb<TheGlue>(1600.0, -6.0, loud), 1.5));
        TheGlue glue;
        CHECK(glue.getParameterById("schp")->getCurrentValueAsText() == "Off");
    }

    // External sidechain: the key drives the compression (through the same feedback loop).
    {
        auto [node, glue] = makeNode<TheGlue>();
        loud(*glue);
        setParameter(*glue, "sidechain", 1.0f);
        const auto n = at(1.0);
        const auto in = tone(1000.0, n, level(-40.0));
        const auto key = tone(200.0, n, level(-6.0));
        const auto out = run(*node, in, &key);
        CHECK(toDb(amplitudeAt(out.left, 1000.0, n / 2, n) / amplitudeAt(in.left, 1000.0, n / 2, n)) < -10.0);
    }

    // Mix: shown in dB (the compressed signal's level), typed in dB or %; parallel compression.
    CHECK(TheGlue::mixText(100.0f) == "0.0 dB");
    CHECK(TheGlue::mixText(50.0f) == "-6.0 dB");
    CHECK(TheGlue::mixText(0.0f) == "-inf dB");
    CHECK_THAT(TheGlue::mixFromText("-6 dB"), WithinAbs(50.1f, 0.1f));
    CHECK(TheGlue::mixFromText("-inf") == 0.0f);
    CHECK(TheGlue::mixFromText("25 %") == 25.0f);
    {
        // Mix at -inf: the dry signal, exactly (no oversampling) or delayed by the latency.
        auto [node, glue] = makeNode<TheGlue>();
        loud(*glue);
        setParameter(*glue, "mix", 0.0f);
        setParameter(*glue, "osrealtime", 0.0f);
        const auto in = tone(700.0, at(0.3), level(-3.0));
        CHECK(run(*node, in).left == in.left);
    }
    // Half mix, heavy compression, linear-phase oversampling: the dry and the compressed halves
    // are in phase at every frequency (no comb filtering).
    {
        const auto halfMix = [&](double frequency)
        {
            return toneGainDb<TheGlue>(frequency, -6.0, [&](auto& c)
                                       {
                                           loud(c);
                                           setParameter(c, "mix", 50.0f);
                                           setParameter(c, "osrealtime", 2.0f);
                                       });
        };
        const double reference = halfMix(1000.0);
        CHECK(reference < -4.0);
        for (const double f : {3000.0, 8000.0, 15000.0})
        {
            INFO(f);
            CHECK_THAT(halfMix(f), WithinAbs(reference, 0.4));
        }
    }

    // Peak Clip: soft clipping just under 0 dBFS after the makeup.
    for (const int factor : {0, 1, 3})
    {
        INFO("oversampling " << factor);
        const auto peak = [&](bool clip)
        {
            auto [node, glue] = makeNode<TheGlue>();
            setParameter(*glue, "threshold", 0.0f);
            setParameter(*glue, "makeup", 12.0f);
            setParameter(*glue, "peakclip", clip ? 1.0f : 0.0f);
            setParameter(*glue, "osrealtime", static_cast<float>(factor));
            return peakOf(run(*node, sum(tone(110.0, at(0.4), 0.35), tone(2500.0, at(0.4), 0.2))).left, at(0.1));
        };
        CHECK(peak(false) > 1.5);
        CHECK(peak(true) < 1.0);
        CHECK(peak(true) > TheGlue::kPeakClipKnee);
    }
    // ... and oversampling keeps its harmonics from aliasing: a loud 15 kHz tone clipped at the
    // base rate folds a 45 kHz harmonic down to 3 kHz; at 8x it is far lower.
    const auto alias = [&](int factor)
    {
        auto [node, glue] = makeNode<TheGlue>();
        setParameter(*glue, "threshold", 0.0f);
        setParameter(*glue, "makeup", 12.0f);
        setParameter(*glue, "peakclip", 1.0f);
        setParameter(*glue, "osrealtime", static_cast<float>(factor));
        const auto n = at(0.5);
        const auto out = run(*node, tone(15000.0, n, 0.5));
        return toDb(amplitudeAt(out.left, 3000.0, n / 2, n));
    };
    CHECK(alias(3) < alias(0) - 20.0);
}

TEST_CASE("The Glue: separate real-time and offline oversampling, each with an integer latency", "[devices][glue]")
{
    using devices::TheGlue;
    auto [node, glue] = makeNode<TheGlue>();
    // Defaults: 2x in real time, 8x offline, linear phase.
    CHECK(glue->osRealtime_->getCurrentChoiceName() == "2x");
    CHECK(glue->osOffline_->getCurrentChoiceName() == "8x");
    CHECK(glue->osPhase_->getCurrentChoiceName() == "Linear Phase");
    const int realtime = glue->getLatencySamples();
    CHECK(realtime == glue->latencyFor(false));
    CHECK(realtime > 0);
    glue->setNonRealtime(true);
    CHECK(glue->getLatencySamples() == glue->latencyFor(true));
    CHECK(glue->getLatencySamples() > realtime);
    glue->setNonRealtime(false);
    CHECK(glue->getLatencySamples() == realtime);
    // Phase: minimum < intermediate < linear; Off: none (and the lowest CPU).
    setParameter(*glue, "osphase", 0.0f);
    const int minimum = glue->getLatencySamples();
    setParameter(*glue, "osphase", 1.0f);
    const int intermediate = glue->getLatencySamples();
    CHECK(minimum < intermediate);
    CHECK(intermediate < realtime);
    setParameter(*glue, "osrealtime", 0.0f);
    CHECK(glue->getLatencySamples() == 0);

    // The output (under the threshold) is the input delayed by exactly the reported latency.
    setParameter(*glue, "osrealtime", 2.0f);
    setParameter(*glue, "osphase", 2.0f);
    const auto latency = static_cast<std::size_t>(glue->getLatencySamples());
    const auto in = tone(100.0, at(0.5), level(-30.0));
    const auto out = run(*node, in);
    double error = 0.0;
    for (std::size_t i = at(0.1); i < in.left.size(); ++i)
        error = std::max(error, std::abs(out.left[i] - in.left[i - latency]));
    CHECK(error < level(-30.0) * 0.01);
}

TEST_CASE("The Glue's VU meter: RMS needle, click for the peak needle, hover for the numbers", "[devices][glue][ui]")
{
    using devices::TheGlue;
    juce::MessageManager::getInstance();
    auto [node, glue] = makeNode<TheGlue>();
    setParameter(*glue, "threshold", -30.0f);
    setParameter(*glue, "release", 0.0f); // 0.1 s
    ui::GlueVuMeter vu(*glue);
    vu.setSize(180, 120);
    CHECK_FALSE(vu.isPeakVisible());

    // A loud passage: both needles move; the peak reads at least the RMS.
    run(*node, tone(1000.0, at(0.6), level(-6.0)));
    vu.update(1.0 / 30.0);
    CHECK(vu.getRmsDb() > 5.0);
    CHECK(vu.getPeakDb() >= vu.getRmsDb() - 0.5);
    CHECK(vu.getReadout().contains("RMS " + juce::String(vu.getRmsDb(), 1) + " dB"));
    CHECK(vu.getReadout().contains("PEAK"));

    // Click: the peak needle shows; click again: it hides.
    const auto click = [&]
    {
        const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
        const juce::MouseEvent event(mouse, {90.0f, 60.0f}, {}, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &vu, &vu, juce::Time::getCurrentTime(), {90.0f, 60.0f},
                                     juce::Time::getCurrentTime(), 1, false);
        vu.mouseDown(event);
    };
    click();
    CHECK(vu.isPeakVisible());
    {
        // The choice stays with the device: a new meter (another page, the editor reopened) and a
        // saved copy show the peak needle too.
        ui::GlueVuMeter other(*glue);
        CHECK(other.isPeakVisible());
        juce::MemoryBlock state;
        glue->getStateInformation(state);
        TheGlue copy;
        copy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        CHECK(copy.isPeakNeedleShown());
    }
    click();
    CHECK_FALSE(vu.isPeakVisible());

    // The peak holds briefly, then falls; the RMS needle follows the signal.
    const double held = vu.getPeakDb();
    run(*node, tone(1000.0, at(1.0), level(-60.0)));
    vu.update(0.3);
    CHECK_THAT(vu.getPeakDb(), WithinAbs(held, 1e-9)); // (within the hold)
    vu.update(0.4);
    vu.update(0.2);
    CHECK(vu.getPeakDb() < held - 2.0);
    CHECK(vu.getRmsDb() < 3.0);

    // It paints (both needles).
    vu.setPeakVisible(true);
    const auto image = vu.createComponentSnapshot(vu.getLocalBounds());
    CHECK(image.isValid());
}

//==================================================================================================
TEST_CASE("De-Esser: above or band, absolute or relative detection, split or wide", "[devices][deesser]")
{
    using devices::DeEsser;
    const auto defaults = [](DeEsser&) {};
    // Above (default, 6 kHz): a loud "s" at 12 kHz comes down by about the range (8 dB).
    const double sibilance = toneGainDb<DeEsser>(12000.0, -6.0, defaults);
    CHECK(sibilance < -6.5);
    CHECK(sibilance > -8.5);
    // The body of the voice as loud, and quiet air: untouched.
    CHECK_THAT(toneGainDb<DeEsser>(1000.0, -6.0, defaults), WithinAbs(0.0, 0.2));
    CHECK_THAT(toneGainDb<DeEsser>(12000.0, -40.0, defaults), WithinAbs(0.0, 0.2));
    // Range limits it; Threshold sets where it starts.
    CHECK(toneGainDb<DeEsser>(12000.0, -6.0, [](auto& d) { setParameter(d, "range", 3.0f); }) > -3.3);
    CHECK_THAT(toneGainDb<DeEsser>(12000.0, -6.0, [](auto& d) { setParameter(d, "threshold", 0.0f); }), WithinAbs(0.0, 0.3));

    // Band (3 .. 10 kHz): the sibilance band comes down; the air above it is neither detected nor
    // turned down.
    const auto band = [](DeEsser& d)
    {
        setParameter(d, "band", 1.0f);
        setParameter(d, "freq", 3000.0f);
        setParameter(d, "high", 10000.0f);
        setParameter(d, "threshold", -20.0f);
    };
    CHECK(toneGainDb<DeEsser>(5500.0, -6.0, band) < -5.0);
    CHECK_THAT(toneGainDb<DeEsser>(18000.0, -10.0, band), WithinAbs(0.0, 0.3));
    {
        auto [node, deesser] = makeNode<DeEsser>();
        band(*deesser);
        const auto n = at(1.0);
        const auto in = sum(tone(5500.0, n, 0.5), tone(17000.0, n, 0.2));
        const auto out = run(*node, in);
        CHECK(toDb(amplitudeAt(out.left, 5500.0, n / 2, n) / amplitudeAt(in.left, 5500.0, n / 2, n)) < -5.0);
        CHECK_THAT(toDb(amplitudeAt(out.left, 17000.0, n / 2, n) / amplitudeAt(in.left, 17000.0, n / 2, n)), WithinAbs(0.0, 0.3));
    }

    // Relative: the highs' share of the whole signal, so a quiet and a loud "s" come down alike, and
    // a loud vowel with a little air is left alone.
    {
        const auto gainOfHighs = [](double levelDb, bool relative, double highShareDb)
        {
            auto [node, deesser] = makeNode<DeEsser>();
            if (relative)
            {
                setParameter(*deesser, "detect", 1.0f);
                setParameter(*deesser, "threshold", -18.0f);
            }
            const auto n = at(1.0);
            const auto in = sum(tone(300.0, n, level(levelDb)), tone(11000.0, n, level(levelDb + highShareDb)));
            const auto out = run(*node, in);
            return toDb(amplitudeAt(out.left, 11000.0, n / 2, n) / amplitudeAt(in.left, 11000.0, n / 2, n));
        };
        // Absolute: only the loud one.
        CHECK(gainOfHighs(-6.0, false, 0.0) < -5.0);
        CHECK(gainOfHighs(-36.0, false, 0.0) > -0.3);
        // Relative: both.
        CHECK(gainOfHighs(-6.0, true, 0.0) < -5.0);
        CHECK(gainOfHighs(-36.0, true, 0.0) < -5.0);
        CHECK(gainOfHighs(-6.0, true, -24.0) > -0.3); // (a vowel with a little air)
    }

    // Wide: while the "s" sounds, everything comes down, the body too; Split keeps the body.
    for (const bool wide : {true, false})
    {
        auto [node, deesser] = makeNode<DeEsser>();
        setParameter(*deesser, "mode", wide ? 1.0f : 0.0f);
        const auto n = at(1.0);
        const auto in = sum(tone(1000.0, n, 0.3), tone(11000.0, n, 0.4));
        const auto out = run(*node, in);
        const double body = toDb(amplitudeAt(out.left, 1000.0, n / 2, n) / amplitudeAt(in.left, 1000.0, n / 2, n));
        if (wide)
            CHECK(body < -6.0);
        else
            CHECK_THAT(body, WithinAbs(0.0, 0.3));
        CHECK(deesser->takeReductionDb() > 6.0f);
    }
}

TEST_CASE("De-Esser: lookahead, stereo modes and listening to the trigger or the delta", "[devices][deesser]")
{
    using devices::DeEsser;
    // Lookahead: reported latency; the gain is already down when the "s" starts.
    {
        DeEsser deesser;
        deesser.setPlayConfigDetails(2, 2, kRate, kBlock);
        deesser.prepareToPlay(kRate, kBlock);
        setParameter(deesser, "lookahead", 2.0f); // 2 ms
        CHECK(deesser.getLatencySamples() == 96);
        setParameter(deesser, "lookahead", 0.0f);
        CHECK(deesser.getLatencySamples() == 0);
    }
    const auto onset = [](int lookahead)
    {
        auto [node, deesser] = makeNode<DeEsser>();
        setParameter(*deesser, "lookahead", static_cast<float>(lookahead));
        setParameter(*deesser, "release", 300.0f);
        const auto in = concat({Stereo(at(0.1)), tone(11000.0, at(0.1), level(-3.0))});
        const auto out = run(*node, in);
        const auto start = at(0.1) + static_cast<std::size_t>(deesser->getLatencySamples());
        return peakOf(out.left, start, start + at(0.0004));
    };
    CHECK(toDb(onset(3) / onset(0)) < -3.0);

    // Stereo: Linked turns both sides down; Unlinked only the side with the "s".
    const auto quietSide = [](int stereo)
    {
        auto [node, deesser] = makeNode<DeEsser>();
        setParameter(*deesser, "stereo", static_cast<float>(stereo));
        setParameter(*deesser, "mode", 1.0f); // wide: everything on that side comes down
        const auto n = at(1.0);
        const auto in = tone(11000.0, n, 0.5, 1000.0, 0.3);
        const auto out = run(*node, in);
        return toDb(amplitudeAt(out.right, 1000.0, n / 2, n) / amplitudeAt(in.right, 1000.0, n / 2, n));
    };
    CHECK(quietSide(0) < -6.0);
    CHECK_THAT(quietSide(1), WithinAbs(0.0, 0.2));
    // Mid / Side: sibilance only in the side signal (out of phase) is caught in Side mode only.
    const auto sideSibilance = [](int stereo)
    {
        auto [node, deesser] = makeNode<DeEsser>();
        setParameter(*deesser, "stereo", static_cast<float>(stereo));
        const auto n = at(1.0);
        auto in = tone(11000.0, n, 0.5);
        for (auto& x : in.right)
            x = -x;
        const auto out = run(*node, in);
        return toDb(amplitudeAt(out.left, 11000.0, n / 2, n) / amplitudeAt(in.left, 11000.0, n / 2, n));
    };
    CHECK_THAT(sideSibilance(2), WithinAbs(0.0, 0.3));
    CHECK(sideSibilance(3) < -6.0);

    // Listen: Trigger plays what the detector hears; Delta what is taken out (with the processed
    // signal, the whole input again).
    CHECK(toneGainDb<DeEsser>(1000.0, -6.0, [](auto& d) { setParameter(d, "listen", 1.0f); }) < -40.0);
    CHECK(toneGainDb<DeEsser>(12000.0, -6.0, [](auto& d) { setParameter(d, "listen", 1.0f); }) > -3.0);
    {
        const auto n = at(1.0);
        const auto in = sum(tone(1000.0, n, 0.3), tone(11000.0, n, 0.4));
        const auto output = [&](int listen)
        {
            auto [node, deesser] = makeNode<DeEsser>();
            setParameter(*deesser, "listen", static_cast<float>(listen));
            return run(*node, in);
        };
        const auto processed = output(0), delta = output(2);
        CHECK(toDb(amplitudeAt(delta.left, 11000.0, n / 2, n) / 0.4) > -6.0);
        CHECK(amplitudeAt(delta.left, 1000.0, n / 2, n) < 0.003);
        const auto whole = sum(processed, delta);
        CHECK_THAT(toDb(amplitudeAt(whole.left, 11000.0, n / 2, n) / 0.4), WithinAbs(0.0, 0.2));
    }
}

//==================================================================================================
TEST_CASE("Expander: downward and upward, knee, hysteresis and range", "[devices][expander]")
{
    using devices::Expander;
    const auto hard = [](Expander& e)
    {
        setParameter(e, "threshold", -40.0f);
        setParameter(e, "ratio", 2.0f);
        setParameter(e, "range", 20.0f);
        setParameter(e, "knee", 0.0f);
        setParameter(e, "hysteresis", 0.0f);
    };
    CHECK_THAT(toneGainDb<Expander>(1000.0, -20.0, hard), WithinAbs(0.0, 0.1));   // above: untouched
    CHECK_THAT(toneGainDb<Expander>(1000.0, -50.0, hard), WithinAbs(-10.0, 1.0)); // 10 dB under: 10 dB more
    CHECK_THAT(toneGainDb<Expander>(1000.0, -70.0, hard), WithinAbs(-20.0, 1.0)); // capped by the range
    CHECK_THAT(toneGainDb<Expander>(1000.0, -50.0, [&](auto& e) { hard(e); setParameter(e, "output", 6.0f); }), WithinAbs(-4.0, 1.0));

    // Upward: louder above the threshold (restoring punch), at most by the range.
    const auto upward = [](Expander& e)
    {
        setParameter(e, "mode", 1.0f);
        setParameter(e, "threshold", -30.0f);
        setParameter(e, "ratio", 2.0f);
        setParameter(e, "range", 12.0f);
        setParameter(e, "knee", 0.0f);
        setParameter(e, "hysteresis", 0.0f);
    };
    CHECK_THAT(toneGainDb<Expander>(1000.0, -40.0, upward), WithinAbs(0.0, 0.1));
    CHECK_THAT(toneGainDb<Expander>(1000.0, -24.0, upward), WithinAbs(6.0, 0.6));
    CHECK_THAT(toneGainDb<Expander>(1000.0, -10.0, upward), WithinAbs(12.0, 0.6));

    // The static curve, and hysteresis: open, it closes only further down.
    Expander expander;
    hard(expander);
    CHECK_THAT(expander.gainChangeFor(-50.0), WithinAbs(-10.0, 1e-6));
    setParameter(expander, "hysteresis", 4.0f);
    CHECK_THAT(expander.gainChangeFor(-42.0, false), WithinAbs(-2.0, 1e-6));
    CHECK_THAT(expander.gainChangeFor(-42.0, true), WithinAbs(0.0, 1e-6));

    // A level hovering around the threshold chatters without hysteresis, not with it.
    const auto deepest = [](float hysteresis)
    {
        auto [node, e] = makeNode<Expander>();
        setParameter(*e, "threshold", -30.0f);
        setParameter(*e, "ratio", 4.0f);
        setParameter(*e, "range", 30.0f);
        setParameter(*e, "knee", 0.0f);
        setParameter(*e, "hysteresis", hysteresis);
        setParameter(*e, "detect", 1.0f); // RMS: steady on a steady tone
        setParameter(*e, "attack", 1.0f);
        setParameter(*e, "release", 10.0f);
        setParameter(*e, "hold", 0.0f);
        std::vector<Stereo> parts;
        for (int k = 0; k < 8; ++k)
            parts.push_back(tone(1000.0, at(0.1), level(k % 2 == 0 ? -28.0 : -31.5)));
        Stereo in;
        for (const auto& part : parts)
            in = concat({in, part});
        const auto out = run(*node, in);
        double lowest = 0.0;
        for (double t = 0.15; t < 0.8; t += 0.01)
            lowest = std::min(lowest, toDb(rmsOf(out.left, at(t), at(t + 0.01)) / rmsOf(in.left, at(t), at(t + 0.01))));
        return lowest;
    };
    CHECK(deepest(0.0f) < -3.0);
    CHECK(deepest(4.0f) > -0.5);
}

TEST_CASE("Expander: hold, peak or RMS detection, lookahead, key filters and listen", "[devices][expander]")
{
    using devices::Expander;
    const auto gate = [](Expander& e)
    {
        setParameter(e, "threshold", -40.0f);
        setParameter(e, "ratio", 10.0f);
        setParameter(e, "range", 30.0f);
        setParameter(e, "knee", 0.0f);
        setParameter(e, "hysteresis", 0.0f);
        setParameter(e, "release", 5.0f);
    };
    // Hold: after the level drops, it waits before closing.
    const auto afterDrop = [&](float hold)
    {
        auto [node, e] = makeNode<Expander>();
        gate(*e);
        setParameter(*e, "hold", hold);
        const auto in = concat({tone(1000.0, at(0.3), level(-34.0)), tone(1000.0, at(0.3), level(-60.0))});
        const auto out = run(*node, in);
        return toDb(rmsOf(out.left, at(0.36), at(0.38)) / rmsOf(in.left, at(0.36), at(0.38)));
    };
    CHECK(afterDrop(200.0f) > -0.5);
    CHECK(afterDrop(0.0f) < -20.0);

    // Peak against RMS: a lone click reads loud to the peak detector, quiet to the RMS one.
    for (const bool rms : {false, true})
    {
        auto [node, e] = makeNode<Expander>();
        setParameter(*e, "detect", rms ? 1.0f : 0.0f);
        Stereo click(at(0.05));
        click.left[100] = click.right[100] = 0.5;
        run(*node, click);
        const float detected = e->takeDetectorDb();
        if (rms)
            CHECK(detected < -20.0f);
        else
            CHECK(detected > -7.0f);
    }

    // Lookahead: reported latency; the first hit after silence comes through whole.
    const auto firstHit = [&](int lookahead)
    {
        auto [node, e] = makeNode<Expander>();
        gate(*e);
        setParameter(*e, "attack", 1.0f);
        setParameter(*e, "lookahead", static_cast<float>(lookahead));
        const auto in = concat({Stereo(at(0.1)), tone(1000.0, at(0.1), level(-10.0))});
        const auto out = run(*node, in);
        const auto start = at(0.1) + static_cast<std::size_t>(e->getLatencySamples());
        return toDb(peakOf(out.left, start, start + at(0.001)) / level(-10.0));
    };
    {
        Expander e;
        e.setPlayConfigDetails(2, 2, kRate, kBlock);
        e.prepareToPlay(kRate, kBlock);
        setParameter(e, "lookahead", 3.0f); // 5 ms
        CHECK(e.getLatencySamples() == 240);
    }
    CHECK(firstHit(3) > -1.0);
    CHECK(firstHit(0) < -6.0);

    // Key filters: rumble under the key high-pass, or hiss over the key low-pass, does not open it.
    CHECK_THAT(toneGainDb<Expander>(60.0, -30.0, gate), WithinAbs(0.0, 0.2));
    CHECK(toneGainDb<Expander>(60.0, -30.0, [&](auto& e) { gate(e); setParameter(e, "keylow", 600.0f); }) < -20.0);
    CHECK(toneGainDb<Expander>(12000.0, -30.0, [&](auto& e) { gate(e); setParameter(e, "keyhigh", 1000.0f); }) < -20.0);
    // Key listen: what the detector hears.
    CHECK(toneGainDb<Expander>(60.0, -10.0, [&](auto& e) { setParameter(e, "keylow", 600.0f); setParameter(e, "keylisten", 1.0f); }) < -30.0);
    CHECK_THAT(toneGainDb<Expander>(3000.0, -10.0, [&](auto& e) { setParameter(e, "keylow", 600.0f); setParameter(e, "keylisten", 1.0f); }),
               WithinAbs(0.0, 0.5));

    // An external key: loud main, silent key: fully down by the range.
    {
        auto [node, e] = makeNode<Expander>();
        gate(*e);
        setParameter(*e, "sidechain", 1.0f);
        const auto n = at(1.0);
        const auto in = tone(1000.0, n, 0.5);
        const Stereo key(n);
        const auto out = run(*node, in, &key);
        CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, n / 2, n) / amplitudeAt(in.left, 1000.0, n / 2, n)), WithinAbs(-30.0, 0.5));
    }
    // Dry/Wet 0 with lookahead: the dry signal, delayed by the latency.
    {
        auto [node, e] = makeNode<Expander>();
        gate(*e);
        setParameter(*e, "mix", 0.0f);
        setParameter(*e, "lookahead", 1.0f);
        const auto latency = static_cast<std::size_t>(e->getLatencySamples());
        const auto in = tone(500.0, at(0.2), level(-60.0));
        const auto out = run(*node, in);
        double error = 0.0;
        for (std::size_t i = latency; i < in.left.size(); ++i)
            error = std::max(error, std::abs(out.left[i] - in.left[i - latency]));
        CHECK(error < 1e-12);
    }
}

//==================================================================================================
TEST_CASE("Soft Clipper: shapes, asymmetry, stereo link, auto gain, delta and the ceiling", "[devices][clipper]")
{
    using devices::SoftClipper;
    // The curves: unchanged below the knee, never past 1, monotonic.
    for (int shape = 0; shape < 4; ++shape)
    {
        double previous = -2.0;
        for (double x = -4.0; x <= 4.0; x += 0.01)
        {
            const double y = SoftClipper::unitShape(x, shape, 0.5);
            CHECK(std::abs(y) <= 1.0 + 1e-12);
            CHECK(y >= previous - 1e-12);
            previous = y;
        }
        CHECK_THAT(SoftClipper::unitShape(0.3, shape, 0.5), WithinAbs(0.3, 1e-12));
    }
    CHECK_THAT(SoftClipper::unitShape(2.0, 2, 0.5), WithinAbs(1.0, 1e-12));                     // hard
    CHECK(SoftClipper::unitShape(0.8, 0, 0.5) < 0.8);                                           // soft rounds
    CHECK(SoftClipper::unitShape(0.8, 1, 0.5) < SoftClipper::unitShape(0.8, 0, 0.5) + 1e-9);    // smooth is gentler
    CHECK(SoftClipper::unitShape(0.6, 3, 0.5) < SoftClipper::unitShape(0.6, 0, 0.5) - 0.005); // tube bends earliest

    // The ceiling holds for every shape and oversampling (after the downsampling filter too).
    for (int factor = 0; factor < 4; ++factor)
        for (int shape = 0; shape < 4; ++shape)
        {
            INFO("oversampling " << factor << ", shape " << shape);
            auto [node, clipper] = makeNode<SoftClipper>();
            setParameter(*clipper, "osrealtime", static_cast<float>(factor));
            setParameter(*clipper, "shape", static_cast<float>(shape));
            setParameter(*clipper, "drive", 12.0f);
            setParameter(*clipper, "ceiling", -1.0f);
            setParameter(*clipper, "asymmetry", shape == 3 ? 60.0f : 0.0f);
            const auto out = run(*node, sum(tone(110.0, at(0.5), 0.6), tone(3500.0, at(0.5), 0.3)));
            CHECK(allFinite(out));
            CHECK(peakOf(out.left) <= level(-1.0) + 1e-9);
            CHECK(clipper->takeClipDb() > 6.0f);
        }

    // Asymmetry: even harmonics (a tube stage's second harmonic).
    const auto secondHarmonic = [](float asymmetry)
    {
        auto [node, clipper] = makeNode<SoftClipper>();
        setParameter(*clipper, "drive", 9.0f);
        setParameter(*clipper, "asymmetry", asymmetry);
        const auto n = at(0.5);
        const auto out = run(*node, tone(500.0, n, 0.5));
        return toDb(amplitudeAt(out.left, 1000.0, n / 2, n) / amplitudeAt(out.left, 500.0, n / 2, n));
    };
    CHECK(secondHarmonic(0.0f) < -80.0);
    CHECK(secondHarmonic(100.0f) > -40.0);

    // Stereo link: when one side clips, the other comes down by as much (the image stays put).
    const auto quietSide = [](bool link)
    {
        auto [node, clipper] = makeNode<SoftClipper>();
        setParameter(*clipper, "link", link ? 1.0f : 0.0f);
        setParameter(*clipper, "drive", 12.0f);
        setParameter(*clipper, "softness", 0.0f);
        const auto n = at(0.5);
        const auto in = tone(200.0, n, 0.5, 1000.0, 0.05);
        const auto out = run(*node, in);
        return toDb(amplitudeAt(out.right, 1000.0, n / 2, n) / (0.05 * level(12.0)));
    };
    CHECK_THAT(quietSide(false), WithinAbs(0.0, 0.2));
    CHECK(quietSide(true) < -2.0);

    // Auto gain: what is not clipped comes out as loud as it went in.
    CHECK_THAT(toneGainDb<SoftClipper>(1000.0, -40.0, [](auto& c) { setParameter(c, "drive", 12.0f); setParameter(c, "autogain", 1.0f); }),
               WithinAbs(0.0, 0.1));
    CHECK_THAT(toneGainDb<SoftClipper>(1000.0, -40.0, [](auto& c) { setParameter(c, "drive", 12.0f); }), WithinAbs(12.0, 0.1));

    // Delta: only what the clipper takes away; with the clipped signal, the driven input again.
    {
        const auto in = tone(300.0, at(0.3), 0.8);
        const auto output = [&](int listen)
        {
            auto [node, clipper] = makeNode<SoftClipper>();
            setParameter(*clipper, "osrealtime", 0.0f);
            setParameter(*clipper, "drive", 6.0f);
            setParameter(*clipper, "listen", static_cast<float>(listen));
            return run(*node, in);
        };
        const auto clipped = output(0), delta = output(1);
        CHECK(peakOf(delta.left) > 0.3);
        double error = 0.0;
        for (std::size_t i = 0; i < in.left.size(); ++i)
            error = std::max(error, std::abs(clipped.left[i] + delta.left[i] - in.left[i] * level(6.0)));
        CHECK(error < 1e-9);
    }
}

TEST_CASE("Soft Clipper: oversampling settings, latency and aliasing", "[devices][clipper]")
{
    using devices::SoftClipper;
    {
        auto [node, clipper] = makeNode<SoftClipper>();
        setParameter(*clipper, "osrealtime", 0.0f);
        CHECK(clipper->getLatencySamples() == 0);
        setParameter(*clipper, "osrealtime", 1.0f);
        const int two = clipper->getLatencySamples();
        setParameter(*clipper, "osrealtime", 2.0f);
        const int four = clipper->getLatencySamples();
        CHECK(two > 0);
        CHECK(four > two);
        setParameter(*clipper, "osphase", 0.0f);
        CHECK(clipper->getLatencySamples() < four);
        clipper->setNonRealtime(true);
        CHECK(clipper->getLatencySamples() == clipper->latencyFor(true));
        clipper->setNonRealtime(false);
    }
    // Half wet on a quiet signal (no clipping): the dry half lines up with the wet half (no combing).
    for (const double f : {500.0, 5000.0, 12000.0})
    {
        INFO(f);
        CHECK_THAT(toneGainDb<SoftClipper>(f, -30.0, [](auto& c) { setParameter(c, "mix", 50.0f); }), WithinAbs(0.0, 0.2));
    }
    // Aliasing: clipping a 7 kHz tone (over a bass note) folds its fifth harmonic (35 kHz) down to
    // 13 kHz at the base rate; oversampling keeps it out. Without the ceiling guard, heavy clipping
    // stays clean too (the filter's ringing may then poke a little over the ceiling).
    const auto alias = [](int factor, float drive, bool guard, double* peak = nullptr)
    {
        auto [node, clipper] = makeNode<SoftClipper>();
        setParameter(*clipper, "drive", drive);
        setParameter(*clipper, "guard", guard ? 1.0f : 0.0f);
        setParameter(*clipper, "osrealtime", static_cast<float>(factor));
        const auto n = at(0.5);
        const auto out = run(*node, sum(tone(7000.0, n, 0.4), tone(100.0, n, 0.3)));
        if (peak != nullptr)
            *peak = peakOf(out.left, n / 2);
        return toDb(amplitudeAt(out.left, 13000.0, n / 2, n));
    };
    CHECK(alias(3, 6.0f, true) < alias(0, 6.0f, true) - 30.0);
    double peak = 0.0;
    CHECK(alias(3, 12.0f, false, &peak) < alias(0, 12.0f, false) - 30.0);
    CHECK(peak < level(-0.3 + 1.5));
}

//==================================================================================================
TEST_CASE("Plate Reverb: its decay time, pre-delay, damping and stereo spread", "[devices][plate]")
{
    /** The tail's level (dB) in 50 ms windows after an impulse, fully wet. */
    const auto tail = [](auto setup, double seconds)
    {
        auto [node, plate] = makeNode<devices::PlateReverb>();
        setParameter(*plate, "mix", 100.0f);
        setup(*plate);
        Stereo impulse(at(seconds));
        impulse.left[0] = impulse.right[0] = 1.0;
        return run(*node, impulse);
    };
    const auto windowDb = [](const Stereo& s, double from, double to) { return toDb(0.5 * (rmsOf(s.left, at(from), at(to)) + rmsOf(s.right, at(from), at(to)))); };

    for (const float decay : {0.8f, 2.0f})
    {
        INFO("decay " << decay);
        const auto out = tail([&](auto& p)
                              {
                                  setParameter(p, "decay", decay);
                                  setParameter(p, "damping", 20000.0f);
                                  setParameter(p, "predelay", 0.0f);
                              },
                              decay * 1.5);
        CHECK(allFinite(out));
        // The slope between 0.2 s and 0.2 s + half the decay time: about 30 dB.
        const double early = windowDb(out, 0.2, 0.25);
        const double late = windowDb(out, 0.2 + decay * 0.5, 0.25 + decay * 0.5);
        const double rt60 = (decay * 0.5) * 60.0 / (early - late);
        INFO("measured RT60 " << rt60);
        CHECK(rt60 > decay * 0.65);
        CHECK(rt60 < decay * 1.4);
    }

    // Pre-delay: nothing comes out before it.
    {
        const auto out = tail([](auto& p) { setParameter(p, "predelay", 50.0f); }, 0.3);
        CHECK(peakOf(out.left, 0, at(0.049)) < 1e-9);
        CHECK(peakOf(out.left, at(0.05), at(0.3)) > 1e-3);
    }

    // Damping: a dark plate loses its highs faster than a bright one (highs above 5 kHz against
    // lows under 1 kHz, in the tail of a burst of noise).
    const auto highShare = [&](float damping)
    {
        auto [node, plate] = makeNode<devices::PlateReverb>();
        setParameter(*plate, "mix", 100.0f);
        setParameter(*plate, "damping", damping);
        setParameter(*plate, "decay", 2.0f);
        const auto out = run(*node, concat({whiteNoise(at(0.3), 0.3), Stereo(at(0.7))}));
        using Shape = dsp::BiquadCoefficients::Shape;
        const auto high = dsp::BiquadCoefficients::make(Shape::lowCut, kRate, 5000.0, 0.7071, 0.0);
        const auto low = dsp::BiquadCoefficients::make(Shape::highCut, kRate, 1000.0, 0.7071, 0.0);
        dsp::Biquad hp, lp;
        double highEnergy = 0.0, lowEnergy = 0.0;
        for (std::size_t i = 0; i < out.left.size(); ++i)
        {
            const double h = hp.process(out.left[i], high);
            const double l = lp.process(out.left[i], low);
            if (i >= at(0.5))
            {
                highEnergy += h * h;
                lowEnergy += l * l;
            }
        }
        return 10.0 * std::log10(highEnergy / lowEnergy);
    };
    CHECK(highShare(2000.0f) < highShare(18000.0f) - 6.0);

    // A mono input comes out stereo (the taps differ), and at Width 0 mono again.
    {
        const auto out = tail([](auto&) {}, 1.0);
        const auto correlation = [](const std::vector<double>& a, const std::vector<double>& b)
        {
            double ab = 0.0, aa = 0.0, bb = 0.0;
            for (std::size_t i = at(0.05); i < a.size(); ++i)
            {
                ab += a[i] * b[i];
                aa += a[i] * a[i];
                bb += b[i] * b[i];
            }
            return ab / std::sqrt(aa * bb + 1e-30);
        };
        CHECK(std::abs(correlation(out.left, out.right)) < 0.5);
        const auto narrow = tail([](auto& p) { setParameter(p, "width", 0.0f); }, 0.5);
        CHECK(correlation(narrow.left, narrow.right) > 0.999);
    }

    // The longest decay stays stable; Dry/Wet 0 is dry.
    {
        auto [node, plate] = makeNode<devices::PlateReverb>();
        setParameter(*plate, "decay", 10.0f);
        setParameter(*plate, "size", 150.0f);
        setParameter(*plate, "mix", 100.0f);
        const auto out = run(*node, concat({whiteNoise(at(1.0), 0.5), Stereo(at(4.0))}));
        CHECK(allFinite(out));
        CHECK(peakOf(out.left) < 4.0);
    }
    {
        auto [node, plate] = makeNode<devices::PlateReverb>();
        setParameter(*plate, "mix", 0.0f);
        const auto in = whiteNoise(at(0.2), 0.5);
        const auto out = run(*node, in);
        CHECK(out.left == in.left);
    }
}

TEST_CASE("Plate Reverb: plates, low decay, synced pre-delay, ducking, freeze and high cut", "[devices][plate]")
{
    using devices::PlateReverb;
    using Shape = dsp::BiquadCoefficients::Shape;
    /** Energy (dB) of the wet output in a band, between two times, after a burst of noise. */
    const auto bandEnergy = [](auto setup, double lowHz, double highHz, double from, double to)
    {
        auto [node, plate] = makeNode<PlateReverb>();
        setParameter(*plate, "mix", 100.0f);
        setup(*plate);
        const auto out = run(*node, concat({whiteNoise(at(0.3), 0.3), Stereo(at(to))}));
        const auto hp = dsp::BiquadCoefficients::make(Shape::lowCut, kRate, lowHz, 0.7071, 0.0);
        const auto lp = dsp::BiquadCoefficients::make(Shape::highCut, kRate, highHz, 0.7071, 0.0);
        std::array<dsp::Biquad, 2> high, low;
        double energy = 0.0;
        for (std::size_t i = 0; i < out.left.size(); ++i)
        {
            const double y = low[1].process(low[0].process(high[1].process(high[0].process(out.left[i], hp), hp), lp), lp);
            if (i >= at(from) && i < at(to))
                energy += y * y;
        }
        return 10.0 * std::log10(energy + 1e-30);
    };

    // Plates: Gold Foil is brighter than Steel (its highs share more of the tail).
    {
        const auto brightness = [&](float plate)
        {
            const auto setup = [plate](PlateReverb& p) { setParameter(p, "plate", plate); };
            return bandEnergy(setup, 5000.0, 20000.0, 0.5, 1.0) - bandEnergy(setup, 20.0, 1000.0, 0.5, 1.0);
        };
        CHECK(brightness(1.0f) > brightness(0.0f) + 1.5);
        CHECK(PlateReverb::characterOf(1).size < PlateReverb::characterOf(0).size);
        CHECK(PlateReverb::characterOf(2).size > PlateReverb::characterOf(0).size);
    }

    // Low Decay: the lows ring longer (or shorter) than the rest.
    {
        const auto lowsLate = [&](float lowDecay)
        {
            const auto setup = [lowDecay](PlateReverb& p)
            {
                setParameter(p, "decay", 1.0f);
                setParameter(p, "lowdecay", lowDecay);
                setParameter(p, "lowfreq", 300.0f);
                setParameter(p, "lowcut", 20.0f);
            };
            return bandEnergy(setup, 20.0, 200.0, 1.0, 1.4) - bandEnergy(setup, 20.0, 200.0, 0.3, 0.5);
        };
        CHECK(lowsLate(200.0f) > lowsLate(50.0f) + 15.0);
        PlateReverb plate;
        setParameter(plate, "decay", 2.0f);
        setParameter(plate, "lowdecay", 150.0f);
        CHECK_THAT(plate.getLowDecaySeconds(), WithinAbs(3.0, 1e-6));
    }

    // Pre-delay synced to the tempo: an eighth at 120 BPM is 250 ms.
    {
        auto [node, plate] = makeNode<PlateReverb>();
        setParameter(*plate, "mix", 100.0f);
        setParameter(*plate, "sync", 1.0f);
        setParameter(*plate, "division", 5.0f); // 1/8
        Stereo impulse(at(0.6));
        impulse.left[0] = impulse.right[0] = 1.0;
        const auto out = runPlaying(*node, *plate, impulse, 120.0, true, [](std::size_t) {});
        CHECK_THAT(plate->getPreDelayMs(), WithinAbs(250.0, 1e-6));
        CHECK(peakOf(out.left, 0, at(0.245)) < 1e-9);
        CHECK(peakOf(out.left, at(0.25), at(0.6)) > 1e-3);
    }

    // Ducking: the reverb steps back while the dry signal plays.
    {
        const auto during = [](float ducking)
        {
            auto [node, plate] = makeNode<PlateReverb>();
            setParameter(*plate, "mix", 100.0f);
            setParameter(*plate, "ducking", ducking);
            const auto out = run(*node, whiteNoise(at(1.0), 0.5));
            return toDb(rmsOf(out.left, at(0.5), at(1.0)));
        };
        CHECK(during(100.0f) < during(0.0f) - 15.0);
    }

    // Freeze: the tail holds, and new input is ignored.
    {
        auto [node, plate] = makeNode<PlateReverb>();
        setParameter(*plate, "mix", 100.0f);
        setParameter(*plate, "decay", 1.0f);
        run(*node, whiteNoise(at(0.5), 0.3));
        setParameter(*plate, "freeze", 1.0f);
        const auto out = run(*node, concat({Stereo(at(1.0)), whiteNoise(at(0.5), 0.9, 7), Stereo(at(1.5))}));
        CHECK(allFinite(out));
        const double early = toDb(rmsOf(out.left, at(0.1), at(0.4)));
        CHECK(early > -50.0);
        CHECK_THAT(toDb(rmsOf(out.left, at(2.6), at(2.9))), WithinAbs(early, 3.0)); // (an unfrozen 1 s decay would be 150 dB down)
        CHECK(plate->getTailLengthSeconds() > 100.0);
    }

    // High Cut darkens the reverb; drive and the modulation rate keep it stable.
    {
        const auto highs = [&](float highCut)
        { return bandEnergy([highCut](PlateReverb& p) { setParameter(p, "highcut", highCut); }, 6000.0, 20000.0, 0.3, 0.8); };
        CHECK(highs(2000.0f) < highs(20000.0f) - 15.0);
        auto [node, plate] = makeNode<PlateReverb>();
        setParameter(*plate, "drive", 100.0f);
        setParameter(*plate, "modrate", 5.0f);
        setParameter(*plate, "modulation", 100.0f);
        setParameter(*plate, "plate", 2.0f);
        setParameter(*plate, "size", 150.0f);
        const auto out = run(*node, concat({whiteNoise(at(0.5), 0.9), Stereo(at(1.0))}));
        CHECK(allFinite(out));
        CHECK(peakOf(out.left) < 4.0);
    }
}

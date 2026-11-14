#include "devices/AutoFilter.h"
#include "devices/ChannelEq.h"
#include "devices/Gate.h"
#include "devices/MultibandDynamics.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;

//==================================================================================================
// Gate

TEST_CASE("Gate passes loud signal, silences what falls below the threshold", "[devices][gate]")
{
    auto [node, gate] = makeNode<Gate>();
    setParameter(*gate, "threshold", -30.0f);
    setParameter(*gate, "hold", 0.0f);
    setParameter(*gate, "release", 20.0f);

    // Loud (-6 dB), then quiet (-46 dB), then loud again.
    const auto input = concat({tone(440.0, at(0.3), 0.5), tone(440.0, at(0.3), 0.005), tone(440.0, at(0.3), 0.5)});
    const auto out = run(*node, input);
    CHECK_THAT(amplitudeAt(out.left, 440.0, at(0.1), at(0.3)), WithinAbs(0.5, 0.005));
    CHECK(amplitudeAt(out.left, 440.0, at(0.45), at(0.6)) < 1.0e-5); // floor -80 dB: silence
    CHECK_THAT(amplitudeAt(out.left, 440.0, at(0.7), at(0.9)), WithinAbs(0.5, 0.005));
    CHECK(gate->takeDetectorPeakDb() > -7.0f);

    // A floor of -20 dB only attenuates.
    setParameter(*gate, "floor", -20.0f);
    const auto floored = run(*node, input);
    CHECK_THAT(toDb(amplitudeAt(floored.left, 440.0, at(0.45), at(0.6)) / 0.005), WithinAbs(-20.0, 0.3));

    // Flip: the loud parts are ducked to the floor, the quiet part passes.
    setParameter(*gate, "flip", 1.0f);
    const auto flipped = run(*node, input);
    CHECK_THAT(toDb(amplitudeAt(flipped.left, 440.0, at(0.1), at(0.3)) / 0.5), WithinAbs(-20.0, 0.3));
    CHECK_THAT(amplitudeAt(flipped.left, 440.0, at(0.45), at(0.6)), WithinAbs(0.005, 0.0002));
}

TEST_CASE("Gate return (hysteresis) and hold keep it open", "[devices][gate]")
{
    auto [node, gate] = makeNode<Gate>();
    setParameter(*gate, "threshold", -30.0f);
    setParameter(*gate, "hold", 0.0f);
    setParameter(*gate, "release", 10.0f);

    // Loud, then hovering at -33 dB: with 6 dB of return the open gate stays open...
    const auto input = concat({tone(300.0, at(0.2), 0.5), tone(300.0, at(0.3), 0.0224)});
    setParameter(*gate, "return", 6.0f);
    CHECK_THAT(toDb(amplitudeAt(run(*node, input).left, 300.0, at(0.3), at(0.5)) / 0.0224), WithinAbs(0.0, 0.2));
    // ... with none it closes.
    setParameter(*gate, "return", 0.0f);
    CHECK(amplitudeAt(run(*node, input).left, 300.0, at(0.3), at(0.5)) < 1.0e-5);

    // Hold: open for 100 ms after the signal stops being loud, then released.
    setParameter(*gate, "hold", 100.0f);
    const auto held = run(*node, concat({tone(300.0, at(0.2), 0.5), tone(300.0, at(0.4), 0.005)}));
    CHECK_THAT(amplitudeAt(held.left, 300.0, at(0.21), at(0.29)), WithinAbs(0.005, 0.0003));
    CHECK(amplitudeAt(held.left, 300.0, at(0.4), at(0.6)) < 1.0e-5);
}

TEST_CASE("Gate lookahead opens before a transient and reports its latency", "[devices][gate]")
{
    auto [node, gate] = makeNode<Gate>();
    setParameter(*gate, "threshold", -30.0f);
    setParameter(*gate, "attack", 2.0f);
    CHECK(node->getLatencySamples() == 0);

    // A burst after silence: without lookahead the attack softens its first cycles.
    const auto input = concat({Stereo(at(0.1)), tone(1000.0, at(0.1), 0.5)});
    const auto plain = run(*node, input);
    const double plainFirst = peakOf(plain.left, at(0.1), at(0.1) + 24);

    setParameter(*gate, "lookahead", 3.0f); // 5 ms
    CHECK(node->getLatencySamples() == 240);
    node->prepare(kRate, kBlock, nullptr);
    const auto ahead = run(*node, input);
    const double aheadFirst = peakOf(ahead.left, at(0.1) + 240, at(0.1) + 240 + 24);
    CHECK(plainFirst < 0.2);
    CHECK(aheadFirst > 0.45); // the gate was already open when the burst arrived
}

TEST_CASE("Gate follows an external sidechain through its key filters", "[devices][gate]")
{
    auto [node, gate] = makeNode<Gate>();
    setParameter(*gate, "threshold", -30.0f);
    setParameter(*gate, "hold", 0.0f);
    setParameter(*gate, "release", 10.0f);
    setParameter(*gate, "sidechain", 1.0f);

    // A steady pad, gated by bursts on the sidechain.
    const auto pad = tone(220.0, at(0.6), 0.3);
    const auto key = concat({tone(1000.0, at(0.2), 0.5), Stereo(at(0.2)), tone(1000.0, at(0.2), 0.5)});
    auto out = run(*node, pad, &key);
    CHECK_THAT(amplitudeAt(out.left, 220.0, at(0.05), at(0.2)), WithinAbs(0.3, 0.005));
    CHECK(amplitudeAt(out.left, 220.0, at(0.33), at(0.4)) < 1.0e-5); // (detector decay + release)
    CHECK_THAT(amplitudeAt(out.left, 220.0, at(0.45), at(0.6)), WithinAbs(0.3, 0.005));

    // A high-pass key filter at 2 kHz: a loud bass on the sidechain no longer opens it.
    setParameter(*gate, "keylow", 2000.0f);
    const auto bass = tone(60.0, at(0.6), 0.5);
    out = run(*node, pad, &bass);
    CHECK(amplitudeAt(out.left, 220.0, at(0.2), at(0.6)) < 1.0e-4);

    // Listen plays what the detector hears.
    setParameter(*gate, "keylow", 20.0f);
    setParameter(*gate, "listen", 1.0f);
    out = run(*node, pad, &key);
    CHECK(amplitudeAt(out.left, 1000.0, at(0.05), at(0.2)) > 0.45);
    CHECK(amplitudeAt(out.left, 220.0, at(0.05), at(0.2)) < 1.0e-3);
}

//==================================================================================================
// Multiband Dynamics

TEST_CASE("Multiband Dynamics: the gain computer's four directions", "[devices][multiband]")
{
    // Above: 4:1 downward compression of 12 dB over -> -9 dB; 0.5:1 upward expansion -> +12 dB.
    CHECK_THAT(MultibandDynamics::gainComputer(-6.0, -18.0, 4.0, -80.0, 1.0, false, 1.0), WithinAbs(-9.0, 1e-9));
    CHECK_THAT(MultibandDynamics::gainComputer(-6.0, -18.0, 0.5, -80.0, 1.0, false, 1.0), WithinAbs(12.0, 1e-9));
    // Below: 2:1 downward expansion of 20 dB under -> -20 dB; 0.5:1 upward compression -> +10 dB.
    CHECK_THAT(MultibandDynamics::gainComputer(-50.0, 0.0, 1.0, -30.0, 2.0, false, 1.0), WithinAbs(-20.0, 1e-9));
    CHECK_THAT(MultibandDynamics::gainComputer(-50.0, 0.0, 1.0, -30.0, 0.5, false, 1.0), WithinAbs(10.0, 1e-9));
    // Between the thresholds nothing; Amount scales; upward gain is capped.
    CHECK(MultibandDynamics::gainComputer(-25.0, -18.0, 4.0, -30.0, 2.0, false, 1.0) == 0.0);
    CHECK_THAT(MultibandDynamics::gainComputer(-6.0, -18.0, 4.0, -80.0, 1.0, false, 0.5), WithinAbs(-4.5, 1e-9));
    CHECK(MultibandDynamics::gainComputer(-200.0, 0.0, 1.0, -30.0, 0.25, false, 1.0) == MultibandDynamics::kMaxUpwardDb);
    // The soft knee is continuous at its edges.
    CHECK_THAT(MultibandDynamics::gainComputer(-15.0, -18.0, 4.0, -80.0, 1.0, true, 1.0),
               WithinAbs(MultibandDynamics::gainComputer(-15.0, -18.0, 4.0, -80.0, 1.0, false, 1.0), 1e-9));
}

TEST_CASE("Multiband Dynamics is flat when idle and works on one band at a time", "[devices][multiband]")
{
    auto [node, mb] = makeNode<MultibandDynamics>();
    for (const auto* band : {"low", "mid", "high"})
        setParameter(*mb, juce::String(band) + "arat", 1.0f);

    // Neutral: the crossovers add back up flat.
    const auto input = sum(sum(tone(40.0, at(1.0), 0.2), tone(1000.0, at(1.0), 0.2)), tone(8000.0, at(1.0), 0.2));
    auto out = run(*node, input);
    for (const double f : {40.0, 1000.0, 8000.0})
        CHECK_THAT(toDb(amplitudeAt(out.left, f, at(0.5), at(1.0)) / 0.2), WithinAbs(0.0, 0.15));

    // Compress only the highs (4:1 above -30 dB): 8 kHz comes down, the rest stays.
    setParameter(*mb, "highathr", -30.0f);
    setParameter(*mb, "higharat", 4.0f);
    setParameter(*mb, "knee", 0.0f);
    setParameter(*mb, "detection", 0.0f); // peak
    setParameter(*mb, "attack", 1.0f);
    setParameter(*mb, "release", 300.0f);
    out = run(*node, input);
    // -14 dB peak, 16 dB over the threshold at 4:1 -> 12 dB down.
    CHECK_THAT(toDb(amplitudeAt(out.left, 8000.0, at(0.5), at(1.0)) / 0.2), WithinAbs(-12.0, 0.6));
    CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, at(0.5), at(1.0)) / 0.2), WithinAbs(0.0, 0.2));
    CHECK(mb->getBandGainDb(2) < -10.0f);
    CHECK(mb->getBandGainDb(0) == 0.0f);

    // Solo the mid band: only it is heard.
    setParameter(*mb, "midsolo", 1.0f);
    out = run(*node, input);
    CHECK(amplitudeAt(out.left, 1000.0, at(0.5), at(1.0)) > 0.18);
    CHECK(amplitudeAt(out.left, 40.0, at(0.5), at(1.0)) < 0.01);
    CHECK(amplitudeAt(out.left, 8000.0, at(0.5), at(1.0)) < 0.01);
}

TEST_CASE("Multiband Dynamics compresses upward and expands downward below the lower threshold", "[devices][multiband]")
{
    auto [node, mb] = makeNode<MultibandDynamics>();
    for (const auto* band : {"low", "mid", "high"})
        setParameter(*mb, juce::String(band) + "arat", 1.0f);
    setParameter(*mb, "knee", 0.0f);
    setParameter(*mb, "detection", 0.0f);
    setParameter(*mb, "midbthr", -30.0f);
    setParameter(*mb, "attack", 0.1f);   // (peak-accurate detection)
    setParameter(*mb, "xlow", 40.0f); // (1 kHz well inside the mid band: no leakage into the others)
    setParameter(*mb, "xhigh", 15000.0f);

    // A quiet mid tone at -50 dB peak: upward compression 0.5:1 lifts it 10 dB...
    const auto quiet = tone(1000.0, at(1.5), 0.00316);
    setParameter(*mb, "midbrat", 0.5f);
    auto out = run(*node, quiet);
    CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, at(1.0), at(1.5)) / 0.00316), WithinAbs(10.0, 0.6));

    // ... downward expansion 2:1 pushes it 20 dB down.
    setParameter(*mb, "midbrat", 2.0f);
    out = run(*node, quiet);
    CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, at(1.0), at(1.5)) / 0.00316), WithinAbs(-20.0, 0.6));

    // Dry/wet at 0 is the input (with the crossovers' phase, but the same level).
    setParameter(*mb, "mix", 0.0f);
    out = run(*node, quiet);
    CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, at(1.0), at(1.5)) / 0.00316), WithinAbs(0.0, 0.1));
}

//==================================================================================================
// Auto Filter

TEST_CASE("Auto Filter low- and high-pass in every circuit, stable at full resonance", "[devices][autofilter]")
{
    auto [node, filter] = makeNode<AutoFilter>();
    setParameter(*filter, "freq", 500.0f);
    setParameter(*filter, "res", 0.0f);
    const auto input = sum(tone(100.0, at(0.5), 0.1), tone(5000.0, at(0.5), 0.1));

    for (int circuit = 0; circuit < 4; ++circuit)
    {
        INFO("circuit " << circuit);
        setParameter(*filter, "circuit", static_cast<float>(circuit));
        setParameter(*filter, "type", 0.0f); // low-pass
        auto out = run(*node, input);
        CHECK(toDb(amplitudeAt(out.left, 100.0, at(0.25), at(0.5)) / 0.1) > -3.0);
        CHECK(toDb(amplitudeAt(out.left, 5000.0, at(0.25), at(0.5)) / 0.1) < -30.0);
        setParameter(*filter, "type", 1.0f); // high-pass
        out = run(*node, input);
        CHECK(toDb(amplitudeAt(out.left, 100.0, at(0.25), at(0.5)) / 0.1) < -20.0);
        CHECK(toDb(amplitudeAt(out.left, 5000.0, at(0.25), at(0.5)) / 0.1) > -3.0);
    }

    // Every type, slope and circuit at full resonance and drive stays finite and bounded.
    setParameter(*filter, "res", 100.0f);
    setParameter(*filter, "drive", 24.0f);
    const auto loud = sum(tone(80.0, at(0.3), 0.9), tone(3000.0, at(0.3), 0.5));
    for (int circuit = 0; circuit < 4; ++circuit)
        for (int type = 0; type < 5; ++type)
            for (int slope = 0; slope < 2; ++slope)
            {
                setParameter(*filter, "circuit", static_cast<float>(circuit));
                setParameter(*filter, "type", static_cast<float>(type));
                setParameter(*filter, "slope", static_cast<float>(slope));
                const auto out = run(*node, loud);
                INFO("circuit " << circuit << " type " << type << " slope " << slope);
                REQUIRE(allFinite(out));
                CHECK(peakOf(out.left) < 20.0);
            }
}

TEST_CASE("Auto Filter: the response curve agrees with the clean circuit", "[devices][autofilter]")
{
    auto [node, filter] = makeNode<AutoFilter>();
    setParameter(*filter, "circuit", 0.0f);
    setParameter(*filter, "freq", 1000.0f);
    setParameter(*filter, "res", 40.0f);
    for (const int type : {0, 2})
    {
        setParameter(*filter, "type", static_cast<float>(type));
        for (const double f : {200.0, 1000.0, 3000.0})
        {
            const auto out = run(*node, tone(f, at(0.4), 0.1));
            INFO("type " << type << " at " << f << " Hz");
            CHECK_THAT(toDb(amplitudeAt(out.left, f, at(0.2), at(0.4)) / 0.1), WithinAbs(filter->getResponseDb(f, 1000.0), 1.0));
        }
    }
}

TEST_CASE("Auto Filter's LFO sweeps in time with the song, the envelope opens it", "[devices][autofilter]")
{
    auto [node, filter] = makeNode<AutoFilter>();
    setParameter(*filter, "freq", 1000.0f);
    setParameter(*filter, "lfoamt", 100.0f);
    setParameter(*filter, "lfosync", 1.0f);
    setParameter(*filter, "lfodiv", 9.0f); // 1/4: a cycle per beat
    setParameter(*filter, "lfoshape", 1.0f); // triangle

    // 120 BPM: the cutoff rises and falls every 0.5 s, between 1 kHz / 8 and 1 kHz x 8.
    std::vector<std::pair<double, float>> cutoffs;
    runPlaying(*node, *filter, Stereo(at(2.0)), 120.0, true,
               [&](std::size_t done) { cutoffs.emplace_back(static_cast<double>(done) / kRate, filter->getModulatedCutoff()); });
    float highest = 0.0f, lowest = 1.0e9f;
    std::vector<double> peaks;
    for (std::size_t i = 1; i + 1 < cutoffs.size(); ++i)
    {
        highest = std::max(highest, cutoffs[i].second);
        lowest = std::min(lowest, cutoffs[i].second);
        if (cutoffs[i].second > cutoffs[i - 1].second && cutoffs[i].second >= cutoffs[i + 1].second && cutoffs[i].second > 4000.0f)
            peaks.push_back(cutoffs[i].first);
    }
    CHECK(highest > 6500.0f);
    CHECK(lowest < 160.0f);
    REQUIRE(peaks.size() >= 3);
    for (std::size_t p = 1; p < peaks.size(); ++p)
        CHECK_THAT(peaks[p] - peaks[p - 1], WithinAbs(0.5, 0.02));

    // At 90 BPM the same setting cycles every 2/3 s.
    peaks.clear();
    cutoffs.clear();
    runPlaying(*node, *filter, Stereo(at(2.5)), 90.0, true,
               [&](std::size_t done) { cutoffs.emplace_back(static_cast<double>(done) / kRate, filter->getModulatedCutoff()); });
    for (std::size_t i = 1; i + 1 < cutoffs.size(); ++i)
        if (cutoffs[i].second > cutoffs[i - 1].second && cutoffs[i].second >= cutoffs[i + 1].second && cutoffs[i].second > 4000.0f)
            peaks.push_back(cutoffs[i].first);
    REQUIRE(peaks.size() >= 3);
    CHECK_THAT(peaks[2] - peaks[1], WithinAbs(2.0 / 3.0, 0.02));

    // Envelope follower instead: a loud input opens a low-pass; quiet leaves it closed.
    setParameter(*filter, "lfoamt", 0.0f);
    setParameter(*filter, "freq", 300.0f);
    setParameter(*filter, "envamt", 100.0f);
    const auto out = run(*node, concat({tone(3000.0, at(0.3), 0.001), tone(3000.0, at(0.3), 0.5)}));
    const double quietGain = amplitudeAt(out.left, 3000.0, at(0.15), at(0.3)) / 0.001;
    const double loudGain = amplitudeAt(out.left, 3000.0, at(0.45), at(0.6)) / 0.5;
    CHECK(toDb(loudGain) - toDb(quietGain) > 20.0);
}

TEST_CASE("Auto Filter's stereo phase moves the channels apart", "[devices][autofilter]")
{
    auto [node, filter] = makeNode<AutoFilter>();
    setParameter(*filter, "freq", 1000.0f);
    setParameter(*filter, "lfoamt", 60.0f);
    setParameter(*filter, "lforate", 2.0f);
    setParameter(*filter, "lfoshape", 4.0f); // square
    setParameter(*filter, "lfostereo", 180.0f);
    const auto out = run(*node, tone(2000.0, at(0.25), 0.3));
    // The first half cycle: left open (+1.8 octaves), right closed.
    const double l = amplitudeAt(out.left, 2000.0, at(0.05), at(0.2));
    const double r = amplitudeAt(out.right, 2000.0, at(0.05), at(0.2));
    CHECK(toDb(l) - toDb(r) > 15.0);
}

//==================================================================================================
// Channel EQ

TEST_CASE("Channel EQ: flat at zero, console bands, low cut and output", "[devices][channeleq]")
{
    auto [node, eq] = makeNode<ChannelEq>();
    const auto measure = [&](double f)
    {
        const auto out = run(*node, tone(f, at(0.4), 0.1));
        return toDb(amplitudeAt(out.left, f, at(0.2), at(0.4)) / 0.1);
    };
    for (const double f : {40.0, 1000.0, 12000.0})
        CHECK_THAT(measure(f), WithinAbs(0.0, 0.05));

    setParameter(*eq, "low", 12.0f);
    CHECK(measure(40.0) > 9.0);
    CHECK_THAT(measure(40.0), WithinAbs(eq->getResponseDb(40.0), 0.3));
    CHECK(std::abs(measure(5000.0)) < 0.3);
    setParameter(*eq, "low", 0.0f);

    setParameter(*eq, "midfreq", 2000.0f);
    setParameter(*eq, "mid", -12.0f);
    CHECK_THAT(measure(2000.0), WithinAbs(-12.0, 0.3));
    CHECK(std::abs(measure(100.0)) < 0.5);
    // Proportional Q: big moves are narrower than small ones.
    CHECK(ChannelEq::midQ(12.0) > ChannelEq::midQ(3.0) * 1.5);
    setParameter(*eq, "mid", 0.0f);

    setParameter(*eq, "high", 9.0f);
    CHECK_THAT(measure(15000.0), WithinAbs(eq->getResponseDb(15000.0), 0.3));
    CHECK(measure(15000.0) > 7.5);
    setParameter(*eq, "high", 0.0f);

    setParameter(*eq, "lowcut", 1.0f);
    CHECK(measure(30.0) < -15.0);
    CHECK(std::abs(measure(1000.0)) < 0.2);

    setParameter(*eq, "output", -6.0f);
    CHECK_THAT(measure(1000.0), WithinAbs(-6.0, 0.2));
}

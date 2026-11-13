#include "devices/PlateReverb.h"
#include "devices/StudioDynamics.h"
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
TEST_CASE("De-Esser turns down loud highs only, in split or wide mode", "[devices][deesser]")
{
    const auto defaults = [](devices::DeEsser&) {};
    // A loud "s" at 12 kHz: down by about the full range (8 dB); at 9 kHz, closer to the 6.5 kHz split,
    // part of it is still below the crossover.
    const double sibilance = toneGainDb<devices::DeEsser>(12000.0, -6.0, defaults);
    CHECK(sibilance < -6.5);
    CHECK(sibilance > -8.5);
    CHECK(toneGainDb<devices::DeEsser>(9000.0, -6.0, defaults) < -4.5);
    // The body of the voice at 1 kHz, as loud: untouched (the split is flat).
    CHECK_THAT(toneGainDb<devices::DeEsser>(1000.0, -6.0, defaults), WithinAbs(0.0, 0.2));
    // Quiet air at 9 kHz (under the threshold): untouched.
    CHECK_THAT(toneGainDb<devices::DeEsser>(9000.0, -40.0, defaults), WithinAbs(0.0, 0.2));
    // Range limits it; Threshold sets where it starts.
    CHECK(toneGainDb<devices::DeEsser>(9000.0, -6.0, [](auto& d) { setParameter(d, "range", 3.0f); }) > -3.3);
    CHECK_THAT(toneGainDb<devices::DeEsser>(9000.0, -6.0, [](auto& d) { setParameter(d, "threshold", 0.0f); }), WithinAbs(0.0, 0.3));

    // Wide: while the "s" sounds, everything comes down, the body too.
    {
        auto [node, deesser] = makeNode<devices::DeEsser>();
        setParameter(*deesser, "mode", 1.0f);
        const auto n = at(1.0);
        const auto in = sum(tone(1000.0, n, 0.3), tone(9000.0, n, 0.4));
        const auto out = run(*node, in);
        CHECK(toDb(amplitudeAt(out.left, 1000.0, n / 2, n) / amplitudeAt(in.left, 1000.0, n / 2, n)) < -6.0);
        CHECK(deesser->takeReductionDb() > 6.0f);
    }
    // Split: the same mix keeps its body.
    {
        auto [node, deesser] = makeNode<devices::DeEsser>();
        const auto n = at(1.0);
        const auto in = sum(tone(1000.0, n, 0.3), tone(9000.0, n, 0.4));
        const auto out = run(*node, in);
        CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, n / 2, n) / amplitudeAt(in.left, 1000.0, n / 2, n)), WithinAbs(0.0, 0.3));
        CHECK(toDb(amplitudeAt(out.left, 9000.0, n / 2, n) / amplitudeAt(in.left, 9000.0, n / 2, n)) < -4.5);
    }
    // Listen: what the detector hears (the highs only).
    CHECK(toneGainDb<devices::DeEsser>(1000.0, -6.0, [](auto& d) { setParameter(d, "listen", 1.0f); }) < -40.0);
    CHECK(toneGainDb<devices::DeEsser>(12000.0, -6.0, [](auto& d) { setParameter(d, "listen", 1.0f); }) > -3.0);
    // Band-pass detection ignores what is far above the frequency.
    CHECK_THAT(toneGainDb<devices::DeEsser>(18000.0, -20.0, [](auto& d)
                                            {
                                                setParameter(d, "detection", 1.0f);
                                                setParameter(d, "freq", 4000.0f);
                                            }),
               WithinAbs(0.0, 0.5));
}

//==================================================================================================
TEST_CASE("Glue Compressor: stepped ratios, range, parallel mix, auto release and soft clip", "[devices][glue]")
{
    const auto at4 = [](devices::GlueCompressor& c)
    {
        setParameter(c, "threshold", -20.0f);
        setParameter(c, "attack", 2.0f);  // 0.3 ms
        setParameter(c, "release", 0.0f); // 0.1 s
    };
    // 14 dB over the threshold at 4:1: about 10.5 dB of reduction.
    const double four = toneGainDb<devices::GlueCompressor>(1000.0, -6.0, at4);
    CHECK(four < -9.0);
    CHECK(four > -12.0);
    // 10:1: more.
    const double ten = toneGainDb<devices::GlueCompressor>(1000.0, -6.0, [&](auto& c)
                                                           {
                                                               at4(c);
                                                               setParameter(c, "ratio", 2.0f);
                                                           });
    CHECK(ten < four - 1.5);
    // Range caps the reduction.
    CHECK(toneGainDb<devices::GlueCompressor>(1000.0, -6.0, [&](auto& c)
                                              {
                                                  at4(c);
                                                  setParameter(c, "range", 3.0f);
                                              }) > -3.3);
    // Below the threshold: nothing.
    CHECK_THAT(toneGainDb<devices::GlueCompressor>(1000.0, -30.0, at4), WithinAbs(0.0, 0.1));
    // Dry/Wet 0: dry.
    CHECK_THAT(toneGainDb<devices::GlueCompressor>(1000.0, -6.0, [&](auto& c)
                                                   {
                                                       at4(c);
                                                       setParameter(c, "mix", 0.0f);
                                                   }),
               WithinAbs(0.0, 1e-6));
    // The sidechain high-pass keeps a loud bass from pumping it.
    const double bass = toneGainDb<devices::GlueCompressor>(50.0, -6.0, at4);
    const double filtered = toneGainDb<devices::GlueCompressor>(50.0, -6.0, [&](auto& c)
                                                                {
                                                                    at4(c);
                                                                    setParameter(c, "schp", 300.0f);
                                                                });
    CHECK(filtered > bass + 6.0);

    // Auto release: quick after a short hit, slow after sustained compression.
    const auto reductionAfter = [](double burstSeconds)
    {
        auto [node, glue] = makeNode<devices::GlueCompressor>();
        setParameter(*glue, "threshold", -20.0f);
        setParameter(*glue, "attack", 3.0f); // 1 ms
        setParameter(*glue, "release", 6.0f); // Auto
        const auto loud = tone(1000.0, at(burstSeconds), 0.5);
        const auto quiet = tone(1000.0, at(0.3), level(-40.0));
        const auto in = concat({loud, quiet});
        const auto out = run(*node, in);
        const auto from = at(burstSeconds) + at(0.25);
        return toDb(amplitudeAt(in.left, 1000.0, from, from + at(0.05)) / amplitudeAt(out.left, 1000.0, from, from + at(0.05)));
    };
    const double afterShort = reductionAfter(0.03);
    const double afterLong = reductionAfter(2.0);
    INFO("after a short hit " << afterShort << " dB, after a long one " << afterLong << " dB");
    CHECK(afterShort < 2.5);
    CHECK(afterLong > afterShort + 3.0);

    // Soft clip: peaks rounded under -0.5 dBFS even with 20 dB of makeup.
    {
        auto [node, glue] = makeNode<devices::GlueCompressor>();
        setParameter(*glue, "threshold", 0.0f);
        setParameter(*glue, "makeup", 20.0f);
        setParameter(*glue, "softclip", 1.0f);
        const auto out = run(*node, tone(1000.0, at(0.5), 0.5));
        CHECK(peakOf(out.left) <= devices::GlueCompressor::kSoftClipCeiling + 1e-9);
        CHECK(peakOf(out.left) > 0.9);
    }
}

//==================================================================================================
TEST_CASE("Expander lowers what is under the threshold by its ratio, at most by its range", "[devices][expander]")
{
    const auto hard = [](devices::Expander& e)
    {
        setParameter(e, "knee", 0.0f);
        setParameter(e, "threshold", -40.0f);
        setParameter(e, "ratio", 2.0f);
        setParameter(e, "range", 20.0f);
    };
    CHECK_THAT(toneGainDb<devices::Expander>(1000.0, -20.0, hard), WithinAbs(0.0, 0.1));   // above: untouched
    CHECK_THAT(toneGainDb<devices::Expander>(1000.0, -50.0, hard), WithinAbs(-10.0, 1.0)); // 10 dB under: 10 dB more
    CHECK_THAT(toneGainDb<devices::Expander>(1000.0, -70.0, hard), WithinAbs(-20.0, 1.0)); // capped by the range
    CHECK_THAT(toneGainDb<devices::Expander>(1000.0, -50.0, [&](auto& e)
                                             {
                                                 hard(e);
                                                 setParameter(e, "ratio", 1.0f);
                                             }),
               WithinAbs(0.0, 0.1));

    // The static curve the editor draws.
    devices::Expander expander;
    setParameter(expander, "knee", 0.0f);
    CHECK_THAT(expander.attenuationFor(-50.0), WithinAbs(10.0, 1e-6));
    CHECK_THAT(expander.attenuationFor(-30.0), WithinAbs(0.0, 1e-6));

    // An external key: loud main, silent key: fully down by the range.
    {
        auto [node, e] = makeNode<devices::Expander>();
        hard(*e);
        setParameter(*e, "sidechain", 1.0f);
        const auto n = at(1.0);
        const auto in = tone(1000.0, n, 0.5);
        const Stereo key(n);
        const auto out = run(*node, in, &key);
        CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, n / 2, n) / amplitudeAt(in.left, 1000.0, n / 2, n)), WithinAbs(-20.0, 0.5));
    }
}

//==================================================================================================
TEST_CASE("Soft Clipper never passes its ceiling and keeps its dry signal aligned", "[devices][clipper]")
{
    using devices::SoftClipper;
    // The curves: unchanged below the knee, never past 1, monotonic.
    for (int shape = 0; shape < 3; ++shape)
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
    CHECK_THAT(SoftClipper::unitShape(2.0, 2, 0.5), WithinAbs(1.0, 1e-12));  // hard
    CHECK(SoftClipper::unitShape(0.8, 0, 0.5) < 0.8);                        // soft rounds
    CHECK(SoftClipper::unitShape(0.8, 1, 0.5) < SoftClipper::unitShape(0.8, 0, 0.5) + 1e-9); // smooth is gentler

    for (int oversampling = 0; oversampling < 3; ++oversampling)
        for (int shape = 0; shape < 3; ++shape)
        {
            INFO("oversampling " << oversampling << ", shape " << shape);
            auto [node, clipper] = makeNode<SoftClipper>();
            setParameter(*clipper, "oversampling", static_cast<float>(oversampling));
            setParameter(*clipper, "shape", static_cast<float>(shape));
            setParameter(*clipper, "drive", 12.0f);
            setParameter(*clipper, "ceiling", -1.0f);
            node->prepare(kRate, kBlock, nullptr);
            const auto out = run(*node, sum(tone(110.0, at(0.5), 0.6), tone(3500.0, at(0.5), 0.3)));
            CHECK(allFinite(out));
            CHECK(peakOf(out.left) <= level(-1.0) + 1e-9);
            CHECK(clipper->takeClipDb() > 6.0f);
        }

    // Latency: none without oversampling, more at 4x than at 2x; it follows the setting.
    {
        auto [node, clipper] = makeNode<SoftClipper>();
        setParameter(*clipper, "oversampling", 0.0f);
        CHECK(clipper->getLatencySamples() == 0);
        setParameter(*clipper, "oversampling", 1.0f);
        const int two = clipper->getLatencySamples();
        setParameter(*clipper, "oversampling", 2.0f);
        const int four = clipper->getLatencySamples();
        CHECK(two > 0);
        CHECK(four > two);
    }

    // Half wet on a quiet signal (no clipping): the dry half lines up with the wet half (no combing).
    for (const double f : {500.0, 5000.0, 12000.0})
    {
        INFO(f);
        CHECK_THAT(toneGainDb<SoftClipper>(f, -30.0, [](auto& c) { setParameter(c, "mix", 50.0f); }), WithinAbs(0.0, 0.2));
    }
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

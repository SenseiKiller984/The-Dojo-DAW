// The EMI TG12345 console device: its cassettes, cards, transformers, dynamics, EQ, pan-pots,
// echo loop and monitoring, against the console's specification.

#include "devices/ConsoleTG12345.h"
#include "ui/devices/ConsoleEditors.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using devices::ConsoleTG12345;

namespace
{
double level(double db)
{
    return std::pow(10.0, db / 20.0);
}

using Setup = std::function<void(ConsoleTG12345&)>;

/** A fresh console device, set up, run over `input` (optionally with a talkback signal). */
Stereo through(const Stereo& input, const Setup& setup = {}, const Stereo* talkback = nullptr)
{
    auto [node, tg] = makeNode<ConsoleTG12345>();
    if (setup)
        setup(*tg);
    return run(*node, input, talkback);
}

/** Gain (dB) at `frequency` through the device, a small tone in both sides. */
double gainAt(double frequency, const Setup& setup = {}, int channel = 0)
{
    const auto out = through(tone(frequency, at(0.5), level(-30.0)), setup);
    const auto& x = channel == 0 ? out.left : out.right;
    return toDb(amplitudeAt(x, frequency, at(0.3), at(0.5)) / level(-30.0));
}

/** Total harmonic distortion (%) of the left output, harmonics 2 to 7. */
double thdOf(const Stereo& out, double f, std::size_t from, std::size_t to)
{
    double harmonics = 0.0;
    for (int k = 2; k <= 7; ++k)
        harmonics += std::pow(amplitudeAt(out.left, f * k, from, to), 2.0);
    return 100.0 * std::sqrt(harmonics) / amplitudeAt(out.left, f, from, to);
}

/** Runs the device in the engine's two halves around its insert loop (`inLoop` is the loop). */
using InLoop = std::function<void(juce::AudioBuffer<double>& buffer, std::size_t offset, int n)>;
Stereo runSplit(ProcessorNode& node, const Stereo& input, const InLoop& inLoop, int loopLatency = 0)
{
    const auto total = input.left.size();
    Stereo out(total);
    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t offset = 0; offset < total; offset += kBlock)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, total - offset));
        for (int i = 0; i < n; ++i)
        {
            buffer.setSample(0, i, input.left[offset + static_cast<std::size_t>(i)]);
            buffer.setSample(1, i, input.right[offset + static_cast<std::size_t>(i)]);
        }
        node.processSend(buffer, n, midi);
        if (inLoop)
            inLoop(buffer, offset, n);
        node.processReturn(buffer, n, loopLatency);
        for (int i = 0; i < n; ++i)
        {
            out.left[offset + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
            out.right[offset + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
        }
    }
    return out;
}

Stereo leftOnly(const Stereo& s)
{
    Stereo out = s;
    std::fill(out.right.begin(), out.right.end(), 0.0);
    return out;
}
} // namespace

TEST_CASE("TG12345: unity at +4 dBu, under 0.1 % THD, 20 Hz - 20 kHz within 0.5 dB, +24 dBu into 600 ohms", "[devices][console]")
{
    // +4 dBu (-18 dBFS) through a Microphone cassette with everything flat comes out at +4 dBu.
    const auto nominal = through(tone(1000.0, at(0.5), level(-18.0)));
    CHECK_THAT(toDb(amplitudeAt(nominal.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-18.0, 0.05));
    CHECK_THAT(toDb(amplitudeAt(nominal.right, 1000.0, at(0.3), at(0.5))), WithinAbs(-18.0, 0.05));
    const double thd = thdOf(nominal, 1000.0, at(0.3), at(0.5));
    INFO("THD at +4 dBu: " << thd << " %");
    CHECK(thd < 0.1);
    CHECK(thd > 0.01); // (the transistor cards are not perfectly linear)

    // The frequency response.
    const double reference = gainAt(1000.0);
    for (const double f : {20.0, 40.0, 100.0, 5000.0, 10000.0, 16000.0, 20000.0})
    {
        INFO(f << " Hz");
        CHECK_THAT(gainAt(f) - reference, WithinAbs(0.0, 0.5));
    }

    // The output drivers: +24 dBu (+2 dBFS) into 600 ohms, rounding off at the rails.
    const auto setHot = [](ConsoleTG12345& tg) { setParameter(tg, "gain", 6.0f); }; // +18 dB
    const auto hot = through(tone(1000.0, at(0.5), level(-12.0)), setHot);              // asks for +10 dBFS
    CHECK(peakOf(hot.left, at(0.2), at(0.5)) < level(ConsoleTG12345::kMaxOutputDbu - 22.0) * 1.01);
    CHECK(peakOf(hot.left, at(0.2), at(0.5)) > level(ConsoleTG12345::kMaxOutputDbu - 22.0) * 0.9);
    // Clean up to near the rails: +20 dBu out stays under 1 %.
    const auto loud = through(tone(1000.0, at(0.5), level(-2.0)));
    CHECK(thdOf(loud, 1000.0, at(0.3), at(0.5)) < 1.0);
}

TEST_CASE("TG12345: the input gain stud switches step in exact 6 dB and 2 dB steps", "[devices][console]")
{
    const auto gainWith = [](float coarse, float fine)
    {
        return gainAt(1000.0,
                      [&](ConsoleTG12345& tg)
                      {
                          setParameter(tg, "gain", coarse);
                          setParameter(tg, "fine", fine);
                      }) -
               gainAt(1000.0);
    };
    CHECK_THAT(gainWith(4.0f, 2.0f), WithinAbs(6.0, 0.02));
    CHECK_THAT(gainWith(2.0f, 2.0f), WithinAbs(-6.0, 0.02));
    CHECK_THAT(gainWith(3.0f, 3.0f), WithinAbs(2.0, 0.02));
    CHECK_THAT(gainWith(3.0f, 0.0f), WithinAbs(-4.0, 0.02));
    CHECK_THAT(gainWith(5.0f, 4.0f), WithinAbs(16.0, 0.03));
    CHECK(ConsoleTG12345::kGainSteps.front() == -18.0);
    CHECK(ConsoleTG12345::kGainSteps.back() == 30.0);

    // A switch moving under signal does not click: the step glides in a few milliseconds.
    auto [node, tg] = makeNode<ConsoleTG12345>();
    const auto before = run(*node, tone(200.0, at(0.2), level(-18.0)));
    setParameter(*tg, "gain", 5.0f); // +12 dB
    auto later = tone(200.0, at(0.4), level(-18.0));
    later.left.erase(later.left.begin(), later.left.begin() + static_cast<std::ptrdiff_t>(at(0.2)));
    later.right.erase(later.right.begin(), later.right.begin() + static_cast<std::ptrdiff_t>(at(0.2)));
    const auto after = run(*node, later);
    double jump = std::abs(after.left[0] - before.left.back());
    for (std::size_t i = 1; i < 400; ++i)
        jump = std::max(jump, std::abs(after.left[i] - after.left[i - 1]));
    CHECK(jump < 0.05); // (a 200 Hz sine at +12 dB moves at most ~0.013 per sample: no step)
}

TEST_CASE("TG12345: the mic transformer saturates in the deep lows when driven hard", "[devices][console]")
{
    const auto at30 = through(tone(30.0, at(1.0), level(-4.0)));
    const auto at1k = through(tone(1000.0, at(1.0), level(-4.0)));
    const double low = thdOf(at30, 30.0, at(0.5), at(1.0));
    const double mid = thdOf(at1k, 1000.0, at(0.5), at(1.0));
    INFO("30 Hz: " << low << " %, 1 kHz: " << mid << " %");
    CHECK(low > 1.0);
    CHECK(low > 3.0 * mid);
    // At +4 dBu even 30 Hz stays clean.
    CHECK(thdOf(through(tone(30.0, at(1.0), level(-18.0))), 30.0, at(0.5), at(1.0)) < 0.2);
}

TEST_CASE("TG12345: compressor 2:1 and limiter 7:1 over the Hold threshold, 1 ms attack, stepped recovery", "[devices][console]")
{
    const auto outputDb = [](float mode, float hold, double inputDb)
    {
        const auto out = through(tone(1000.0, at(1.0), level(inputDb)),
                                 [&](ConsoleTG12345& tg)
                                 {
                                     setParameter(tg, "mode", mode);
                                     setParameter(tg, "hold", hold);
                                 });
        return toDb(amplitudeAt(out.left, 1000.0, at(0.7), at(1.0)));
    };
    // Below the threshold (Hold 0: +4 dBu) nothing changes.
    CHECK_THAT(outputDb(1.0f, 0.0f, -24.0), WithinAbs(-24.0, 0.1));
    // 12 dB over: Comp (2:1) leaves 6 dB, Limit (7:1) 1.7 dB.
    CHECK_THAT(outputDb(1.0f, 0.0f, -6.0), WithinAbs(-12.0, 0.4));
    CHECK_THAT(outputDb(2.0f, 0.0f, -6.0), WithinAbs(-16.3, 0.4));
    // Hold moves the threshold: +6 dB, so 6 dB over leaves 3 dB.
    CHECK_THAT(outputDb(1.0f, 6.0f, -6.0), WithinAbs(-9.0, 0.4));
    // Out: no gain change at all.
    CHECK_THAT(outputDb(0.0f, 0.0f, -6.0), WithinAbs(-6.0, 0.1));

    // The gain reduction is shown.
    {
        auto [node, tg] = makeNode<ConsoleTG12345>();
        setParameter(*tg, "mode", 1.0f);
        (void) run(*node, tone(1000.0, at(0.3), level(-6.0)));
        CHECK_THAT(tg->getGainReductionDb(), WithinAbs(6.0, 0.6));
    }

    // Envelope (dB) of the output in 0.25 ms windows.
    const auto envelope = [](const Stereo& out, std::size_t from, std::size_t count)
    {
        std::vector<double> e;
        const std::size_t window = at(0.00025);
        for (std::size_t i = from; i + window <= from + count; i += window)
            e.push_back(toDb(peakOf(out.left, i, i + window)));
        return e;
    };

    // Attack: a step 20 dB over the threshold (a 10 kHz tone, so the rectifier sees a nearly
    // steady level) settles in about 1 ms (63 % of the way, in dB).
    {
        const auto input = concat({tone(10000.0, at(0.2), level(-40.0)), tone(10000.0, at(0.2), level(2.0))});
        auto [node, tg] = makeNode<ConsoleTG12345>();
        setParameter(*tg, "mode", 2.0f); // Limit
        const auto out = run(*node, input);
        const std::size_t step = at(0.2) + static_cast<std::size_t>(tg->getLatencySamples());
        const auto e = envelope(out, step, at(0.03));
        const double start = e[1], end = e.back();
        std::size_t k = 1;
        while (k < e.size() && e[k] - end > 0.37 * (start - end))
            ++k;
        const double seconds = static_cast<double>(k) * 0.00025;
        INFO("attack " << seconds * 1000.0 << " ms (from " << start << " to " << end << " dB)");
        CHECK(start - end > 10.0);
        CHECK(seconds > 0.0005);
        CHECK(seconds < 0.0016);
    }

    // Recovery: after the loud part, the gain comes back 90 % of the way in the switch's time.
    const auto recovery = [&](float position)
    {
        const auto input = concat({tone(1000.0, at(0.5), level(-6.0)), tone(1000.0, at(8.0), level(-30.0))});
        auto [node, tg] = makeNode<ConsoleTG12345>();
        setParameter(*tg, "mode", 1.0f);
        setParameter(*tg, "recovery", position);
        const auto out = run(*node, input);
        // The gain deficit against the input (-30 dBFS) in 10 ms windows after the drop.
        const std::size_t drop = at(0.5) + static_cast<std::size_t>(tg->getLatencySamples());
        const double first = -30.0 - toDb(amplitudeAt(out.left, 1000.0, drop + at(0.002), drop + at(0.012)));
        for (double t = 0.01; t < 7.5; t += 0.005)
        {
            const double deficit = -30.0 - toDb(amplitudeAt(out.left, 1000.0, drop + at(t), drop + at(t + 0.01)));
            if (deficit < 0.1 * first)
                return t + 0.005;
        }
        return 100.0;
    };
    const double fast = recovery(0.0), slow = recovery(3.0), slowest = recovery(5.0);
    INFO("recovery 100 ms: " << fast << " s, 1 s: " << slow << " s, 5 s: " << slowest << " s");
    CHECK_THAT(fast, WithinAbs(0.1, 0.03));
    CHECK_THAT(slow, WithinAbs(1.0, 0.15));
    CHECK_THAT(slowest, WithinAbs(5.0, 0.7));
}

TEST_CASE("TG12345: bass 50 Hz shelf, treble 5 kHz bell boost and 10 kHz shelf cut, presence on Mk II", "[devices][console]")
{
    const auto with = [](std::initializer_list<std::pair<const char*, float>> settings)
    {
        return [settings](ConsoleTG12345& tg)
        {
            for (const auto& [id, value] : settings)
                setParameter(tg, id, value);
        };
    };
    const double flat50 = gainAt(50.0), flat1k = gainAt(1000.0), flat5k = gainAt(5000.0), flat10k = gainAt(10000.0);
    // Bass: +-10 dB at 50 Hz (the stud switch is calibrated there), broad passive shelf.
    CHECK_THAT(gainAt(50.0, with({{"bass", 10.0f}})) - flat50, WithinAbs(10.0, 0.2));
    CHECK_THAT(gainAt(50.0, with({{"bass", 0.0f}})) - flat50, WithinAbs(-10.0, 0.2));
    CHECK_THAT(gainAt(50.0, with({{"bass", 7.0f}})) - flat50, WithinAbs(4.0, 0.2));
    CHECK(std::abs(gainAt(1000.0, with({{"bass", 10.0f}})) - flat1k) < 0.6);
    // Treble boost: a bell at 5 kHz.
    CHECK_THAT(gainAt(5000.0, with({{"treble", 10.0f}})) - flat5k, WithinAbs(10.0, 0.2));
    CHECK(gainAt(15000.0, with({{"treble", 10.0f}})) < gainAt(5000.0, with({{"treble", 10.0f}})) - 3.0);
    // Treble cut: a shelf calibrated at 10 kHz, still falling above.
    CHECK_THAT(gainAt(10000.0, with({{"treble", 0.0f}})) - flat10k, WithinAbs(-10.0, 0.2));
    CHECK(gainAt(18000.0, with({{"treble", 0.0f}})) < gainAt(10000.0, with({{"treble", 0.0f}})));
    CHECK(std::abs(gainAt(1000.0, with({{"treble", 0.0f}})) - flat1k) < 0.8);

    // Presence: not fitted on a Mk I channel; on a Mk II channel and on the Group cassette, a bell
    // at each of its switch's frequencies.
    CHECK(std::abs(gainAt(2800.0, with({{"presence", 10.0f}})) - gainAt(2800.0)) < 0.05);
    for (int f = 0; f < 8; ++f)
    {
        const double hz = ConsoleTG12345::kPresenceHz[static_cast<std::size_t>(f)];
        INFO(hz << " Hz");
        CHECK_THAT(gainAt(hz, with({{"mark", 1.0f}, {"presfreq", static_cast<float>(f)}, {"presence", 10.0f}})) - gainAt(hz),
                   WithinAbs(10.0, 0.25));
    }
    CHECK_THAT(gainAt(1200.0, with({{"mark", 1.0f}, {"presfreq", 2.0f}, {"presence", 0.0f}})) - gainAt(1200.0), WithinAbs(-10.0, 0.25));
    CHECK_THAT(gainAt(4200.0, with({{"cassette", 1.0f}, {"presfreq", 5.0f}, {"presence", 8.0f}})) -
                   gainAt(4200.0, with({{"cassette", 1.0f}})),
               WithinAbs(6.0, 0.25));
    // (the Group cassette has no bass or treble networks)
    CHECK(std::abs(gainAt(50.0, with({{"cassette", 1.0f}, {"bass", 10.0f}})) - gainAt(50.0, with({{"cassette", 1.0f}}))) < 0.05);
}

TEST_CASE("TG12345: dual reverse-connected 11 kOhm pan-pots, the Mk I spreader, the fader", "[devices][console]")
{
    // The pan law: each side's 11 kOhm pot into the bus's 22 kOhm mixing resistor.
    const auto [left, right] = ConsoleTG12345::panGains(0.0);
    CHECK_THAT(toDb(left), WithinAbs(-7.04, 0.02));
    CHECK_THAT(left, WithinAbs(right, 1e-12));
    CHECK(ConsoleTG12345::panGains(-1.0).first == 1.0);
    CHECK(ConsoleTG12345::panGains(-1.0).second == 0.0);
    CHECK(ConsoleTG12345::panGains(0.5).second > ConsoleTG12345::panGains(0.5).first);

    // Mono input, panned centre: both sides 7 dB down; panned L30: left louder.
    const auto mono = [](float pan)
    {
        return [pan](ConsoleTG12345& tg)
        {
            setParameter(tg, "input", 1.0f);
            setParameter(tg, "pana", pan);
        };
    };
    CHECK_THAT(gainAt(1000.0, mono(0.0f), 0), WithinAbs(-7.04, 0.05));
    CHECK_THAT(gainAt(1000.0, mono(0.0f), 1), WithinAbs(-7.04, 0.05));
    CHECK(gainAt(1000.0, mono(-30.0f), 0) > gainAt(1000.0, mono(-30.0f), 1) + 3.0);
    CHECK_THAT(gainAt(1000.0, mono(100.0f), 1), WithinAbs(0.0, 0.05));

    // The stereo pair: A left, B right by default: the track's own image.
    const auto pair = through(tone(1000.0, at(0.5), level(-20.0), 3000.0, level(-26.0)));
    CHECK_THAT(toDb(amplitudeAt(pair.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-20.0, 0.05));
    CHECK(amplitudeAt(pair.left, 3000.0, at(0.3), at(0.5)) < level(-80.0));
    CHECK_THAT(toDb(amplitudeAt(pair.right, 3000.0, at(0.3), at(0.5))), WithinAbs(-26.0, 0.05));

    // Spreader (Mk I): 0 % folds the pair to mono, 200 % doubles the side signal; not on a Mk II.
    const auto spread = [](float width, float mark)
    {
        return through(tone(1000.0, at(0.5), level(-20.0), 1000.0, 0.0),
                       [&](ConsoleTG12345& tg)
                       {
                           setParameter(tg, "spread", width);
                           setParameter(tg, "mark", mark);
                       });
    };
    const auto narrow = spread(0.0f, 0.0f);
    CHECK_THAT(amplitudeAt(narrow.left, 1000.0, at(0.3), at(0.5)), WithinAbs(amplitudeAt(narrow.right, 1000.0, at(0.3), at(0.5)), 1e-6));
    CHECK_THAT(toDb(amplitudeAt(narrow.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-26.02, 0.05));
    const auto wide = spread(200.0f, 0.0f); // L = 1.5 x, R = -0.5 x
    CHECK_THAT(toDb(amplitudeAt(wide.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-20.0 + 3.52, 0.05));
    CHECK_THAT(toDb(amplitudeAt(wide.right, 1000.0, at(0.3), at(0.5))), WithinAbs(-26.02, 0.05));
    const auto markTwo = spread(0.0f, 1.0f);
    CHECK(amplitudeAt(markTwo.right, 1000.0, at(0.3), at(0.5)) < level(-80.0));

    // The fader: off is silence; +10 dB at the top.
    CHECK(peakOf(through(tone(1000.0, at(0.3), level(-20.0)), [](ConsoleTG12345& tg) { setParameter(tg, "fader", -80.0f); }).left,
                 at(0.1), at(0.3)) < 1e-4); // (-80 dB: the output transformer's last breath of the fade)
    CHECK_THAT(gainAt(1000.0, [](ConsoleTG12345& tg) { setParameter(tg, "fader", 10.0f); }), WithinAbs(10.0, 0.05));
    CHECK_THAT(gainAt(1000.0, [](ConsoleTG12345& tg) { setParameter(tg, "fader", -12.5f); }), WithinAbs(-12.5, 0.05));
}

TEST_CASE("TG12345: the echo send feeds the insert loop; its return mixes in, in time with the channel", "[devices][console]")
{
    const auto input = leftOnly(tone(1000.0, at(0.4), level(-20.0)));
    const auto setUp = [](ConsoleTG12345& tg, float send, float fader, bool pre)
    {
        setParameter(tg, "echosend", send);
        setParameter(tg, "echoreturn", 0.0f);
        setParameter(tg, "fader", fader);
        setParameter(tg, "echopre", pre ? 1.0f : 0.0f);
    };
    // With nothing in the loop (the engine runs the device whole) there is no echo.
    {
        const auto whole = through(input, [&](ConsoleTG12345& tg) { setUp(tg, 0.0f, 0.0f, false); });
        CHECK(amplitudeAt(whole.right, 1000.0, at(0.2), at(0.4)) < level(-80.0));
    }
    // A loop that halves the send: the right side (the channel is panned left) carries only the
    // echo: half the send, the send being the channel at its echo send level, after the fader.
    const auto echoLevel = [&](float send, float fader, bool pre)
    {
        auto [node, tg] = makeNode<ConsoleTG12345>();
        setUp(*tg, send, fader, pre);
        CHECK(node->isLoopEngaged());
        const auto out = runSplit(*node, input, [](juce::AudioBuffer<double>& b, std::size_t, int n) { b.applyGain(0, n, 0.5); });
        return toDb(amplitudeAt(out.right, 1000.0, at(0.2), at(0.4)));
    };
    CHECK_THAT(echoLevel(0.0f, 0.0f, false), WithinAbs(-26.02, 0.05));
    CHECK_THAT(echoLevel(-6.0f, 0.0f, false), WithinAbs(-32.02, 0.05));
    CHECK_THAT(echoLevel(0.0f, -10.0f, false), WithinAbs(-36.02, 0.05)); // post-fader: follows the fader
    CHECK_THAT(echoLevel(0.0f, -10.0f, true), WithinAbs(-26.02, 0.05));  // pre-fader: does not
    CHECK(echoLevel(-80.0f, 0.0f, false) < -150.0);

    // The loop's latency: the channel waits for its echo, so a click and its echo meet.
    {
        Stereo click(at(0.2));
        click.left[at(0.05)] = 0.5;
        auto [node, tg] = makeNode<ConsoleTG12345>();
        setUp(*tg, 0.0f, 0.0f, false);
        constexpr int kDelay = 300;
        std::vector<double> line(kDelay, 0.0); // (a loop device with 300 samples of latency)
        std::size_t write = 0;
        const auto out = runSplit(*node, click,
                                  [&](juce::AudioBuffer<double>& b, std::size_t, int n)
                                  {
                                      for (int i = 0; i < n; ++i)
                                      {
                                          const double in = b.getSample(0, i);
                                          b.setSample(0, i, line[write]);
                                          b.setSample(1, i, line[write]);
                                          line[write] = in;
                                          write = (write + 1) % line.size();
                                      }
                                  },
                                  kDelay);
        const auto peakLeft = std::max_element(out.left.begin(), out.left.end(), [](double a, double b) { return std::abs(a) < std::abs(b); });
        const auto peakRight = std::max_element(out.right.begin(), out.right.end(), [](double a, double b) { return std::abs(a) < std::abs(b); });
        CHECK(std::distance(out.left.begin(), peakLeft) == std::distance(out.right.begin(), peakRight));
        const auto expected = static_cast<std::ptrdiff_t>(at(0.05)) + kDelay + tg->getLatencySamples();
        CHECK(std::abs(std::distance(out.left.begin(), peakLeft) - expected) <= 2); // (the filters' peak, near their delay)
    }

    // The loop is offered by the Microphone and Echo / Aux cassettes only.
    ConsoleTG12345 tg;
    CHECK(tg.isLoopAvailable());
    CHECK_FALSE(tg.isLoopInSeries()); // (in parallel: an empty send returns nothing)
    CHECK(tg.getLoopName() == "Echo Send");
    for (const int cassette : {1, 2, 4})
    {
        setParameter(tg, "cassette", static_cast<float>(cassette));
        CHECK_FALSE(tg.isLoopAvailable());
        CHECK_FALSE(tg.isLoopEngaged());
    }
    setParameter(tg, "cassette", 3.0f);
    CHECK(tg.isLoopAvailable());
    CHECK(tg.isLoopInSeries());
    CHECK(tg.getLoopName() == "Echo Chamber");
}

TEST_CASE("TG12345: the Group, Main (track announce) and Echo / Aux cassettes", "[devices][console]")
{
    // Group: the sub-group's gain, compressor and fader on the stereo bus.
    const auto group = [](ConsoleTG12345& tg) { setParameter(tg, "cassette", 1.0f); };
    CHECK_THAT(gainAt(1000.0, group), WithinAbs(0.0, 0.05));
    CHECK_THAT(gainAt(1000.0, group, 1), WithinAbs(0.0, 0.05)); // (no pan-pots: the bus stays stereo)
    {
        const auto out = through(tone(1000.0, at(1.0), level(-6.0)),
                                 [](ConsoleTG12345& tg)
                                 {
                                     setParameter(tg, "cassette", 1.0f);
                                     setParameter(tg, "mode", 1.0f);
                                     setParameter(tg, "fader", -3.0f);
                                 });
        CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, at(0.7), at(1.0))), WithinAbs(-15.0, 0.4));
    }

    // Main: the mix amplifier and main fader; Track Announce puts the talkback mic on the outputs.
    const auto program = tone(1000.0, at(0.5), level(-20.0));
    const auto voice = tone(300.0, at(0.5), level(-24.0));
    const auto announce = [&](bool on, float source)
    {
        return through(program,
                       [&](ConsoleTG12345& tg)
                       {
                           setParameter(tg, "cassette", 2.0f);
                           setParameter(tg, "announce", on ? 1.0f : 0.0f);
                           setParameter(tg, "annsource", source);
                       },
                       &voice);
    };
    const auto quiet = announce(false, 0.0f);
    CHECK(amplitudeAt(quiet.left, 300.0, at(0.3), at(0.5)) < level(-90.0));
    CHECK_THAT(toDb(amplitudeAt(quiet.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-20.0, 0.05));
    const auto talking = announce(true, 0.0f);
    CHECK_THAT(toDb(amplitudeAt(talking.left, 300.0, at(0.3), at(0.5))), WithinAbs(-24.0, 0.1));
    CHECK_THAT(toDb(amplitudeAt(talking.right, 300.0, at(0.3), at(0.5))), WithinAbs(-24.0, 0.1));
    // ... or a 1 kHz tone at +4 dBu (-18 dBFS) on top of the program.
    const auto tone1k = through(Stereo(at(0.5)),
                                [](ConsoleTG12345& tg)
                                {
                                    setParameter(tg, "cassette", 2.0f);
                                    setParameter(tg, "announce", 1.0f);
                                    setParameter(tg, "annsource", 1.0f);
                                });
    CHECK_THAT(toDb(amplitudeAt(tone1k.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-18.0, 0.1));
    {
        // The display monitors the outputs: their peak level in dBu and their correlation.
        auto [node, tg] = makeNode<ConsoleTG12345>();
        setParameter(*tg, "cassette", 2.0f);
        (void) run(*node, tone(1000.0, at(0.5), level(-18.0)));
        CHECK_THAT(tg->takeOutputPeakDbu(0), WithinAbs(4.0, 0.2));
        CHECK(tg->takeOutputPeakDbu(0) < -90.0f); // (taken)
        CHECK(tg->getCorrelation() > 0.99f);
        auto inverted = tone(1000.0, at(1.5), level(-18.0));
        for (auto& x : inverted.right)
            x = -x;
        (void) run(*node, inverted);
        CHECK(tg->getCorrelation() < -0.9f);
    }

    // Echo / Aux: with nothing in the chamber loop the send goes straight to the return.
    const auto echoCassette = [](float send, float ret)
    {
        return [send, ret](ConsoleTG12345& tg)
        {
            setParameter(tg, "cassette", 3.0f);
            setParameter(tg, "echosend", send);
            setParameter(tg, "echoreturn", ret);
        };
    };
    CHECK_THAT(gainAt(1000.0, echoCassette(-6.0f, 2.0f)), WithinAbs(-4.0, 0.05));
    // With a chamber: Send Master into it, Return Master out of it.
    {
        auto [node, tg] = makeNode<ConsoleTG12345>();
        echoCassette(-6.0f, -3.0f)(*tg);
        double sendPeak = 0.0;
        const auto out = runSplit(*node, tone(1000.0, at(0.4), level(-20.0)),
                                  [&](juce::AudioBuffer<double>& b, std::size_t offset, int n)
                                  {
                                      if (offset > at(0.2))
                                          sendPeak = std::max(sendPeak, b.getMagnitude(0, 0, n));
                                      b.applyGain(0, n, 0.5);
                                  });
        CHECK_THAT(toDb(sendPeak), WithinAbs(-26.0, 0.1));
        CHECK_THAT(toDb(amplitudeAt(out.left, 1000.0, at(0.2), at(0.4))), WithinAbs(-20.0 - 6.0 - 6.02 - 3.0, 0.1));
    }
}

TEST_CASE("TG12345: control room and studio monitoring with talkback; exports pass through", "[devices][console]")
{
    const auto program = tone(1000.0, at(0.5), level(-20.0), 1000.0, level(-26.0));
    const auto voice = tone(300.0, at(0.5), level(-24.0));
    const auto listen = [&](std::initializer_list<std::pair<const char*, float>> settings)
    {
        return through(program,
                       [settings](ConsoleTG12345& tg)
                       {
                           setParameter(tg, "cassette", 4.0f);
                           for (const auto& [id, value] : settings)
                               setParameter(tg, id, value);
                       },
                       &voice);
    };
    const auto levelOf = [](const std::vector<double>& x, double f) { return toDb(amplitudeAt(x, f, at(0.3), at(0.5))); };
    // The control room: its level, Dim (-20 dB), Mono, Cut.
    const auto plain = listen({});
    CHECK_THAT(levelOf(plain.left, 1000.0), WithinAbs(-20.0, 0.05));
    CHECK_THAT(levelOf(plain.right, 1000.0), WithinAbs(-26.0, 0.05));
    CHECK_THAT(levelOf(listen({{"crlevel", -6.0f}}).left, 1000.0), WithinAbs(-26.0, 0.05));
    CHECK_THAT(levelOf(listen({{"dim", 1.0f}}).left, 1000.0), WithinAbs(-40.0, 0.05));
    const auto mono = listen({{"mono", 1.0f}});
    CHECK_THAT(levelOf(mono.left, 1000.0), WithinAbs(levelOf(mono.right, 1000.0), 0.01));
    CHECK(peakOf(listen({{"cut", 1.0f}}).left, at(0.2), at(0.5)) < 1e-4);
    // Talkback: the control room dims; the studio hears the talkback with its playback.
    const auto talking = listen({{"talkback", 1.0f}});
    CHECK_THAT(levelOf(talking.left, 1000.0), WithinAbs(-40.0, 0.05));
    CHECK(amplitudeAt(talking.left, 300.0, at(0.3), at(0.5)) < level(-90.0));
    const auto studio = listen({{"listen", 1.0f}, {"talkback", 1.0f}, {"studiolevel", -10.0f}});
    CHECK_THAT(levelOf(studio.left, 1000.0), WithinAbs(-30.0, 0.05));
    CHECK_THAT(levelOf(studio.left, 300.0), WithinAbs(-24.0, 0.1));
    CHECK(amplitudeAt(listen({{"listen", 1.0f}}).left, 300.0, at(0.3), at(0.5)) < level(-90.0));

    // An offline export is not monitoring: the signal passes as it is.
    auto [node, tg] = makeNode<ConsoleTG12345>();
    setParameter(*tg, "cassette", 4.0f);
    setParameter(*tg, "crlevel", -12.0f);
    setParameter(*tg, "dim", 1.0f);
    node->setNonRealtime(true);
    const auto exported = run(*node, program);
    CHECK_THAT(toDb(amplitudeAt(exported.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-20.0, 0.05));
}

TEST_CASE("TG12345: oversampling, latency and saved settings", "[devices][console]")
{
    ConsoleTG12345 tg;
    tg.prepareToPlay(kRate, kBlock);
    CHECK(tg.getLatencySamples() == tg.latencyFor(false));
    CHECK(tg.latencyFor(false) > 0);
    tg.setNonRealtime(true);
    CHECK(tg.getLatencySamples() == tg.latencyFor(true));
    tg.setNonRealtime(false);
    setParameter(tg, "osrealtime", 0.0f);
    CHECK(tg.getLatencySamples() == 0);

    setParameter(tg, "cassette", 1.0f);
    setParameter(tg, "presence", 9.0f);
    setParameter(tg, "fader", -7.5f);
    juce::MemoryBlock state;
    tg.getStateInformation(state);
    ConsoleTG12345 other;
    other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(other.getCassette() == ConsoleTG12345::group);
    CHECK(other.presence_->getIndex() == 9);
    CHECK_THAT(other.fader_->get(), WithinAbs(-7.5, 1e-4));
    // Typed values.
    auto* pan = other.getParameterById("pana");
    CHECK(pan->getText(pan->convertTo0to1(0.0f), 10) == "C");
    CHECK_THAT(pan->convertFrom0to1(pan->getValueForText("L30")), WithinAbs(-30.0, 1e-3));
    auto* fader = other.getParameterById("fader");
    CHECK(fader->getText(0.0f, 10) == "Off");
    CHECK_THAT(fader->convertFrom0to1(fader->getValueForText("-inf")), WithinAbs(-80.0, 1e-3));
}

TEST_CASE("TG12345: its editor shows the controls of the cassette it is set to", "[ui][console]")
{
    juce::MessageManager::getInstance();
    ConsoleTG12345 tg;
    tg.prepareToPlay(kRate, kBlock);
    // Every cassette's pages name real controls; each cassette has what its circuits have.
    for (int cassette = 0; cassette < 5; ++cassette)
        for (const bool markTwo : {false, true})
            for (const bool stereo : {false, true})
            {
                ui::PagedEditor pages(tg, ui::ConsoleEditor::pagesFor(tg, cassette, markTwo, stereo));
                CHECK(pages.getMissingParameters().isEmpty());
            }
    const auto has = [&](int cassette, bool markTwo, bool stereo, const juce::String& id)
    {
        for (const auto& page : ui::ConsoleEditor::pagesFor(tg, cassette, markTwo, stereo))
            for (const auto& [parameter, caption] : page.parameters)
                if (parameter == id)
                    return true;
        return false;
    };
    CHECK(has(0, false, true, "spread"));
    CHECK_FALSE(has(0, false, true, "presence"));
    CHECK(has(0, true, true, "presence"));
    CHECK_FALSE(has(0, true, true, "spread"));
    CHECK_FALSE(has(0, false, false, "panb"));
    CHECK(has(1, false, true, "presence"));
    CHECK_FALSE(has(1, false, true, "bass"));
    CHECK(has(2, false, true, "announce"));
    CHECK(has(3, false, true, "echoreturn"));
    CHECK(has(4, false, true, "talkback"));
    CHECK_FALSE(has(4, false, true, "mode"));

    // The editor follows the cassette switch, and paints every page.
    ui::ConsoleEditor editor(tg);
    editor.setSize(ui::ConsoleEditor::preferredWidth(tg), 230);
    CHECK(editor.getPages().getPageTitle(0) == "Input");
    setParameter(tg, "cassette", 4.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil(500);
    CHECK(editor.getPages().getPageTitle(0) == "Control Room");
    for (int cassette = 0; cassette < 5; ++cassette)
    {
        setParameter(tg, "cassette", static_cast<float>(cassette));
        juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
        for (std::size_t p = 0; p < editor.getPages().getNumPages(); ++p)
        {
            editor.getPages().showPage(p);
            (void) editor.createComponentSnapshot(editor.getLocalBounds());
        }
    }
}

// The Harrison 4032 console device: its revisions A, B and C, the I/O module's in-line paths,
// filters, EQ, VCA and sends, the bus master and the master section, against the specification.

#include "devices/DeviceRegistry.h"
#include "devices/Harrison4032.h"
#include "session/Project.h"
#include "ui/DeviceActions.h"
#include "ui/EditorState.h"
#include "ui/devices/HarrisonEditors.h"
#include "EffectTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using devices::Harrison4032;

namespace
{
double level(double db)
{
    return std::pow(10.0, db / 20.0);
}

using Setup = std::function<void(Harrison4032&)>;
using Settings = std::initializer_list<std::pair<const char*, float>>;

Setup with(Settings settings)
{
    std::vector<std::pair<const char*, float>> list(settings);
    return [list](Harrison4032& h)
    {
        for (const auto& [id, value] : list)
            setParameter(h, id, value);
    };
}

/** Runs a fresh device over `input`; also gives the extra outputs (the sends) when asked. */
struct Result
{
    Stereo main;
    std::vector<std::vector<double>> extra; ///< per extra output channel
};
Result runFull(const Stereo& input, const Setup& setup = {}, const Stereo* tapeReturn = nullptr, bool offline = false)
{
    auto processor = std::make_unique<Harrison4032>();
    auto* h = processor.get();
    ProcessorNode node(std::move(processor));
    node.prepare(kRate, kBlock, nullptr);
    if (setup)
        setup(*h);
    node.setNonRealtime(offline);
    const auto total = input.left.size();
    Result result{Stereo(total), std::vector<std::vector<double>>(Harrison4032::Outputs::count, std::vector<double>(total, 0.0))};
    juce::AudioBuffer<double> buffer(2, kBlock), side(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t offset = 0; offset < total; offset += kBlock)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, total - offset));
        for (int i = 0; i < n; ++i)
        {
            const auto k = offset + static_cast<std::size_t>(i);
            buffer.setSample(0, i, input.left[k]);
            buffer.setSample(1, i, input.right[k]);
            if (tapeReturn != nullptr)
            {
                side.setSample(0, i, tapeReturn->left[k]);
                side.setSample(1, i, tapeReturn->right[k]);
            }
        }
        node.process(buffer, n, midi, tapeReturn != nullptr ? &side : nullptr);
        for (int i = 0; i < n; ++i)
        {
            const auto k = offset + static_cast<std::size_t>(i);
            result.main.left[k] = buffer.getSample(0, i);
            result.main.right[k] = buffer.getSample(1, i);
            for (int c = 0; c < Harrison4032::Outputs::count; ++c)
                if (const auto* aux = node.getAuxOutput(c))
                    result.extra[static_cast<std::size_t>(c)][k] = aux[i];
        }
    }
    return result;
}

Stereo through(const Stereo& input, const Setup& setup = {}, const Stereo* tapeReturn = nullptr)
{
    return runFull(input, setup, tapeReturn).main;
}

/** Gain (dB) at `frequency` through the device, a small tone in both sides. */
double gainAt(double frequency, const Setup& setup = {}, int channel = 0)
{
    const auto out = through(tone(frequency, at(0.5), level(-30.0)), setup);
    const auto& x = channel == 0 ? out.left : out.right;
    return toDb(amplitudeAt(x, frequency, at(0.3), at(0.5)) / level(-30.0));
}

double thdOf(const std::vector<double>& x, double f, std::size_t from, std::size_t to)
{
    double harmonics = 0.0;
    for (int k = 2; k <= 7; ++k)
        harmonics += std::pow(amplitudeAt(x, f * k, from, to), 2.0);
    return 100.0 * std::sqrt(harmonics) / amplitudeAt(x, f, from, to);
}

Setup revision(int r, Settings more = {})
{
    std::vector<std::pair<const char*, float>> list(more);
    return [r, list](Harrison4032& h)
    {
        setParameter(h, "revision", static_cast<float>(r));
        for (const auto& [id, value] : list)
            setParameter(h, id, value);
    };
}
} // namespace

TEST_CASE("Harrison 4032: unity at +4 dBu, under 0.02 % THD, 20 Hz - 20 kHz within 0.25 dB, no latency", "[devices][harrison]")
{
    std::array<double, 3> thd{};
    for (int r = 0; r < 3; ++r)
    {
        INFO("revision " << Harrison4032::revisionSpec(r).name);
        // The microphone input at MIC GAIN +40 dB: a track at -18 dBFS (a -36 dBu microphone) comes
        // out at +4 dBu (-18 dBFS).
        const auto nominal = through(tone(1000.0, at(0.5), level(-18.0)), revision(r));
        CHECK_THAT(toDb(amplitudeAt(nominal.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-18.0, 0.05));
        CHECK_THAT(toDb(amplitudeAt(nominal.right, 1000.0, at(0.3), at(0.5))), WithinAbs(-18.0, 0.05));
        thd[static_cast<std::size_t>(r)] = thdOf(nominal.left, 1000.0, at(0.3), at(0.5));
        CHECK(thd[static_cast<std::size_t>(r)] < 0.02);
        // Flat with the EQ and filters out.
        const double reference = gainAt(1000.0, revision(r));
        for (const double f : {20.0, 50.0, 200.0, 5000.0, 10000.0, 20000.0})
        {
            INFO(f << " Hz");
            CHECK_THAT(gainAt(f, revision(r)) - reference, WithinAbs(0.0, 0.25));
        }
    }
    INFO("THD A " << thd[0] << " %, B " << thd[1] << " %, C " << thd[2] << " %");
    CHECK(thd[0] > thd[1]);
    CHECK(thd[1] > thd[2]);
    Harrison4032 h;
    h.prepareToPlay(kRate, kBlock);
    CHECK(h.getLatencySamples() == 0);
}

TEST_CASE("Harrison 4032: headroom by revision: +22, +24.5 and +26 dBu; cleaner high levels on B and C", "[devices][harrison]")
{
    for (int r = 0; r < 3; ++r)
    {
        const auto& spec = Harrison4032::revisionSpec(r);
        INFO("revision " << spec.name);
        // Driven far past the rails (MIC GAIN 60 dB on a hot track): the peak stops at the rails.
        const auto hot = through(tone(500.0, at(0.4), level(-6.0)), revision(r, {{"micgain", 60.0f}}));
        const double rail = level(spec.maxOutputDbu - 22.0);
        CHECK(peakOf(hot.left, at(0.2), at(0.4)) < rail * 1.01);
        CHECK(peakOf(hot.left, at(0.2), at(0.4)) > rail * 0.95);
    }
    // At +20 dBu out the LM301A / +-15 V revision is grittier than the later ones.
    const auto at20 = [](int r)
    {
        const auto out = through(tone(1000.0, at(0.5), level(-2.0)), revision(r));
        return thdOf(out.left, 1000.0, at(0.3), at(0.5));
    };
    const double a = at20(0), b = at20(1), c = at20(2);
    INFO("+20 dBu THD: A " << a << " %, B " << b << " %, C " << c << " %");
    CHECK(a > 1.5 * b);
    CHECK(b > 1.5 * c);
    CHECK(c < 0.1);
    // The LM301A's slew rate: a full-level 20 kHz tone loses its peaks on A, not on C.
    const auto slew = [](int r)
    {
        const auto out = through(tone(20000.0, at(0.3), level(-1.0)), revision(r, {{"input", 1.0f}}));
        return toDb(amplitudeAt(out.left, 20000.0, at(0.1), at(0.3)));
    };
    CHECK(slew(0) < slew(2) - 0.3);
    CHECK_THAT(slew(2), WithinAbs(-1.0, 0.05));
}

TEST_CASE("Harrison 4032: mic gain +10 to +60 dB, pad, polarity, +48 V, line trim, EIN", "[devices][harrison]")
{
    const double unity = gainAt(1000.0);
    CHECK_THAT(gainAt(1000.0, with({{"micgain", 60.0f}})) - unity, WithinAbs(20.0, 0.05));
    CHECK_THAT(gainAt(1000.0, with({{"micgain", 10.0f}})) - unity, WithinAbs(-30.0, 0.05));
    CHECK_THAT(gainAt(1000.0, with({{"pad", 1.0f}})) - unity, WithinAbs(-20.0, 0.05));
    // Polarity: the microphone inverted.
    {
        const auto input = tone(200.0, at(0.3), level(-20.0));
        const auto plain = through(input), inverted = through(input, with({{"phase", 1.0f}}));
        double sum = 0.0, ref = 0.0;
        for (std::size_t i = at(0.1); i < at(0.3); ++i)
        {
            sum += std::abs(plain.left[i] + inverted.left[i]);
            ref += std::abs(plain.left[i]);
        }
        CHECK(sum < 1e-4 * ref);
    }
    // The line input: +4 dBu in, +4 dBu out; trim +-10 dB.
    CHECK_THAT(gainAt(1000.0, with({{"input", 1.0f}})), WithinAbs(0.0, 0.05));
    CHECK_THAT(gainAt(1000.0, with({{"input", 1.0f}, {"linetrim", -10.0f}})), WithinAbs(-10.0, 0.05));
    CHECK_THAT(gainAt(1000.0, with({{"input", 1.0f}, {"linetrim", 7.5f}})), WithinAbs(7.5, 0.05));

    // +48 V switched on: a pop on the microphone input (a -60 dBu step, dying away).
    {
        auto [node, h] = makeNode<Harrison4032>();
        (void) run(*node, Stereo(at(0.1)));
        setParameter(*h, "phantom", 1.0f);
        const auto out = run(*node, Stereo(at(0.3)));
        const double popDb = toDb(peakOf(out.left, 0, at(0.05)));
        INFO("pop " << popDb << " dBFS");
        CHECK(popDb > -50.0); // (-60 dBu + 40 dB = -20 dBu = -42 dBFS)
        CHECK(popDb < -35.0);
        CHECK(peakOf(out.left, at(0.25), at(0.3)) < level(-80.0)); // (gone)
    }

    // EIN: with Noise on, -128 dBu (C) at the mic input over 20 kHz: at MIC GAIN 60 dB the output
    // hiss is -68 dBu (-90 dBFS); the earlier revisions are noisier.
    const auto hiss = [](int r)
    {
        const auto out = through(Stereo(at(1.0)), revision(r, {{"noise", 1.0f}, {"micgain", 60.0f}}));
        return toDb(rmsOf(out.left, at(0.2), at(1.0)));
    };
    const double c = hiss(2), a = hiss(0);
    INFO("hiss A " << a << " dBFS, C " << c << " dBFS");
    CHECK_THAT(c, WithinAbs(-128.0 + 60.0 - 22.0 + 10.0 * std::log10(24000.0 / 20000.0), 0.5));
    CHECK(a > c + 1.0);
    // Off by default.
    CHECK(peakOf(through(Stereo(at(0.3)), with({{"micgain", 60.0f}})).left) < 1e-9);
}

TEST_CASE("Harrison 4032: HPF and LPF: 12 dB/oct, the C's 18 dB/oct HPF from 20 Hz, and their switching", "[devices][harrison]")
{
    const auto slope = [](int r, double hz)
    {
        const auto set = revision(r, {{"filtersin", 1.0f}, {"eqin", 1.0f}, {"hpf", static_cast<float>(hz)}});
        return gainAt(hz / 4.0, set) - gainAt(hz / 8.0, set); // (an octave, two below the corner)
    };
    CHECK_THAT(slope(0, 400.0), WithinAbs(12.0, 0.6));
    CHECK_THAT(slope(1, 400.0), WithinAbs(12.0, 0.6));
    CHECK_THAT(slope(2, 400.0), WithinAbs(18.0, 0.8));
    // -3 dB at the corner (Butterworth).
    CHECK_THAT(gainAt(400.0, revision(2, {{"filtersin", 1.0f}, {"hpf", 400.0f}})), WithinAbs(-3.0, 0.3));
    CHECK_THAT(gainAt(400.0, revision(1, {{"eqin", 1.0f}, {"hpf", 400.0f}})), WithinAbs(-3.0, 0.3));
    // The sweep: the C reaches down to 20 Hz, A and B stop at 25 Hz.
    CHECK_THAT(gainAt(20.0, revision(2, {{"filtersin", 1.0f}, {"hpf", 20.0f}})), WithinAbs(-3.0, 0.3));
    CHECK(gainAt(20.0, revision(0, {{"eqin", 1.0f}, {"hpf", 20.0f}})) < -4.0);
    // LPF: 12 dB/oct on every revision, from 160 Hz to 20 kHz.
    const auto lpf = [](int r, double hz) { return revision(r, {{"filtersin", 1.0f}, {"eqin", 1.0f}, {"lpf", static_cast<float>(hz)}}); };
    for (int r = 0; r < 3; ++r)
    {
        CHECK_THAT(gainAt(2000.0, lpf(r, 2000.0)), WithinAbs(-3.0, 0.3));
        CHECK_THAT(gainAt(2000.0, lpf(r, 500.0)) - gainAt(4000.0, lpf(r, 500.0)), WithinAbs(12.0, 0.6));
    }
    CHECK_THAT(gainAt(160.0, lpf(2, 160.0)), WithinAbs(-3.0, 0.3));

    // Switching: on A and B the filters come in with EQ IN; the C has FILTERS IN of its own.
    const auto hpfAt = [](int r, float filtersIn, float eqIn)
    { return gainAt(100.0, revision(r, {{"hpf", 400.0f}, {"filtersin", filtersIn}, {"eqin", eqIn}})); };
    CHECK(hpfAt(0, 1.0f, 0.0f) > -0.1); // (A: FILTERS IN does nothing)
    CHECK(hpfAt(0, 0.0f, 1.0f) < -20.0);
    CHECK(hpfAt(1, 0.0f, 1.0f) < -20.0);
    CHECK(hpfAt(2, 0.0f, 1.0f) > -0.1); // (C: EQ IN leaves the filters alone)
    CHECK(hpfAt(2, 1.0f, 0.0f) < -30.0);
}

TEST_CASE("Harrison 4032: the 4-band proportional-Q EQ: ranges, shelves, Q by revision, EQ IN", "[devices][harrison]")
{
    const auto band = [](int r, Settings settings)
    {
        std::vector<std::pair<const char*, float>> list(settings);
        list.emplace_back("eqin", 1.0f);
        return [r, list](Harrison4032& h)
        {
            setParameter(h, "revision", static_cast<float>(r));
            for (const auto& [id, value] : list)
                setParameter(h, id, value);
        };
    };
    // +-15 dB at each band's frequency, across its sweep. (On A and B the filters come in with the
    // EQ: the HPF, at its lowest 25 Hz, takes 0.6 dB at 40 Hz.)
    CHECK_THAT(gainAt(40.0, band(2, {{"lffreq", 40.0f}, {"lfgain", 15.0f}})), WithinAbs(15.0, 0.3));
    CHECK_THAT(gainAt(40.0, band(0, {{"lffreq", 40.0f}, {"lfgain", 15.0f}})), WithinAbs(15.0 - 0.62, 0.3));
    for (int r = 0; r < 3; ++r)
    {
        CHECK_THAT(gainAt(800.0, band(r, {{"lffreq", 800.0f}, {"lfgain", -15.0f}})), WithinAbs(-15.0, 0.3));
        CHECK_THAT(gainAt(80.0, band(r, {{"lmffreq", 80.0f}, {"lmfgain", 15.0f}})), WithinAbs(15.0, 0.3));
        CHECK_THAT(gainAt(1500.0, band(r, {{"lmffreq", 1500.0f}, {"lmfgain", -12.0f}})), WithinAbs(-12.0, 0.3));
        CHECK_THAT(gainAt(800.0, band(r, {{"hmffreq", 800.0f}, {"hmfgain", 9.0f}})), WithinAbs(9.0, 0.3));
        CHECK_THAT(gainAt(15000.0, band(r, {{"hmffreq", 15000.0f}, {"hmfgain", 15.0f}})), WithinAbs(15.0, 0.4));
        CHECK_THAT(gainAt(3000.0, band(r, {{"hffreq", 3000.0f}, {"hfgain", 15.0f}})), WithinAbs(15.0, 0.3));
    }
    // EQ IN out: nothing.
    CHECK(std::abs(gainAt(3000.0, with({{"hffreq", 3000.0f}, {"hfgain", 15.0f}}))) < 0.05);

    // Shelf / Peak: A has bells only; B and C shelve. A shelf keeps going above (below) its corner;
    // a bell comes back.
    const auto hfShelf = [&](int r) { return band(r, {{"hffreq", 2000.0f}, {"hfgain", 12.0f}, {"hfshelf", 1.0f}}); };
    CHECK(gainAt(12000.0, hfShelf(0)) < 4.0);
    CHECK_THAT(gainAt(12000.0, hfShelf(1)), WithinAbs(12.0, 0.5));
    CHECK_THAT(gainAt(12000.0, hfShelf(2)), WithinAbs(12.0, 0.5));
    const auto lfShelf = [&](int r) { return band(r, {{"lffreq", 400.0f}, {"lfgain", -10.0f}, {"lfshelf", 1.0f}}); };
    CHECK(gainAt(40.0, lfShelf(0)) > -3.0);
    CHECK_THAT(gainAt(40.0, lfShelf(2)), WithinAbs(-10.0, 0.5));

    // Q: A and B have a fixed 1.5 on the mids; the C's knob goes from 0.5 (broad) to 3.0 (sharp).
    const auto skirt = [&](int r, float q)
    { return gainAt(1000.0 * std::pow(2.0, 0.5), band(r, {{"hmffreq", 1000.0f}, {"hmfgain", 15.0f}, {"hmfq", q}})); };
    CHECK_THAT(skirt(0, 0.5f), WithinAbs(skirt(0, 3.0f), 0.01)); // (no Q control on A)
    CHECK_THAT(skirt(1, 0.5f), WithinAbs(skirt(1, 3.0f), 0.01));
    CHECK(skirt(2, 0.5f) > skirt(2, 3.0f) + 6.0);
    // Proportional Q: a small boost is broader than a large one (relative to its own peak).
    const auto width = [&](float gain)
    {
        const auto set = band(2, {{"lmffreq", 500.0f}, {"lmfgain", gain}, {"lmfq", 1.5f}});
        return gainAt(1000.0, set) / gainAt(500.0, set);
    };
    CHECK(width(3.0f) > width(15.0f) + 0.05);
    CHECK_THAT(Harrison4032::proportionalQ(2.0, 15.0), WithinAbs(2.0, 1e-12));
    CHECK_THAT(Harrison4032::proportionalQ(2.0, 0.0), WithinAbs(0.8, 1e-12));

    // The display's curve is the processing's.
    Harrison4032 h;
    band(2, {{"hmffreq", 2500.0f}, {"hmfgain", 9.0f}, {"hpf", 100.0f}, {"filtersin", 1.0f}})(h);
    CHECK_THAT(h.responseDb(2500.0), WithinAbs(9.0, 0.3));
    CHECK(h.responseDb(25.0) < -30.0);
}

TEST_CASE("Harrison 4032: in-line paths: FLIP swaps the channel and monitor paths", "[devices][harrison]")
{
    // The track (microphone) in the channel path, the tape return (sidechain) in the monitor path.
    const auto track = tone(1000.0, at(0.5), level(-18.0));
    const auto tape = tone(300.0, at(0.5), level(-18.0));
    const auto levels = [&](Settings settings)
    {
        const auto out = through(track, with(settings), &tape);
        return std::make_pair(toDb(amplitudeAt(out.left, 1000.0, at(0.3), at(0.5))), toDb(amplitudeAt(out.left, 300.0, at(0.3), at(0.5))));
    };
    // The channel path's EQ shapes the channel input, not the tape return; FLIP swaps them.
    const auto [trackPlain, tapePlain] = levels({});
    CHECK_THAT(trackPlain, WithinAbs(-18.0, 0.1));
    CHECK_THAT(tapePlain, WithinAbs(-18.0, 0.1));
    const Settings eq{{"eqin", 1.0f}, {"hmffreq", 1000.0f}, {"hmfgain", -12.0f}, {"lmffreq", 300.0f}, {"lmfgain", 6.0f}};
    Harrison4032 curve;
    with(eq)(curve);
    const auto [trackEq, tapeEq] = levels(eq);
    CHECK_THAT(trackEq, WithinAbs(-18.0 + curve.responseDb(1000.0), 0.1));
    CHECK(curve.responseDb(1000.0) < -10.0);
    CHECK_THAT(tapeEq, WithinAbs(-18.0, 0.1));
    const auto [trackFlip, tapeFlip] =
        levels({{"flip", 1.0f}, {"eqin", 1.0f}, {"hmffreq", 1000.0f}, {"hmfgain", -12.0f}, {"lmffreq", 300.0f}, {"lmfgain", 6.0f}});
    CHECK_THAT(trackFlip, WithinAbs(-18.0, 0.1));
    CHECK_THAT(tapeFlip, WithinAbs(-18.0 + curve.responseDb(300.0), 0.1));
    CHECK(curve.responseDb(300.0) > 4.0);
    // The VCA fader moves the channel path; the monitor level the monitor path.
    const auto [trackFader, tapeMon] = levels({{"fader", -10.0f}, {"monlevel", -6.0f}});
    CHECK_THAT(trackFader, WithinAbs(-28.0, 0.1));
    CHECK_THAT(tapeMon, WithinAbs(-24.0, 0.1));
    // MIX off: the channel path leaves the mix (it still feeds its buses and sends).
    const auto [trackOff, tapeOn] = levels({{"mix", 0.0f}});
    CHECK(trackOff < -150.0);
    CHECK_THAT(tapeOn, WithinAbs(-18.0, 0.1));
}

TEST_CASE("Harrison 4032: pan pot, quad and LCRS panning, the VCA fader and its control voltage", "[devices][harrison]")
{
    // Mono module: constant power, -3 dB in the centre; the stereo pair: a balance.
    const auto mono = [](float pan) { return with({{"format", 1.0f}, {"pan", pan}}); };
    CHECK_THAT(gainAt(1000.0, mono(0.0f), 0), WithinAbs(-3.01, 0.05));
    CHECK_THAT(gainAt(1000.0, mono(0.0f), 1), WithinAbs(-3.01, 0.05));
    CHECK_THAT(gainAt(1000.0, mono(-100.0f), 0), WithinAbs(0.0, 0.05));
    CHECK(gainAt(1000.0, mono(-100.0f), 1) < -100.0);
    CHECK_THAT(gainAt(1000.0, with({{"pan", 0.0f}}), 0), WithinAbs(0.0, 0.05));
    CHECK_THAT(gainAt(1000.0, with({{"pan", 50.0f}}), 0), WithinAbs(-6.02, 0.05));
    CHECK_THAT(gainAt(1000.0, with({{"pan", 50.0f}}), 1), WithinAbs(0.0, 0.05));

    // Quad (front / back): the rear pair on the quad outputs; LCRS: centre and surround there.
    const auto input = tone(1000.0, at(0.5), level(-20.0));
    const auto levelOf = [](const std::vector<double>& x) { return toDb(amplitudeAt(x, 1000.0, at(0.3), at(0.5))); };
    {
        const auto r = runFull(input, with({{"panmode", 1.0f}, {"frontback", 0.0f}}));
        CHECK_THAT(levelOf(r.main.left), WithinAbs(-23.01, 0.05));
        CHECK_THAT(levelOf(r.extra[Harrison4032::Outputs::quad]), WithinAbs(-23.01, 0.05));
        CHECK_THAT(levelOf(r.extra[Harrison4032::Outputs::quad + 1]), WithinAbs(-23.01, 0.05));
    }
    {
        const auto r = runFull(input, with({{"format", 1.0f}, {"panmode", 2.0f}, {"pan", 0.0f}}));
        CHECK(levelOf(r.main.left) < -100.0);
        CHECK_THAT(levelOf(r.extra[Harrison4032::Outputs::quad]), WithinAbs(-20.0, 0.05)); // centre
        const auto s = runFull(input, with({{"format", 1.0f}, {"panmode", 2.0f}, {"pan", -100.0f}, {"frontback", 100.0f}}));
        CHECK_THAT(levelOf(s.extra[Harrison4032::Outputs::quad + 1]), WithinAbs(-20.0, 0.05)); // surround
        CHECK(levelOf(s.main.left) < -100.0);
    }

    // The fader: off is silent; +10 dB at the top.
    CHECK_THAT(gainAt(1000.0, with({{"fader", 10.0f}})), WithinAbs(10.0, 0.05));
    CHECK(gainAt(1000.0, with({{"fader", -80.0f}})) < -150.0);
    // Control-voltage feedthrough: a fast 40 dB fader move thumps on the dbx 202 / EGC-101
    // generation, about 20 dB less on the dbx 2150.
    const auto thump = [](int r)
    {
        auto [node, h] = makeNode<Harrison4032>();
        setParameter(*h, "revision", static_cast<float>(r));
        (void) run(*node, Stereo(at(0.1)));
        setParameter(*h, "fader", -40.0f);
        return toDb(peakOf(run(*node, Stereo(at(0.2))).left));
    };
    const double a = thump(0), b = thump(1), c = thump(2);
    INFO("thump A " << a << ", B " << b << ", C " << c << " dBFS");
    CHECK_THAT(a, WithinAbs(-53.0, 3.0));
    CHECK_THAT(b, WithinAbs(a, 0.5));
    CHECK_THAT(c, WithinAbs(a - 20.0, 1.0));
}

TEST_CASE("Harrison 4032: aux and cue sends, the 32 bus assignments, crosstalk, direct out", "[devices][harrison]")
{
    using Outputs = Harrison4032::Outputs;
    const auto input = tone(1000.0, at(0.5), level(-20.0));
    const auto levelOf = [](const std::vector<double>& x) { return toDb(amplitudeAt(x, 1000.0, at(0.3), at(0.5))); };

    // Aux 1 - 4: pre or post fader.
    {
        const auto r = runFull(input, with({{"aux1", 0.0f}, {"aux2", -6.0f}, {"aux3", 0.0f}, {"aux3pre", 1.0f}, {"fader", -10.0f}}));
        CHECK_THAT(levelOf(r.extra[Outputs::aux(1)]), WithinAbs(-30.0, 0.05)); // post: follows the fader
        CHECK_THAT(levelOf(r.extra[Outputs::aux(2)]), WithinAbs(-36.0, 0.05));
        CHECK_THAT(levelOf(r.extra[Outputs::aux(3)]), WithinAbs(-20.0, 0.05)); // pre: does not
        CHECK(peakOf(r.extra[Outputs::aux(4)]) < 1e-12);
    }
    // Cue 1 - 2: level and pan; from the monitor path (the tape return) or the channel.
    {
        const auto tape = tone(300.0, at(0.5), level(-20.0));
        const auto r = runFull(input, with({{"cue1", 0.0f}, {"cue1pan", -100.0f}, {"cue2", -6.0f}, {"cuefrom", 0.0f}}), &tape);
        CHECK_THAT(toDb(amplitudeAt(r.extra[Outputs::cue(1)], 300.0, at(0.3), at(0.5))), WithinAbs(-20.0, 0.1));
        CHECK(amplitudeAt(r.extra[Outputs::cue(1) + 1], 300.0, at(0.3), at(0.5)) < level(-100.0));
        CHECK_THAT(toDb(amplitudeAt(r.extra[Outputs::cue(2)], 300.0, at(0.3), at(0.5))), WithinAbs(-6.0 - 20.0 - 3.01, 0.1));
        CHECK(amplitudeAt(r.extra[Outputs::cue(1)], 1000.0, at(0.3), at(0.5)) < level(-100.0)); // (not the channel)
        const auto c = runFull(input, with({{"cue1", 0.0f}, {"cuefrom", 1.0f}}), &tape);
        CHECK_THAT(levelOf(c.extra[Outputs::cue(1)]), WithinAbs(-23.01, 0.1));
    }
    // The routing switches: post fader, each assigned bus gets the channel; with Bus Pan the pan
    // pot divides it between odd and even buses.
    {
        const auto r = runFull(input, with({{"bus5", 1.0f}, {"bus6", 1.0f}, {"bus17", 1.0f}, {"fader", -6.0f}, {"revision", 2.0f}}));
        CHECK_THAT(levelOf(r.extra[Outputs::bus(5)]), WithinAbs(-26.0, 0.05));
        CHECK_THAT(levelOf(r.extra[Outputs::bus(6)]), WithinAbs(-26.0, 0.05));
        CHECK_THAT(levelOf(r.extra[Outputs::bus(17)]), WithinAbs(-26.0, 0.05));
        // Crosstalk into the neighbours: -90 dB on the C.
        CHECK_THAT(levelOf(r.extra[Outputs::bus(18)]) - levelOf(r.extra[Outputs::bus(17)]), WithinAbs(-90.0, 0.5));
        CHECK(peakOf(r.extra[Outputs::bus(1)]) < 1e-12);
        const auto p = runFull(input, with({{"bus1", 1.0f}, {"bus2", 1.0f}, {"buspan", 1.0f}, {"pan", -50.0f}}));
        CHECK(levelOf(p.extra[Outputs::bus(1)]) > levelOf(p.extra[Outputs::bus(2)]) + 6.0);
    }
    // Crosstalk by revision: about -62 dB on A (inverting summing), -85 dB on B.
    const auto crosstalk = [&](int r)
    {
        const auto x = runFull(input, revision(r, {{"bus9", 1.0f}}));
        return levelOf(x.extra[Outputs::bus(10)]) - levelOf(x.extra[Outputs::bus(9)]);
    };
    CHECK_THAT(crosstalk(0), WithinAbs(-62.0, 0.5));
    CHECK_THAT(crosstalk(1), WithinAbs(-85.0, 0.5));
    // Direct out: the channel after its fader.
    {
        const auto r = runFull(input, with({{"direct", 1.0f}, {"fader", -3.0f}}));
        CHECK_THAT(levelOf(r.extra[Outputs::direct]), WithinAbs(-23.0, 0.05));
        CHECK_THAT(levelOf(r.extra[Outputs::direct + 1]), WithinAbs(-23.0, 0.05));
    }
}

TEST_CASE("Harrison 4032: bus master and master section with its monitor matrix", "[devices][harrison]")
{
    // Bus master: the summing amplifier, fader and line driver; the optional Jensen output
    // transformer saturates the deep lows at high levels.
    CHECK_THAT(gainAt(1000.0, with({{"module", 1.0f}})), WithinAbs(0.0, 0.05));
    CHECK_THAT(gainAt(1000.0, with({{"module", 1.0f}, {"fader", -7.0f}, {"linetrim", 2.0f}})), WithinAbs(-5.0, 0.05));
    {
        const auto thd = [](bool transformer)
        {
            const auto out = through(tone(20.0, at(1.5), level(1.0)), with({{"module", 1.0f}, {"transformer", transformer ? 1.0f : 0.0f}}));
            return thdOf(out.left, 20.0, at(0.5), at(1.5));
        };
        CHECK(thd(true) > 3.0 * thd(false));
    }
    // Master section: the monitor matrix: the mix, an external source (sidechain), both; mono.
    const auto program = tone(1000.0, at(0.5), level(-20.0), 1000.0, level(-26.0));
    const auto external = tone(300.0, at(0.5), level(-24.0));
    const auto listen = [&](Settings settings, bool offline = false)
    {
        std::vector<std::pair<const char*, float>> list(settings);
        list.emplace_back("module", 2.0f);
        return runFull(program,
                       [list](Harrison4032& h)
                       {
                           for (const auto& [id, value] : list)
                               setParameter(h, id, value);
                       },
                       &external, offline)
            .main;
    };
    const auto mix = listen({});
    CHECK_THAT(toDb(amplitudeAt(mix.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-20.0, 0.05));
    CHECK(amplitudeAt(mix.left, 300.0, at(0.3), at(0.5)) < level(-100.0));
    const auto ext = listen({{"crm", 1.0f}});
    CHECK_THAT(toDb(amplitudeAt(ext.left, 300.0, at(0.3), at(0.5))), WithinAbs(-24.0, 0.05));
    CHECK(amplitudeAt(ext.left, 1000.0, at(0.3), at(0.5)) < level(-100.0));
    const auto both = listen({{"crm", 2.0f}, {"crmlevel", -6.0f}});
    CHECK_THAT(toDb(amplitudeAt(both.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-26.0, 0.05));
    CHECK_THAT(toDb(amplitudeAt(both.left, 300.0, at(0.3), at(0.5))), WithinAbs(-30.0, 0.05));
    const auto mono = listen({{"mono", 1.0f}});
    CHECK_THAT(amplitudeAt(mono.left, 1000.0, at(0.3), at(0.5)), WithinAbs(amplitudeAt(mono.right, 1000.0, at(0.3), at(0.5)), 1e-6));
    // The master fader is the mix; the matrix is monitoring: an offline export is the mix.
    const auto exported = listen({{"crm", 1.0f}, {"mono", 1.0f}, {"fader", -3.0f}}, true);
    CHECK_THAT(toDb(amplitudeAt(exported.left, 1000.0, at(0.3), at(0.5))), WithinAbs(-23.0, 0.05));
    CHECK_THAT(toDb(amplitudeAt(exported.right, 1000.0, at(0.3), at(0.5))), WithinAbs(-29.0, 0.05));
}

TEST_CASE("Harrison 4032: revisions, saved settings and typed values", "[devices][harrison]")
{
    const auto& a = Harrison4032::revisionSpec(0);
    const auto& b = Harrison4032::revisionSpec(1);
    const auto& c = Harrison4032::revisionSpec(2);
    CHECK_FALSE(a.shelves);
    CHECK(b.shelves);
    CHECK(c.shelves);
    CHECK_FALSE(b.variableQ);
    CHECK(c.variableQ);
    CHECK(a.hpfPoles == 2);
    CHECK(c.hpfPoles == 3);
    CHECK_FALSE(b.filtersSwitch);
    CHECK(c.filtersSwitch);
    CHECK(a.maxOutputDbu == 22.0);
    CHECK(b.maxOutputDbu == 24.5);
    CHECK(c.maxOutputDbu == 26.0);
    CHECK(a.railVolts == 15.0);
    CHECK(c.railVolts == 18.0);

    Harrison4032 h;
    setParameter(h, "revision", 0.0f);
    setParameter(h, "hmfgain", 4.5f);
    setParameter(h, "bus12", 1.0f);
    setParameter(h, "flip", 1.0f);
    juce::MemoryBlock state;
    h.getStateInformation(state);
    Harrison4032 other;
    other.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(other.getRevision() == Harrison4032::revisionA);
    CHECK_THAT(other.hmfGain_->get(), WithinAbs(4.5, 1e-4));
    CHECK(other.bus_[11]->get());
    CHECK(other.flip_->get());
    auto* pan = other.getParameterById("pan");
    CHECK(pan->getText(pan->convertTo0to1(-30.0f), 10) == "L30");
    auto* fb = other.getParameterById("frontback");
    CHECK(fb->getText(fb->convertTo0to1(0.0f), 10) == "Middle");
}


TEST_CASE("Harrison 4032: the editor shows each revision's controls; the routing switches feed DAW busses", "[ui][harrison]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Vocal");
    auto device = session.addDevice(track, Harrison4032::kTypeId, "Harrison 4032");
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto& console = dynamic_cast<Harrison4032&>(node->getProcessor());
    const ui::DeviceEditorContext context{project, editorState, device, track};

    // Every module's, revision's and pan mode's pages name real controls; each revision has its own.
    for (int module = 0; module < 3; ++module)
        for (int r = 0; r < 3; ++r)
            for (int panMode = 0; panMode < 3; ++panMode)
            {
                ui::PagedEditor pages(console, ui::HarrisonEditor::pagesFor(console, context, module, r, false, panMode));
                CHECK(pages.getMissingParameters().isEmpty());
            }
    const auto has = [&](int module, int r, const juce::String& id, int panMode = 0)
    {
        for (const auto& page : ui::HarrisonEditor::pagesFor(console, context, module, r, false, panMode))
            for (const auto& [parameter, caption] : page.parameters)
                if (parameter == id)
                    return true;
        return false;
    };
    CHECK_FALSE(has(0, 0, "lfshelf"));
    CHECK_FALSE(has(0, 0, "hmfq"));
    CHECK_FALSE(has(0, 0, "filtersin"));
    CHECK(has(0, 1, "hfshelf"));
    CHECK_FALSE(has(0, 1, "lmfq"));
    CHECK_FALSE(has(0, 1, "filtersin"));
    CHECK(has(0, 2, "lmfq"));
    CHECK(has(0, 2, "filtersin"));
    CHECK(has(0, 2, "lfshelf"));
    CHECK_FALSE(has(0, 2, "frontback"));
    CHECK(has(0, 2, "frontback", 1));
    CHECK(has(1, 2, "transformer"));
    CHECK_FALSE(has(1, 2, "eqin"));
    CHECK(has(2, 2, "crm"));
    CHECK(has(2, 2, "mono"));

    // The editor follows the revision switch, and paints every page of every module.
    ui::HarrisonEditor editor(console, context);
    editor.setSize(ui::HarrisonEditor::preferredWidth(console, context), 230);
    CHECK(editor.getPages().getPageTitle(0) == "Input");
    for (int module = 0; module < 3; ++module)
        for (int r = 0; r < 3; ++r)
        {
            setParameter(console, "module", static_cast<float>(module));
            setParameter(console, "revision", static_cast<float>(r));
            juce::MessageManager::getInstance()->runDispatchLoopUntil(300);
            for (std::size_t p = 0; p < editor.getPages().getNumPages(); ++p)
            {
                editor.getPages().showPage(p);
                (void) editor.createComponentSnapshot(editor.getLocalBounds());
            }
        }
    CHECK(editor.getPages().getPageTitle(0) == "Master");
    setParameter(console, "module", 0.0f);

    // The routing switches: assigning bus 5 makes the DAW bus "Bus 5-6" and a device send of the
    // pair to it; the pair's last unassignment removes the send.
    const auto pairSend = [&] { return session::Session::findDeviceSend(track, session::device::getId(device), Harrison4032::Outputs::bus(5),
                                                                        Harrison4032::Outputs::bus(6)); };
    ui::harrison::assignBus(session, device, console, 5, true);
    CHECK(console.bus_[4]->get());
    REQUIRE(pairSend().isValid());
    const auto bus = session.findTrack(session::send::getTarget(pairSend()));
    CHECK(session::track::getName(bus) == "Bus 5-6");
    CHECK(session::track::getKind(bus) == session::TrackKind::bus);
    ui::harrison::assignBus(session, device, console, 6, true);
    ui::harrison::assignBus(session, device, console, 5, false);
    CHECK(pairSend().isValid()); // (bus 6 still assigned)
    ui::harrison::assignBus(session, device, console, 6, false);
    CHECK_FALSE(pairSend().isValid());
    // Assigning again reuses the bus.
    ui::harrison::assignBus(session, device, console, 6, true);
    CHECK(session.findTrack(session::send::getTarget(pairSend())) == bus);

    // Aux, cue, quad and direct connect to their own busses (one undo step each).
    const auto connections = ui::harrison::connections(device, track);
    REQUIRE(connections.size() == 8);
    CHECK(connections.back().busName == "Direct Vocal");
    const auto& aux1 = connections.front();
    CHECK(aux1.busName == "Aux 1");
    session.getUndoManager().beginNewTransaction();
    const auto send = ui::deviceActions::connectDeviceOutput(session, device, aux1.left, aux1.right, aux1.busName);
    REQUIRE(send.isValid());
    CHECK(session::track::getName(ui::deviceActions::connectedBus(session, device, aux1.left, aux1.right)) == "Aux 1");
    CHECK(ui::deviceActions::connectDeviceOutput(session, device, aux1.left, aux1.right, aux1.busName) == send); // (already)
    session.getUndoManager().undo();
    CHECK_FALSE(ui::deviceActions::connectedBus(session, device, aux1.left, aux1.right).isValid());
}

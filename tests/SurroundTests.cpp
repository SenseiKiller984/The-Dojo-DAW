#include "devices/DeviceRegistry.h"
#include "core/rt/RealtimeGuard.h"
#include "engine/AudioEngine.h"
#include "engine/dsp/SurroundVirtualizer.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"
#include "TestAudio.h"
#include "TestSupport.h"
#include "ui/Preferences.h"

#include <juce_dsp/juce_dsp.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <random>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using engine::Speaker;
using engine::dsp::SurroundVirtualizer;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;
constexpr double kPi = 3.14159265358979323846;
const double kHeadTime = SurroundVirtualizer::kHeadRadius / SurroundVirtualizer::kSpeedOfSound;

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    auto processor = devices::createBuiltinDevice(device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

/** The virtualizer's two outputs for `input` on one speaker of `layout` (others silent). */
struct Ears
{
    std::vector<double> left, right;
};
Ears render(SurroundVirtualizer& v, const engine::ChannelLayout& layout, int channel, const std::vector<double>& input)
{
    std::vector<std::vector<double>> channels(static_cast<std::size_t>(layout.numChannels), std::vector<double>(input.size(), 0.0));
    channels[static_cast<std::size_t>(channel)] = input;
    std::vector<const double*> pointers;
    for (auto& c : channels)
        pointers.push_back(c.data());
    Ears ears{std::vector<double>(input.size()), std::vector<double>(input.size())};
    v.process(pointers.data(), layout.numChannels, ears.left.data(), ears.right.data(), static_cast<int>(input.size()));
    return ears;
}

double energy(const std::vector<double>& x, std::size_t from = 0)
{
    double e = 0.0;
    for (std::size_t i = from; i < x.size(); ++i)
        e += x[i] * x[i];
    return e;
}

/** The lag (in samples, fractional by parabolic interpolation) at which `b` best matches `a`. */
double lagOf(const std::vector<double>& a, const std::vector<double>& b, int maxLag)
{
    std::vector<double> c(static_cast<std::size_t>(2 * maxLag + 1));
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (std::size_t i = static_cast<std::size_t>(maxLag); i + static_cast<std::size_t>(maxLag) < a.size(); ++i)
            sum += a[i] * b[static_cast<std::size_t>(static_cast<long long>(i) + lag)];
        c[static_cast<std::size_t>(lag + maxLag)] = sum;
    }
    const auto best = static_cast<int>(std::max_element(c.begin(), c.end()) - c.begin());
    if (best == 0 || best == 2 * maxLag)
        return best - maxLag;
    const double y0 = c[static_cast<std::size_t>(best - 1)], y1 = c[static_cast<std::size_t>(best)], y2 = c[static_cast<std::size_t>(best + 1)];
    return best - maxLag + 0.5 * (y0 - y2) / (y0 - 2.0 * y1 + y2);
}

std::vector<double> noise(std::size_t n, unsigned seed, double amplitude = 0.3)
{
    std::mt19937 random(seed);
    std::normal_distribution<double> d(0.0, amplitude);
    std::vector<double> x(n);
    for (auto& v : x)
        v = d(random);
    return x;
}

/** A first-order energy split: the share of `x`'s energy above `frequency` (one-pole high-pass). */
double highShare(const std::vector<double>& x, double frequency)
{
    const double a = std::exp(-2.0 * kPi * frequency / kRate);
    double low = 0.0, high = 0.0, lp = 0.0;
    for (const double v : x)
    {
        lp = (1.0 - a) * v + a * lp;
        low += lp * lp;
        high += (v - lp) * (v - lp);
    }
    return high / std::max(1.0e-12, low + high);
}

/** A session on a real engine with `outputs` device outputs. */
struct Rig
{
    explicit Rig(int outputs)
        : engine(1)
        , devices(session, createDevice)
        , numOutputs(outputs)
    {
        engine.prepare(kRate, kBlock, outputs, 0);
        devices.setPlayHead(engine.getPlayHead());
        devices.prepareAll(kRate, kBlock);
        builder.setRenderFormat(kRate, kBlock);
        out.resize(static_cast<std::size_t>(outputs));
    }

    void publish()
    {
        auto graph = builder.build(session, assets, &devices);
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
    }

    void monitor(engine::command::SurroundMonitoring mode, bool alsa = false)
    {
        engine::command::SetSurroundMonitoring c;
        c.mode = mode;
        c.alsaOrder = alsa;
        c.room = 0.0f;
        REQUIRE(engine.post(c));
    }

    void render(double seconds)
    {
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        for (auto& channel : out)
            channel.clear();
        std::vector<std::vector<float>> buffers(static_cast<std::size_t>(numOutputs), std::vector<float>(kBlock));
        std::vector<float*> pointers;
        for (auto& b : buffers)
            pointers.push_back(b.data());
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, pointers.data(), numOutputs, kBlock);
            for (std::size_t ch = 0; ch < buffers.size(); ++ch)
                out[ch].insert(out[ch].end(), buffers[ch].begin(), buffers[ch].end());
            engine.releaseRetiredGraphs();
        }
    }

    double level(int channel) const { return out[static_cast<std::size_t>(channel)][out[0].size() / 2]; }
    double rms(int channel) const
    {
        const auto& x = out[static_cast<std::size_t>(channel)];
        double e = 0.0;
        for (std::size_t i = x.size() / 4; i < x.size(); ++i)
            e += static_cast<double>(x[i]) * x[i];
        return std::sqrt(e / static_cast<double>(x.size() - x.size() / 4));
    }

    engine::AudioEngine engine;
    Session session;
    AudioAssetCache assets;
    DeviceManager devices;
    GraphBuilder builder;
    int numOutputs;
    std::vector<std::vector<float>> out;
};
} // namespace

// --- The head model ------------------------------------------------------------------------------------

TEST_CASE("Virtualizer head model: Woodworth time differences and the Brown-Duda head shadow", "[surround][virtualizer]")
{
    const auto itd = [](double azimuth)
    {
        return SurroundVirtualizer::earDelay(SurroundVirtualizer::incidence(azimuth, false))
               - SurroundVirtualizer::earDelay(SurroundVirtualizer::incidence(azimuth, true));
    };
    // Woodworth: (a/c)(theta + sin theta).
    for (const double azimuth : {0.0, 15.0, 30.0, 60.0, 90.0})
    {
        const double theta = azimuth * kPi / 180.0;
        CHECK_THAT(itd(azimuth), WithinAbs(kHeadTime * (theta + std::sin(theta)), 1e-9));
    }
    CHECK_THAT(itd(90.0), WithinAbs(0.000656, 0.000002)); // about 0.66 ms at the side
    CHECK_THAT(itd(-30.0), WithinAbs(-itd(30.0), 1e-12));  // symmetric
    CHECK_THAT(itd(150.0), WithinAbs(itd(30.0), 1e-12));   // (the cone of confusion: front and back alike)
    CHECK(SurroundVirtualizer::earDelay(0.0) == 0.0);

    CHECK_THAT(SurroundVirtualizer::shadowAlpha(0.0), WithinAbs(2.0, 1e-12));
    CHECK_THAT(SurroundVirtualizer::shadowAlpha(150.0 * kPi / 180.0), WithinAbs(0.1, 1e-12));
    // +6 dB of treble facing the ear, about -20 dB in its shadow, nothing in the bass.
    CHECK_THAT(juce::Decibels::gainToDecibels(SurroundVirtualizer::shadowGain(0.0, 20000.0)), WithinAbs(6.0, 0.3));
    CHECK(juce::Decibels::gainToDecibels(SurroundVirtualizer::shadowGain(150.0 * kPi / 180.0, 20000.0)) < -18.0);
    CHECK_THAT(SurroundVirtualizer::shadowGain(kPi / 2.0, 50.0), WithinAbs(1.0, 0.01));

    // Speaker places (ITU-R BS.775 / Dolby).
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::left, false) == -30.0);
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::rightSurround, false) == 110.0);
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::rightSurround, true) == 90.0);
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::leftRear, true) == -150.0);
}

TEST_CASE("Virtualizer: each speaker reaches the ears with its time and level differences", "[surround][virtualizer]")
{
    SurroundVirtualizer v;
    v.prepare(kRate);
    const auto layout = engine::ChannelLayout::surround71();
    v.configure(layout, SurroundVirtualizer::Output::headphones, 0.0); // (anonymous: direct sound only)
    const auto input = noise(24000, 1);

    // Right side surround (+90 deg): the right ear first by 0.66 ms, and louder, brighter.
    auto ears = render(v, layout, layout.indexOf(Speaker::rightSurround), input);
    CHECK_THAT(lagOf(ears.right, ears.left, 64) / kRate, WithinAbs(kHeadTime * (kPi / 2.0 + 1.0), 0.00003));
    CHECK(energy(ears.right) > 2.0 * energy(ears.left));
    CHECK(highShare(ears.right, 3000.0) > highShare(ears.left, 3000.0));

    // Left front (-30 deg): the left ear first by 0.26 ms.
    v.reset();
    ears = render(v, layout, layout.indexOf(Speaker::left), input);
    CHECK_THAT(lagOf(ears.left, ears.right, 64) / kRate, WithinAbs(kHeadTime * (kPi / 6.0 + 0.5), 0.00003));
    CHECK(energy(ears.left) > energy(ears.right));

    // Centre: both ears alike.
    v.reset();
    ears = render(v, layout, layout.indexOf(Speaker::centre), input);
    CHECK_THAT(lagOf(ears.left, ears.right, 64), WithinAbs(0.0, 0.05));
    CHECK_THAT(energy(ears.left) / energy(ears.right), WithinRel(1.0, 0.001));

    // Behind (the rear speakers) is duller than in front: the pinna cue.
    v.reset();
    const auto front = render(v, layout, layout.indexOf(Speaker::left), input);
    v.reset();
    const auto back = render(v, layout, layout.indexOf(Speaker::leftRear), input);
    CHECK(highShare(back.left, 5000.0) < 0.85 * highShare(front.left, 5000.0));

    // The LFE: only the bass, equally on both sides.
    v.reset();
    ears = render(v, layout, layout.indexOf(Speaker::lfe), input);
    CHECK_THAT(energy(ears.left) / energy(ears.right), WithinRel(1.0, 1e-9));
    CHECK(highShare(ears.left, 500.0) < 0.02);
}

TEST_CASE("Virtualizer: a full mix is as loud as the fold-down; the room adds reflections", "[surround][virtualizer]")
{
    for (const auto& layout : {engine::ChannelLayout::surround51(), engine::ChannelLayout::surround71(), engine::ChannelLayout::dolbyDigital()})
    {
        SurroundVirtualizer v;
        v.prepare(kRate);
        v.configure(layout, SurroundVirtualizer::Output::headphones, 0.0);

        // Uncorrelated noise of equal level on every speaker but the LFE.
        const std::size_t n = 48000;
        std::vector<std::vector<double>> channels;
        std::vector<const double*> pointers;
        for (int c = 0; c < layout.numChannels; ++c)
            channels.push_back(layout.speakers[static_cast<std::size_t>(c)] == Speaker::lfe ? std::vector<double>(n, 0.0)
                                                                                              : noise(n, static_cast<unsigned>(10 + c)));
        for (auto& c : channels)
            pointers.push_back(c.data());
        std::vector<double> left(n), right(n);
        v.process(pointers.data(), layout.numChannels, left.data(), right.data(), static_cast<int>(n));

        const auto fold = engine::conversionMatrix(layout, engine::ChannelLayout::stereo());
        std::vector<double> folded(n, 0.0);
        for (std::size_t i = 0; i < n; ++i)
            for (int c = 0; c < layout.numChannels; ++c)
                folded[i] += fold.gain[0][static_cast<std::size_t>(c)] * channels[static_cast<std::size_t>(c)][i];
        const double ratioDb = 10.0 * std::log10(energy(left, 4800) / energy(folded, 4800));
        INFO("layout of " << layout.numChannels << " channels: " << ratioDb << " dB");
        CHECK(std::abs(ratioDb) < 1.5);

        // With a room: more energy arrives later (reflections, reverberation).
        SurroundVirtualizer roomy;
        roomy.prepare(kRate);
        roomy.configure(layout, SurroundVirtualizer::Output::headphones, 1.0);
        std::vector<double> impulse(9600, 0.0);
        impulse[0] = 1.0;
        const auto dry = render(v, layout, 0, impulse);
        const auto wet = render(roomy, layout, 0, impulse);
        const std::size_t late = static_cast<std::size_t>(0.003 * kRate); // after the direct sound
        CHECK(energy(wet.left, late) > 10.0 * energy(dry.left, late) + 1e-9);
    }
}

TEST_CASE("Virtualizer for speakers: crosstalk cancellation keeps each signal to its ear", "[surround][virtualizer]")
{
    // A left-only source, played through the speakers: the far (right) ear also hears the left
    // speaker, delayed by the head's path difference and shadowed. Simulated with the same model
    // the canceller assumes; with cancellation the right ear gets much less of it.
    const auto layout = engine::ChannelLayout::stereo();
    const auto input = noise(48000, 5);
    const auto ears = [&](SurroundVirtualizer::Output output)
    {
        SurroundVirtualizer v;
        v.prepare(kRate);
        v.configure(layout, output, 0.0);
        auto out = render(v, layout, 0, input);
        // At the ears: each speaker directly plus the other one across the head (0.26 ms, -1.1 dB, 250 Hz - 6 kHz).
        const double delay = kHeadTime * (kPi / 6.0 + 0.5) * kRate;
        juce::dsp::IIR::Filter<double> highL, lowL, highR, lowR;
        highL.coefficients = highR.coefficients = juce::dsp::IIR::Coefficients<double>::makeHighPass(kRate, 250.0);
        lowL.coefficients = lowR.coefficients = juce::dsp::IIR::Coefficients<double>::makeLowPass(kRate, 6000.0);
        std::vector<double> leftEar(out.left.size()), rightEar(out.right.size());
        for (std::size_t i = 0; i < out.left.size(); ++i)
        {
            const double back = static_cast<double>(i) - delay;
            const auto at = [&](const std::vector<double>& x)
            {
                if (back < 1.0)
                    return 0.0;
                const auto k = static_cast<std::size_t>(back);
                const double f = back - static_cast<double>(k);
                return x[k] + (x[k + 1 < x.size() ? k + 1 : k] - x[k]) * f;
            };
            leftEar[i] = out.left[i] + 0.88 * lowR.processSample(highR.processSample(at(out.right)));
            rightEar[i] = out.right[i] + 0.88 * lowL.processSample(highL.processSample(at(out.left)));
        }
        return std::pair{energy(leftEar, 4800), energy(rightEar, 4800)};
    };
    const auto [phonesLeft, phonesRight] = ears(SurroundVirtualizer::Output::headphones);
    const auto [speakersLeft, speakersRight] = ears(SurroundVirtualizer::Output::speakers);
    const double separationWithout = 10.0 * std::log10(phonesLeft / phonesRight);
    const double separationWith = 10.0 * std::log10(speakersLeft / speakersRight);
    INFO("separation " << separationWithout << " dB without, " << separationWith << " dB with cancellation");
    CHECK(separationWith > separationWithout + 3.0);

    // Stable: long noise does not grow.
    SurroundVirtualizer v;
    v.prepare(kRate);
    v.configure(engine::ChannelLayout::surround51(), SurroundVirtualizer::Output::speakers, 1.0);
    const auto long51 = render(v, engine::ChannelLayout::surround51(), 0, noise(480000, 9));
    double peak = 0.0;
    for (std::size_t i = long51.left.size() - 48000; i < long51.left.size(); ++i)
        peak = std::max(peak, std::abs(long51.left[i]));
    CHECK(peak < 3.0);
}

TEST_CASE("Device channel orders for built-in virtualizers", "[surround]")
{
    const auto wave8 = engine::dsp::deviceLayout(8, false);
    CHECK(wave8.numChannels == 8);
    CHECK(wave8.indexOf(Speaker::centre) == 2);
    CHECK(wave8.indexOf(Speaker::lfe) == 3);
    CHECK(wave8.indexOf(Speaker::leftRear) == 4);
    CHECK(wave8.indexOf(Speaker::leftSurround) == 6);
    const auto alsa8 = engine::dsp::deviceLayout(8, true);
    CHECK(alsa8.indexOf(Speaker::leftRear) == 2);
    CHECK(alsa8.indexOf(Speaker::centre) == 4);
    CHECK(engine::dsp::deviceLayout(6, false).indexOf(Speaker::leftSurround) == 4);
    CHECK(engine::dsp::deviceLayout(6, true).indexOf(Speaker::centre) == 4);
    CHECK(engine::dsp::deviceLayout(4, false).numChannels == 4);
    CHECK(engine::dsp::deviceLayout(2, false) == engine::ChannelLayout::stereo());
}

// --- The engine's monitoring modes ---------------------------------------------------------------------

TEST_CASE("Surround monitoring modes: standard, fold-down, virtualizer and the device's own", "[surround][engine]")
{
    test::TempFolder temp;
    // A 5.1 file: channel k holds 0.1 * (k + 1) (L .1, R .2, C .3, LFE .4, Ls .5, Rs .6).
    std::vector<std::vector<float>> channels;
    for (int k = 0; k < 6; ++k)
        channels.emplace_back(static_cast<std::size_t>(kRate), 0.1f * static_cast<float>(k + 1));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("six.wav"), channels, kRate);
    const double minus3 = std::sqrt(0.5);

    const auto setUp = [&](Rig& rig)
    {
        rig.session.setMasterFormat(TrackFormat::surround51);
        juce::String error;
        auto asset = rig.assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = rig.session.addTrack(TrackKind::audio, "Six");
        track::setFormat(t, TrackFormat::surround51, nullptr);
        rig.session.addClip(t, file, 0.0, asset->getLengthSeconds());
        rig.publish();
    };
    using Mode = engine::command::SurroundMonitoring;
    using Rendering = engine::SurroundRendering;

    SECTION("stereo device")
    {
        Rig rig(2);
        setUp(rig);

        rig.monitor(Mode::foldDown); // (the default)
        rig.render(0.5);
        CHECK(rig.engine.collectTelemetry().surroundRendering == Rendering::foldDown);
        CHECK_THAT(rig.level(0), WithinAbs(0.1 + minus3 * 0.3 + minus3 * 0.5, 1e-5));

        rig.monitor(Mode::standard); // the front pair, as it is
        rig.render(0.5);
        CHECK(rig.engine.collectTelemetry().surroundRendering == Rendering::frontPair);
        CHECK_THAT(rig.level(0), WithinAbs(0.1, 1e-5));
        CHECK_THAT(rig.level(1), WithinAbs(0.2, 1e-5));

        rig.monitor(Mode::virtualizer);
        rig.render(0.5);
        CHECK(rig.engine.collectTelemetry().surroundRendering == Rendering::virtualizer);
        CHECK(rig.rms(0) > 0.05); // (constant channels have no direction: the head model is tested above)
        CHECK(rig.rms(1) > 0.05);

        rig.monitor(Mode::device); // a stereo device has no surround channels: the virtualizer
        rig.render(0.5);
        CHECK(rig.engine.collectTelemetry().surroundRendering == Rendering::virtualizer);
    }

    SECTION("surround devices")
    {
        Rig rig(8);
        setUp(rig);

        rig.monitor(Mode::device); // WAVE order: L R C LFE BL BR SL SR (5.1 surrounds to the sides)
        rig.render(0.5);
        CHECK(rig.engine.collectTelemetry().surroundRendering == Rendering::device);
        CHECK_THAT(rig.level(2), WithinAbs(0.3, 1e-5)); // C
        CHECK_THAT(rig.level(3), WithinAbs(0.4, 1e-5)); // LFE
        CHECK_THAT(rig.level(6), WithinAbs(0.5, 1e-5)); // Ls -> SL
        CHECK_THAT(rig.level(7), WithinAbs(0.6, 1e-5)); // Rs -> SR

        rig.monitor(Mode::device, true); // ALSA order: L R RL RR C LFE SL SR
        rig.render(0.5);
        CHECK_THAT(rig.level(4), WithinAbs(0.3, 1e-5)); // C
        CHECK_THAT(rig.level(5), WithinAbs(0.4, 1e-5)); // LFE

        rig.monitor(Mode::standard); // channel n to output n
        rig.render(0.5);
        CHECK(rig.engine.collectTelemetry().surroundRendering == Rendering::discrete);
        for (int ch = 0; ch < 6; ++ch)
            CHECK_THAT(rig.level(ch), WithinAbs(0.1 * (ch + 1), 1e-5));
        CHECK(rig.level(6) == 0.0);

        rig.monitor(Mode::virtualizer); // on outputs 1 and 2 only, also with more outputs
        rig.render(0.5);
        CHECK(rig.engine.collectTelemetry().surroundRendering == Rendering::virtualizer);
        CHECK(rig.rms(0) > 0.05);
        CHECK(rig.level(2) == 0.0);
    }
}

TEST_CASE("Surround preferences are remembered; classic fold-down by default", "[surround][preferences]")
{
    test::TempFolder temp;
    juce::PropertiesFile::Options options;
    options.applicationName = "dojo-tests";
    juce::PropertiesFile file(temp.dir.getChildFile("settings.xml"), options);
    ui::Preferences preferences(file);
    CHECK(preferences.getSurroundMode() == ui::Preferences::SurroundMode::foldDown);
    CHECK_FALSE(preferences.getVirtualizerSpeakers());
    CHECK_THAT(preferences.getVirtualizerRoom(), WithinAbs(0.5, 1e-9));

    preferences.setSurroundMode(ui::Preferences::SurroundMode::virtualizer);
    preferences.setVirtualizerSpeakers(true);
    preferences.setVirtualizerRoom(1.7);
    ui::Preferences again(file);
    CHECK(again.getSurroundMode() == ui::Preferences::SurroundMode::virtualizer);
    CHECK(again.getVirtualizerSpeakers());
    CHECK_THAT(again.getVirtualizerRoom(), WithinAbs(1.0, 1e-9));
}

TEST_CASE("The virtualizer and the device mode never touch the heap on the audio thread", "[surround][engine][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();
    test::TempFolder temp;
    std::vector<std::vector<float>> channels;
    for (int k = 0; k < 8; ++k)
        channels.push_back(test::sine(200.0 + 50.0 * k, kRate, static_cast<int>(2.0 * kRate), 0.2f));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("eight.wav"), channels, kRate);

    for (const int outputs : {2, 8})
    {
        Rig rig(outputs);
        rig.session.setMasterFormat(TrackFormat::surround71);
        juce::String error;
        auto asset = rig.assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = rig.session.addTrack(TrackKind::audio, "Eight");
        track::setFormat(t, TrackFormat::surround71, nullptr);
        rig.session.addClip(t, file, 0.0, asset->getLengthSeconds());
        rig.publish();
        REQUIRE(rig.engine.post(engine::command::SetPlaying{true}));
        std::vector<std::vector<float>> buffers(static_cast<std::size_t>(outputs), std::vector<float>(kBlock));
        std::vector<float*> pointers;
        for (auto& b : buffers)
            pointers.push_back(b.data());

        const auto before = rt::getRealtimeViolationCount();
        using Mode = engine::command::SurroundMonitoring;
        for (int block = 0; block < 200; ++block)
        {
            if (block % 50 == 0)
            {
                engine::command::SetSurroundMonitoring c;
                c.mode = std::array<Mode, 4>{Mode::virtualizer, Mode::device, Mode::standard, Mode::virtualizer}[static_cast<std::size_t>(block / 50)];
                c.speakers = block >= 150;
                c.room = 0.8f;
                REQUIRE(rig.engine.post(c));
            }
            rig.engine.process(nullptr, 0, pointers.data(), outputs, kBlock);
            rig.engine.releaseRetiredGraphs();
        }
        CHECK(rt::getRealtimeViolationCount() == before);
        CHECK(test::peak(buffers[0]) > 0.0f);
    }
}

#include "core/rt/RealtimeGuard.h"
#include "devices/DeviceRegistry.h"
#include "devices/Dynamics.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/ParameterControl.h"
#include "DeviceTestSupport.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kLatency = 240; // 5 ms lookahead at 48 kHz

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
double gainToDb(double gain) { return 20.0 * std::log10(gain); }

double sineAt(double frequency, std::size_t i, double phase = 0.0)
{
    return std::sin(2.0 * kPi * frequency * static_cast<double>(i) / kRate + phase);
}

/** A tone whose amplitude follows `amplitude(i)` (both channels unless `right` is given). */
template <typename Left, typename Right>
Stereo shaped(std::size_t n, double frequency, Left left, Right right)
{
    Stereo s(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        s.left[i] = left(i) * sineAt(frequency, i);
        s.right[i] = right(i) * sineAt(frequency, i);
    }
    return s;
}

template <typename Amplitude>
Stereo shaped(std::size_t n, double frequency, Amplitude amplitude)
{
    return shaped(n, frequency, amplitude, amplitude);
}

double peakOf(const std::vector<double>& x, std::size_t begin = 0, std::size_t end = static_cast<std::size_t>(-1))
{
    double result = 0.0;
    for (std::size_t i = begin; i < std::min(end, x.size()); ++i)
        result = std::max(result, std::abs(x[i]));
    return result;
}

double rmsOf(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(end - begin));
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

/** A reference true-peak meter: 8x oversampling with a long (96-tap) Hann-windowed sinc. */
double truePeakOf(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    constexpr int kHalf = 48;
    constexpr int kOversampling = 8;
    double peak = 0.0;
    for (std::size_t n = begin; n < end; ++n)
        for (int p = 0; p < kOversampling; ++p)
        {
            const double t = static_cast<double>(n) + static_cast<double>(p) / kOversampling;
            double value = 0.0;
            for (int k = -kHalf + 1; k <= kHalf; ++k)
            {
                const auto m = static_cast<long long>(n) + k;
                if (m < 0 || m >= static_cast<long long>(x.size()))
                    continue;
                const double d = t - static_cast<double>(m);
                const double sinc = std::abs(d) < 1.0e-12 ? 1.0 : std::sin(kPi * d) / (kPi * d);
                const double window = 0.5 + 0.5 * std::cos(kPi * d / (kHalf + 1));
                value += x[static_cast<std::size_t>(m)] * sinc * window;
            }
            peak = std::max(peak, std::abs(value));
        }
    return peak;
}

/** The limiter's gain at output sample `at` for a steady input tone of amplitude `inputAmplitude`. */
double gainAround(const std::vector<double>& out, std::size_t at, double inputAmplitude)
{
    // Two periods of 1 kHz.
    return peakOf(out, at - 48, at + 48) / inputAmplitude;
}

std::pair<std::unique_ptr<engine::ProcessorNode>, Limiter*> makeLimiter(float ceilingDb)
{
    auto result = makeNode<Limiter>();
    setParameter(*result.second, "ceiling", ceilingDb);
    setParameter(*result.second, "lookahead", 5.0f);
    result.first->prepare(kRate, kBlock, nullptr);
    return result;
}
} // namespace

TEST_CASE("Limiter Maximize: a lower threshold means louder output, never over the ceiling", "[devices][dynamics][limiter]")
{
    auto [node, limiter] = makeLimiter(-0.3f);
    const double ceiling = dbToGain(static_cast<double>(limiter->ceiling_->get())); // -0.3 dB, as the parameter holds it

    // A quiet passage (-26 dBFS) followed by a loud one (-6 dBFS).
    const std::size_t half = 48'000;
    const auto music = shaped(2 * half, 220.0, [&](std::size_t i) { return i < half ? 0.05 : 0.5; });
    const auto quietRange = std::pair{kLatency + 9'600, half};
    const auto loudRange = std::pair{half + kLatency + 9'600, 2 * half};
    const auto rangeOf = [&](const std::vector<double>& x, std::pair<std::size_t, std::size_t> r)
    { return rmsOf(x, r.first, r.second); };

    // Maximize at 0 dB: the input's 0 dBFS comes out at the ceiling; nothing here reaches it.
    setParameter(*limiter, "maximize", 1.0f);
    setParameter(*limiter, "input", 24.0f); // ignored while Maximize is on
    node->prepare(kRate, kBlock, nullptr);
    const auto unity = run(*node, music);
    CHECK_THAT(rangeOf(unity.left, quietRange) / rangeOf(music.left, {9'600, half}), WithinRel(ceiling, 1.0e-3));
    CHECK(limiter->takeGainReductionDb() < 0.01f);

    // -12 dB: the quiet part is 11.7 dB louder, the loud part is limited at the ceiling.
    setParameter(*limiter, "threshold", -12.0f);
    node->prepare(kRate, kBlock, nullptr);
    const auto loud = run(*node, music);
    CHECK_THAT(gainToDb(rangeOf(loud.left, quietRange) / rangeOf(music.left, {9'600, half})), WithinAbs(11.7, 0.05));
    CHECK(peakOf(loud.left) <= ceiling + 1.0e-12);
    CHECK(peakOf(loud.left, loudRange.first, loudRange.second) > ceiling * 0.99);
    CHECK(limiter->takeGainReductionDb() > 5.0f);

    // Less dynamic range: the loud part was 20 dB above the quiet one, now much less.
    const double before = gainToDb(rangeOf(unity.left, loudRange) / rangeOf(unity.left, quietRange));
    const double after = gainToDb(rangeOf(loud.left, loudRange) / rangeOf(loud.left, quietRange));
    CHECK_THAT(before, WithinAbs(20.0, 0.05));
    CHECK(after < 14.5);
    CHECK(rangeOf(loud.left, {kLatency, 2 * half}) > 2.0 * rangeOf(unity.left, {kLatency, 2 * half})); // louder overall

    // With Maximize off, the threshold does nothing and Input Gain drives again.
    setParameter(*limiter, "maximize", 0.0f);
    setParameter(*limiter, "input", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    const auto off = run(*node, music);
    CHECK_THAT(off.left[half / 2 + kLatency], WithinAbs(music.left[half / 2], 1.0e-12));
}

TEST_CASE("Limiter Auto Release: quick after a short peak, slow after sustained limiting", "[devices][dynamics][limiter]")
{
    const double base = 0.1;
    const std::size_t burstStart = 24'000;
    const auto withBurst = [&](std::size_t length)
    {
        return shaped(burstStart + length + 96'000, 1000.0,
                      [=](std::size_t i) { return i >= burstStart && i < burstStart + length ? 2.0 : base; });
    };
    // The gain 60 ms after the burst has passed the limiter (the level after it is far below the ceiling).
    const auto gainAfter = [&](Limiter& limiter, engine::ProcessorNode& node, std::size_t length, bool autoRelease)
    {
        setParameter(limiter, "autorelease", autoRelease ? 1.0f : 0.0f);
        setParameter(limiter, "release", 80.0f);
        node.prepare(kRate, kBlock, nullptr);
        const auto out = run(node, withBurst(length));
        return gainAround(out.left, burstStart + length + kLatency + 2'880, base);
    };

    auto [node, limiter] = makeLimiter(-6.0f); // the burst needs ~12 dB of gain reduction
    const std::size_t shortBurst = 240;        // 5 ms
    const std::size_t longBurst = 48'000;      // 1 s

    const double fixedShort = gainAfter(*limiter, *node, shortBurst, false);
    const double fixedLong = gainAfter(*limiter, *node, longBurst, false);
    const double autoShort = gainAfter(*limiter, *node, shortBurst, true);
    const double autoLong = gainAfter(*limiter, *node, longBurst, true);
    INFO("fixed " << fixedShort << " / " << fixedLong << ", auto " << autoShort << " / " << autoLong);

    // A fixed release recovers the same way whatever came before.
    CHECK_THAT(fixedShort, WithinAbs(fixedLong, 0.02));
    // Auto: a transient is forgotten quickly (faster than the fixed 80 ms)...
    CHECK(autoShort > 0.85);
    CHECK(autoShort > fixedShort + 0.1);
    // ...sustained limiting lets go slowly (no pumping on dense material).
    CHECK(autoLong < 0.45);
    CHECK(autoLong < fixedLong - 0.1);

    // Either way the ceiling holds.
    setParameter(*limiter, "autorelease", 1.0f);
    node->prepare(kRate, kBlock, nullptr);
    const auto out = run(*node, withBurst(longBurst));
    CHECK(peakOf(out.left) <= dbToGain(-6.0) + 1.0e-12);
}

TEST_CASE("Limiter Stereo links the channels, L/R limits each on its own, Link blends", "[devices][dynamics][limiter]")
{
    // A loud left channel (12 dB over the ceiling) and a quiet right one.
    auto [node, limiter] = makeLimiter(-6.0f);
    const double ceiling = dbToGain(-6.0);
    const double quiet = 0.2;
    const auto input = shaped(48'000, 1000.0, [](std::size_t) { return 2.0; }, [=](std::size_t) { return quiet; });

    const auto rightGainDb = [&](const juce::String& channels, float link)
    {
        setParameter(*limiter, "channels", channels == "L/R" ? 1.0f : 0.0f);
        setParameter(*limiter, "link", link);
        node->prepare(kRate, kBlock, nullptr);
        const auto out = run(*node, input);
        CHECK(peakOf(out.left) <= ceiling + 1.0e-12);
        CHECK(peakOf(out.right) <= ceiling + 1.0e-12);
        CHECK(peakOf(out.left, 24'000, 48'000) > ceiling * 0.99); // the loud side is at the ceiling
        return gainToDb(peakOf(out.right, 24'000, 48'000) / quiet);
    };
    const double needed = gainToDb(ceiling / 2.0); // -12 dB

    // Stereo: the quiet side gets the loud side's gain reduction (the image stays put).
    CHECK_THAT(rightGainDb("Stereo", 0.0f), WithinAbs(needed, 0.05));
    CHECK_THAT(rightGainDb("Stereo", 50.0f), WithinAbs(needed, 0.05)); // Link only matters in L/R
    // L/R: the quiet side is untouched.
    CHECK_THAT(rightGainDb("L/R", 0.0f), WithinAbs(0.0, 0.01));
    // Link blends in dB: half the reduction at 50 %, all of it at 100 %.
    CHECK_THAT(rightGainDb("L/R", 50.0f), WithinAbs(needed / 2.0, 0.05));
    CHECK_THAT(rightGainDb("L/R", 100.0f), WithinAbs(needed, 0.05));
}

TEST_CASE("Limiter Soft Clip curve: linear below the knee, rounding off under the ceiling", "[devices][dynamics][limiter]")
{
    const double ceiling = 0.8;
    CHECK(Limiter::softClip(0.3, ceiling) == 0.3);
    CHECK(Limiter::softClip(-0.4, ceiling) == -0.4); // the knee is half the ceiling
    double previous = 0.0;
    for (double x = 0.0; x <= 4.0; x += 0.01)
    {
        const double y = Limiter::softClip(x, ceiling);
        CHECK(y >= previous); // monotonic
        CHECK(y < ceiling);   // never reaches the ceiling
        CHECK(y <= x);
        CHECK_THAT(Limiter::softClip(-x, ceiling), WithinAbs(-y, 1.0e-15)); // symmetric
        previous = y;
    }
    // Smooth at the knee (no corner): the slope is 1 on both sides.
    const double k = ceiling * Limiter::kSoftClipKnee;
    CHECK_THAT((Limiter::softClip(k + 1.0e-6, ceiling) - Limiter::softClip(k, ceiling)) / 1.0e-6, WithinAbs(1.0, 1.0e-4));
    // At the most the limiter lets through (6 dB over), it is within 0.03 dB of the ceiling.
    CHECK(Limiter::softClip(2.0 * ceiling, ceiling) > ceiling * dbToGain(-0.03));
}

TEST_CASE("Limiter Soft Clip saturates the peaks instead of turning them down", "[devices][dynamics][limiter]")
{
    auto [node, limiter] = makeLimiter(-1.0f);
    const double ceiling = dbToGain(-1.0);
    const auto hot = shaped(48'000, 500.0, [&](std::size_t) { return ceiling * dbToGain(4.0); }); // 4 dB over

    setParameter(*limiter, "mode", 0.0f); // Standard
    node->prepare(kRate, kBlock, nullptr);
    const auto standard = run(*node, hot);
    const double standardReduction = limiter->takeGainReductionDb();

    setParameter(*limiter, "mode", 1.0f); // Soft Clip
    node->prepare(kRate, kBlock, nullptr);
    CHECK(node->getLatencySamples() == static_cast<int>(kLatency)); // no extra latency
    const auto soft = run(*node, hot);
    const double softReduction = limiter->takeGainReductionDb();

    const std::size_t from = 24'000, to = 48'000;
    const auto thd = [&](const std::vector<double>& x)
    { return amplitudeAt(x, 1500.0, from, to) / amplitudeAt(x, 500.0, from, to); };
    INFO("THD standard " << thd(standard.left) << ", soft " << thd(soft.left));

    CHECK(standardReduction > 3.9f);
    CHECK(softReduction < 0.01f);            // within its 6 dB headroom: no gain reduction...
    CHECK(peakOf(soft.left) < ceiling);      // ...the curve keeps it under the ceiling
    CHECK(thd(standard.left) < 0.002);       // the limiter is clean on a steady tone
    CHECK(thd(soft.left) > 0.03);            // the clipper adds harmonics (warmth / crunch)
    CHECK(rmsOf(soft.left, from, to) > rmsOf(standard.left, from, to) * 1.1); // and is louder

    // Far over the headroom the limiter takes the rest; the output still stays under the ceiling.
    const auto hotter = shaped(48'000, 500.0, [&](std::size_t) { return ceiling * dbToGain(18.0); });
    node->prepare(kRate, kBlock, nullptr);
    const auto clipped = run(*node, hotter);
    CHECK(peakOf(clipped.left) < ceiling);
    CHECK_THAT(limiter->takeGainReductionDb(), WithinAbs(12.0, 0.1));

    // Quiet material (below the knee) passes untouched.
    const auto quiet = shaped(9'600, 500.0, [&](std::size_t) { return ceiling * 0.45; });
    node->prepare(kRate, kBlock, nullptr);
    const auto clean = run(*node, quiet);
    for (std::size_t i = kLatency; i < quiet.left.size(); i += 97)
        CHECK_THAT(clean.left[i], WithinAbs(quiet.left[i - kLatency], 1.0e-12));
}

TEST_CASE("Limiter True Peak keeps the peaks between samples under the ceiling", "[devices][dynamics][limiter]")
{
    auto [node, limiter] = makeLimiter(-1.0f);
    const double ceiling = dbToGain(-1.0);

    // A quarter-rate tone at 45 degrees: every sample is at 0.707, the waveform peaks at 1.0, 3 dB higher.
    const std::size_t n = 24'000;
    Stereo between(n);
    for (std::size_t i = 0; i < n; ++i)
        between.left[i] = between.right[i] = std::sin(kPi / 2.0 * static_cast<double>(i) + kPi / 4.0);
    REQUIRE(peakOf(between.left) < ceiling);
    REQUIRE(truePeakOf(between.left, 1'000, 1'100) > 0.99);

    // Standard only sees the samples: they are under the ceiling, so nothing is done.
    const auto standard = run(*node, between);
    CHECK(limiter->takeGainReductionDb() < 0.01f);
    CHECK(truePeakOf(standard.left, 12'000, 12'400) > 0.99);

    // True Peak: 8 samples more latency, and the reconstructed waveform stays under the ceiling.
    setParameter(*limiter, "mode", 2.0f);
    CHECK(node->getLatencySamples() == static_cast<int>(kLatency) + Limiter::kTruePeakDelay);
    node->prepare(kRate, kBlock, nullptr);
    (void) run(*node, between); // past the onset (whose ringing reads a little higher)
    (void) limiter->takeGainReductionDb();
    const auto limited = run(*node, between); // the tone repeats every 4 samples: seamless
    // The true peak is at 0 dB; the limiter aims a hair under the ceiling.
    CHECK_THAT(limiter->takeGainReductionDb(), WithinAbs(1.0 + Limiter::kTruePeakMarginDb, 0.02));
    CHECK(truePeakOf(limited.left, 12'000, 12'400) <= ceiling);
    CHECK(truePeakOf(limited.left, 12'000, 12'400) > ceiling * dbToGain(-0.1));

    // Dense, bright material driven 6 dB into the limiter: noise up to 19.5 kHz, peaking at 0 dBFS.
    juce::Random random(1234);
    std::vector<double> lowpass(127);
    for (std::size_t k = 0; k < lowpass.size(); ++k)
    {
        const double d = static_cast<double>(k) - 63.0;
        const double cutoff = 19'500.0 / kRate;
        const double sinc = std::abs(d) < 0.5 ? 2.0 * cutoff : std::sin(2.0 * kPi * cutoff * d) / (kPi * d);
        lowpass[k] = sinc * (0.42 + 0.5 * std::cos(kPi * d / 64.0) + 0.08 * std::cos(2.0 * kPi * d / 64.0));
    }
    Stereo noise(n);
    for (auto* channel : {&noise.left, &noise.right})
    {
        std::vector<double> white(n + lowpass.size());
        for (auto& v : white)
            v = random.nextDouble() * 2.0 - 1.0;
        for (std::size_t i = 0; i < n; ++i)
            for (std::size_t k = 0; k < lowpass.size(); ++k)
                (*channel)[i] += white[i + k] * lowpass[k];
        const double scale = 1.0 / peakOf(*channel);
        for (auto& v : *channel)
            v *= scale;
    }
    setParameter(*limiter, "input", 6.0f);
    const auto overshootDb = [&](float mode)
    {
        setParameter(*limiter, "mode", mode);
        node->prepare(kRate, kBlock, nullptr);
        const auto out = run(*node, noise);
        return gainToDb(std::max(truePeakOf(out.left, 2'400, n - 100), truePeakOf(out.right, 2'400, n - 100)) / ceiling);
    };
    const double standardOvershoot = overshootDb(0.0f);
    const double truePeakOvershoot = overshootDb(2.0f);
    INFO("overshoot: standard " << standardOvershoot << " dB, true peak " << truePeakOvershoot << " dB");
    CHECK(standardOvershoot > 0.5);   // sample peaks are held, the waveform between them is not
    CHECK(truePeakOvershoot <= 0.0);

    // Delay compensation: an impulse comes out exactly at the reported latency.
    setParameter(*limiter, "input", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    Stereo impulse(4'800);
    impulse.left[1'000] = impulse.right[1'000] = 0.5;
    const auto delayed = run(*node, impulse);
    CHECK(test::indexOfPeak(delayed.left) == 1'000 + static_cast<std::size_t>(node->getLatencySamples()));

    setParameter(*limiter, "mode", 0.0f);
    CHECK(node->getLatencySamples() == static_cast<int>(kLatency));
}

TEST_CASE("Limiter is real-time safe in every mode and keeps its new settings", "[devices][dynamics][limiter]")
{
    auto [node, limiter] = makeLimiter(-0.5f);
    setParameter(*limiter, "maximize", 1.0f);
    setParameter(*limiter, "threshold", -9.0f);
    setParameter(*limiter, "autorelease", 1.0f);
    setParameter(*limiter, "channels", 1.0f);
    setParameter(*limiter, "link", 30.0f);
    const auto music = shaped(9'600, 330.0, [](std::size_t i) { return 0.3 + 0.7 * std::abs(std::sin(static_cast<double>(i) * 0.001)); });

    for (const float mode : {0.0f, 1.0f, 2.0f})
    {
        setParameter(*limiter, "mode", mode);
        node->prepare(kRate, kBlock, nullptr);
        const auto violationsBefore = rt::getRealtimeViolationCount();
        const auto out = run(*node, music);
        if (rt::isAllocationTrapInstalled())
            CHECK(rt::getRealtimeViolationCount() == violationsBefore);
        CHECK(peakOf(out.left) <= dbToGain(-0.5) + 1.0e-12);
        CHECK(std::isfinite(out.right[5'000]));
    }

    juce::MemoryBlock state;
    limiter->getStateInformation(state);
    Limiter copy;
    copy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(copy.maximize_->get());
    CHECK_THAT(copy.threshold_->get(), WithinAbs(-9.0, 1.0e-4));
    CHECK(copy.autoRelease_->get());
    CHECK(copy.mode_->getIndex() == 2);
    CHECK(copy.channels_->getIndex() == 1);
    CHECK_THAT(copy.link_->get(), WithinAbs(30.0, 1.0e-4));
    CHECK_THAT(copy.ceiling_->get(), WithinAbs(-0.5, 1.0e-4));

    // A project saved before these settings existed (only the four original parameters) loads with
    // the new ones at their defaults: it sounds as it did.
    juce::XmlElement old("DOJO_DEVICE");
    old.setAttribute("type", Limiter::kTypeId);
    old.setAttribute("version", 1);
    const std::initializer_list<std::pair<const char*, double>> original{{"input", 3.0}, {"ceiling", -2.0}, {"release", 120.0}, {"lookahead", 4.0}};
    for (const auto& [id, value] : original)
    {
        auto* element = old.createNewChildElement("PARAM");
        element->setAttribute("id", id);
        element->setAttribute("value", value);
    }
    juce::MemoryBlock oldState;
    juce::AudioProcessor::copyXmlToBinary(old, oldState);
    Limiter loaded;
    loaded.setStateInformation(oldState.getData(), static_cast<int>(oldState.getSize()));
    CHECK_THAT(loaded.input_->get(), WithinAbs(3.0, 1.0e-4));
    CHECK_THAT(loaded.ceiling_->get(), WithinAbs(-2.0, 1.0e-4));
    CHECK_THAT(loaded.release_->get(), WithinAbs(120.0, 0.5));
    CHECK_THAT(loaded.lookahead_->get(), WithinAbs(4.0, 1.0e-4));
    CHECK(!loaded.maximize_->get());
    CHECK(!loaded.autoRelease_->get());
    CHECK(loaded.mode_->getIndex() == 0);
    CHECK(loaded.channels_->getIndex() == 0);

    CHECK(!loaded.mode_->isAutomatable()); // it changes the latency
    CHECK(!loaded.lookahead_->isAutomatable());
    CHECK(loaded.channels_->isAutomatable());
    CHECK(loaded.maximize_->isAutomatable());
}

TEST_CASE("Limiter editor shows every setting and dims the ones that do nothing", "[devices][dynamics][limiter][ui]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    ui::EditorState editorState;
    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::audio, "Master Bus");
    auto device = session.addDevice(track, Limiter::kTypeId, "Limiter");
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto& limiter = dynamic_cast<Limiter&>(node->getProcessor());

    int width = 0;
    auto editor = ui::createDeviceEditor(*node, ui::DeviceEditorContext{project, editorState, device, track}, width);
    REQUIRE(editor != nullptr);
    CHECK(width >= 5 * ui::ParameterControl::kWidth);
    editor->setSize(width, 160);

    std::map<juce::String, ui::ParameterControl*> controls;
    for (auto* child : editor->getChildren())
        if (auto* control = dynamic_cast<ui::ParameterControl*>(child))
            if (auto* withId = dynamic_cast<juce::AudioProcessorParameterWithID*>(&control->getParameter()))
                controls[withId->paramID] = control;
    for (const char* id : {"maximize", "threshold", "input", "ceiling", "mode", "autorelease", "release", "lookahead", "channels", "link"})
    {
        INFO(id);
        REQUIRE(controls.count(id) == 1);
        CHECK(editor->getLocalBounds().contains(controls[id]->getBounds())); // laid out inside the editor
    }
    CHECK(controls["maximize"]->getY() < controls["autorelease"]->getY()); // two rows
    CHECK(controls["maximize"]->getX() == controls["autorelease"]->getX());

    // Defaults: Maximize off (Threshold idle), fixed release, Stereo (Link idle).
    CHECK(controls["input"]->isEnabled());
    CHECK(!controls["threshold"]->isEnabled());
    CHECK(controls["release"]->isEnabled());
    CHECK(!controls["link"]->isEnabled());

    setParameter(limiter, "maximize", 1.0f);
    setParameter(limiter, "autorelease", 1.0f);
    setParameter(limiter, "channels", 1.0f);
    CHECK(test::pumpMessagesUntil([&] { return controls["threshold"]->isEnabled(); }, 2'000));
    CHECK(!controls["input"]->isEnabled());
    CHECK(!controls["release"]->isEnabled());
    CHECK(controls["link"]->isEnabled());
}

TEST_CASE("Changing the limiter's latency re-aligns the delay compensation", "[devices][dynamics][limiter][pdc]")
{
    juce::MessageManager::getInstance();
    int published = 0;
    session::Project project([&](std::unique_ptr<engine::RenderGraph>) { ++published; },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::audio, "Vocals");
    session.addTrack(session::TrackKind::audio, "Guitar");
    auto device = session.addDevice(track, Limiter::kTypeId, "Limiter");
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto& limiter = dynamic_cast<Limiter&>(node->getProcessor());
    auto& compiler = project.getCompiler();
    REQUIRE(test::pumpMessagesUntil([&] { return compiler.getTotalLatency() == 240; }, 3'000));

    // No other edit happens: the new latency alone makes the engine re-align the tracks.
    setParameter(limiter, "mode", 2.0f); // True Peak
    CHECK(test::pumpMessagesUntil([&] { return compiler.getTotalLatency() == 240 + Limiter::kTruePeakDelay; }, 3'000));
    setParameter(limiter, "lookahead", 2.0f);
    CHECK(test::pumpMessagesUntil([&] { return compiler.getTotalLatency() == 96 + Limiter::kTruePeakDelay; }, 3'000));
    setParameter(limiter, "mode", 1.0f); // Soft Clip: no detector delay
    CHECK(test::pumpMessagesUntil([&] { return compiler.getTotalLatency() == 96; }, 3'000));
    CHECK(published > 1);
}

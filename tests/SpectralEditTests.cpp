// Spectral editing: region gain and erase, repair, de-noise, de-hum, the spectrogram; the session's
// spectral edits and the spectral editor.

#include "session/audioedit/Spectral.h"
#include "session/audioedit/Stft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace dojo;
using namespace dojo::audioedit;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 44100.0;

void addSine(std::vector<float>& x, double hz, double amplitude, double from = 0.0, double to = 1.0e9)
{
    const auto a = static_cast<std::size_t>(from * kRate);
    const auto b = std::min(x.size(), static_cast<std::size_t>(to * kRate));
    for (std::size_t i = a; i < b; ++i)
        x[i] += static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
}

/** The level (dB) of one frequency in [from, to) (Goertzel, Hann-windowed). */
double levelDb(const float* x, double hz, double from, double to)
{
    const auto a = static_cast<std::size_t>(from * kRate);
    const auto n = static_cast<std::size_t>((to - from) * kRate);
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    double s1 = 0.0, s2 = 0.0, windowSum = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
        windowSum += win;
        const double s0 = static_cast<double>(x[a + i]) * win + 2.0 * std::cos(w) * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - 2.0 * std::cos(w) * s1 * s2;
    return 20.0 * std::log10(2.0 * std::sqrt(std::max(0.0, power)) / windowSum + 1.0e-12);
}

double rmsDb(const float* x, double from, double to)
{
    double sum = 0.0;
    const auto a = static_cast<std::size_t>(from * kRate), b = static_cast<std::size_t>(to * kRate);
    for (std::size_t i = a; i < b; ++i)
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return 10.0 * std::log10(sum / static_cast<double>(b - a) + 1.0e-20);
}

juce::AudioBuffer<float> toBuffer(const std::vector<float>& x, int channels = 1)
{
    juce::AudioBuffer<float> b(channels, static_cast<int>(x.size()));
    for (int ch = 0; ch < channels; ++ch)
        std::copy(x.begin(), x.end(), b.getWritePointer(ch));
    return b;
}

spectral::Operation op(spectral::Kind kind, double start, double end, double low = 0.0, double high = 1.0e6)
{
    spectral::Operation o;
    o.kind = kind;
    o.start = start;
    o.end = end;
    o.low = low;
    o.high = high;
    return o;
}
} // namespace

TEST_CASE("Spectral editing: a region's gain and erasure, in its band and time only; the rest untouched", "[spectral][dsp]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 2.0), 0.0f);
    addSine(x, 1000.0, 0.3);
    addSine(x, 3000.0, 0.3);
    const auto input = toBuffer(x, 2);
    auto gain = op(spectral::Kind::gain, 0.6, 1.2, 800.0, 1200.0);
    gain.gainDb = -20.0;
    const auto out = spectral::apply(input, kRate, {gain});
    REQUIRE(out.getNumSamples() == input.getNumSamples());
    REQUIRE(out.getNumChannels() == 2);
    const float* y = out.getReadPointer(1);
    CHECK_THAT(levelDb(y, 1000.0, 0.7, 1.1) - levelDb(x.data(), 1000.0, 0.7, 1.1), WithinAbs(-20.0, 1.0));
    CHECK_THAT(levelDb(y, 3000.0, 0.7, 1.1) - levelDb(x.data(), 3000.0, 0.7, 1.1), WithinAbs(0.0, 0.3));
    CHECK_THAT(levelDb(y, 1000.0, 1.4, 1.9) - levelDb(x.data(), 1000.0, 1.4, 1.9), WithinAbs(0.0, 0.1));
    for (int i = 0; i < input.getNumSamples(); ++i)
        if (i < static_cast<int>(0.54 * kRate) || i > static_cast<int>(1.26 * kRate))
            REQUIRE(out.getSample(0, i) == input.getSample(0, i));

    const auto erased = spectral::apply(input, kRate, {op(spectral::Kind::erase, 0.5, 1.5, 2500.0, 3500.0)});
    CHECK(levelDb(erased.getReadPointer(0), 3000.0, 0.7, 1.3) < levelDb(x.data(), 3000.0, 0.7, 1.3) - 40.0);
    CHECK_THAT(levelDb(erased.getReadPointer(0), 1000.0, 0.7, 1.3) - levelDb(x.data(), 1000.0, 0.7, 1.3), WithinAbs(0.0, 0.3));

    // Disabled operations do nothing; the key follows what counts.
    auto disabled = gain;
    disabled.enabled = false;
    CHECK(spectral::keyOf({disabled}) == spectral::keyOf({}));
    CHECK(spectral::keyOf({gain}) != spectral::keyOf({}));
    auto louder = gain;
    louder.gainDb = -6.0;
    CHECK(spectral::keyOf({gain}) != spectral::keyOf({louder}));
}

TEST_CASE("Spectral repair rebuilds a click out of a tone from what surrounds it", "[spectral][dsp]")
{
    std::vector<float> clean(static_cast<std::size_t>(kRate * 2.0), 0.0f);
    addSine(clean, 440.0, 0.3);
    auto damaged = clean;
    std::mt19937 random(7);
    std::uniform_real_distribution<float> noise(-0.8f, 0.8f);
    for (std::size_t i = static_cast<std::size_t>(1.0 * kRate); i < static_cast<std::size_t>(1.004 * kRate); ++i)
        damaged[i] += noise(random);
    const auto repaired = spectral::apply(toBuffer(damaged), kRate, {op(spectral::Kind::repair, 0.998, 1.006)});
    double before = 0.0, after = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.9 * kRate); i < static_cast<std::size_t>(1.1 * kRate); ++i)
    {
        before = std::max(before, std::abs(static_cast<double>(damaged[i] - clean[i])));
        after = std::max(after, std::abs(static_cast<double>(repaired.getSample(0, static_cast<int>(i)) - clean[i])));
    }
    CHECK(before > 0.5);
    CHECK(after < 0.06);
    CHECK_THAT(levelDb(repaired.getReadPointer(0), 440.0, 0.95, 1.05), WithinAbs(levelDb(clean.data(), 440.0, 0.95, 1.05), 0.5));
}

TEST_CASE("De-noise from a learnt profile; de-hum at the mains frequency it finds", "[spectral][dsp]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 3.0), 0.0f);
    std::mt19937 random(3);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    for (auto& v : x)
        v = noise(random);
    addSine(x, 1000.0, 0.3, 1.0);
    const auto input = toBuffer(x);
    auto denoise = op(spectral::Kind::denoise, 0.0, 3.0);
    denoise.profile = spectral::learnProfile(input, kRate, 0.1, 0.9);
    REQUIRE(static_cast<int>(denoise.profile.size()) == spectral::fftSizeFor(kRate) / 2 + 1);
    denoise.reductionDb = 24.0;
    const auto clean = spectral::apply(input, kRate, {denoise});
    CHECK(rmsDb(clean.getReadPointer(0), 0.2, 0.8) < rmsDb(x.data(), 0.2, 0.8) - 12.0);
    CHECK_THAT(levelDb(clean.getReadPointer(0), 1000.0, 1.5, 2.5) - levelDb(x.data(), 1000.0, 1.5, 2.5), WithinAbs(0.0, 1.0));

    for (const double mains : {60.0, 50.0})
    {
        std::vector<float> h(static_cast<std::size_t>(kRate * 2.0), 0.0f);
        addSine(h, 440.0, 0.3);
        addSine(h, mains, 0.1);
        addSine(h, 2.0 * mains, 0.05);
        addSine(h, 3.0 * mains, 0.03);
        const auto humming = toBuffer(h);
        CHECK_THAT(spectral::detectHum(humming, kRate), WithinAbs(mains, 0.1));
        const auto dehummed = spectral::apply(humming, kRate, {op(spectral::Kind::dehum, 0.0, 2.0)});
        for (int k = 1; k <= 3; ++k)
            CHECK(levelDb(dehummed.getReadPointer(0), k * mains, 0.5, 1.5) < levelDb(h.data(), k * mains, 0.5, 1.5) - 20.0);
        CHECK_THAT(levelDb(dehummed.getReadPointer(0), 440.0, 0.5, 1.5) - levelDb(h.data(), 440.0, 0.5, 1.5), WithinAbs(0.0, 0.5));
    }
}

TEST_CASE("The spectrogram shows a tone in its row, at its level", "[spectral][dsp]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 1.0), 0.0f);
    addSine(x, 1000.0, 0.3, 0.25, 0.75);
    const auto s = spectral::analyse(toBuffer(x), kRate, 384);
    REQUIRE(s.frames > 50);
    REQUIRE(s.rows == 384);
    CHECK_THAT(s.duration, WithinAbs(1.0, 1e-9));
    const int middle = static_cast<int>(0.5 / s.hop);
    int brightest = 0;
    for (int r = 0; r < s.rows; ++r)
        if (s.at(middle, r) > s.at(middle, brightest))
            brightest = r;
    CHECK_THAT(s.rowHz(brightest), WithinAbs(1000.0, 30.0));
    CHECK_THAT(s.hzRow(s.rowHz(100.0)), WithinAbs(100.0, 1e-9));
    const double db = s.at(middle, brightest) / 255.0 * 110.0 - 110.0;
    CHECK_THAT(db, WithinAbs(20.0 * std::log10(0.3), 1.5));
    CHECK(s.at(static_cast<int>(0.1 / s.hop), brightest) < 40); // (silence before)
}

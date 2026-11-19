#include "core/rt/RealtimeGuard.h"
#include "engine/dsp/SurroundVirtualizer.h"
#include "TestSupport.h"

#include <juce_dsp/juce_dsp.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <random>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using engine::ChannelLayout;
using engine::Speaker;
using engine::dsp::SurroundVirtualizer;

namespace
{
constexpr double kRate = 48'000.0;
constexpr double kPi = 3.14159265358979323846;
const double kHeadTime = SurroundVirtualizer::kHeadRadius / SurroundVirtualizer::kSpeedOfSound;

double rad(double degrees) { return degrees * kPi / 180.0; }

struct Ears
{
    std::vector<double> left, right;
};

/** The virtualizer's two outputs for `input` on one speaker of `layout` (the others silent). */
Ears render(SurroundVirtualizer& v, const ChannelLayout& layout, Speaker speaker, const std::vector<double>& input)
{
    const int channel = layout.indexOf(speaker);
    REQUIRE(channel >= 0);
    std::vector<std::vector<double>> channels(static_cast<std::size_t>(layout.numChannels), std::vector<double>(input.size(), 0.0));
    channels[static_cast<std::size_t>(channel)] = input;
    std::vector<const double*> pointers;
    for (auto& c : channels)
        pointers.push_back(c.data());
    Ears ears{std::vector<double>(input.size()), std::vector<double>(input.size())};
    v.process(pointers.data(), layout.numChannels, ears.left.data(), ears.right.data(), static_cast<int>(input.size()));
    return ears;
}

Ears renderFresh(const ChannelLayout& layout, Speaker speaker, const std::vector<double>& input, double room)
{
    SurroundVirtualizer v;
    v.prepare(kRate);
    v.configure(layout, SurroundVirtualizer::Output::headphones, room);
    return render(v, layout, speaker, input);
}

std::vector<double> noise(std::size_t n, unsigned seed)
{
    std::mt19937 random(seed);
    std::normal_distribution<double> d(0.0, 0.3);
    std::vector<double> x(n);
    for (auto& v : x)
        v = d(random);
    return x;
}

double energy(const std::vector<double>& x, std::size_t from = 0)
{
    double e = 0.0;
    for (std::size_t i = from; i < x.size(); ++i)
        e += x[i] * x[i];
    return e;
}

/** The energy of x[from, to). */
double energyBetween(const std::vector<double>& x, std::size_t from, std::size_t to)
{
    double e = 0.0;
    for (std::size_t i = from; i < to && i < x.size(); ++i)
        e += x[i] * x[i];
    return e;
}

/** The energy of `x` in a narrow band around `frequency` (a band-pass of Q 8). */
double bandEnergy(const std::vector<double>& x, double frequency)
{
    juce::dsp::IIR::Filter<double> band;
    band.coefficients = juce::dsp::IIR::Coefficients<double>::makeBandPass(kRate, frequency, 8.0);
    double e = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        const double y = band.processSample(x[i]);
        if (i >= 4800)
            e += y * y;
    }
    return e;
}

/** The share of `x`'s energy above `frequency` (one-pole split). */
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

/** The lag (samples, fractional) at which `b` best matches `a`. */
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

/** Woodworth's time difference for a source at `lateral` (radians from the median plane). */
double woodworth(double lateral) { return kHeadTime * (lateral + std::sin(lateral)); }
} // namespace

TEST_CASE("Height virtualizer geometry: the top layer 30 degrees up, angles to the ears in 3D", "[surround][virtualizer][immersive]")
{
    // Every top speaker is 30 degrees up; the ear-level ones (and the LFE) are not.
    for (const auto& layout : {ChannelLayout::surround514(), ChannelLayout::surround714(), ChannelLayout::surround916()})
        for (int c = 0; c < layout.numChannels; ++c)
        {
            const auto speaker = layout.speakers[static_cast<std::size_t>(c)];
            CHECK(SurroundVirtualizer::elevationOf(speaker) == (engine::isHeight(speaker) ? 30.0 : 0.0));
        }
    // The tops' azimuths (the virtualizer's own convention: positive to the right).
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::topFrontLeft, false) == -30.0);  // 5.1.4
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::topFrontLeft, true) == -45.0);   // 7.1.4, 9.1.6
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::topRearRight, false) == 110.0);
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::topRearRight, true) == 135.0);
    CHECK(SurroundVirtualizer::azimuthOf(Speaker::topMiddleLeft, true) == -90.0);

    // At ear level the 3D angle is the old one; above, a source is less lateral.
    for (const double azimuth : {-150.0, -90.0, -30.0, 0.0, 45.0, 110.0, 180.0})
        for (const bool right : {false, true})
            CHECK_THAT(SurroundVirtualizer::incidence(azimuth, 0.0, right), WithinAbs(SurroundVirtualizer::incidence(azimuth, right), 1e-12));
    CHECK_THAT(SurroundVirtualizer::incidence(-90.0, 30.0, false), WithinAbs(rad(30.0), 1e-12));  // 30 deg from the left ear
    CHECK_THAT(SurroundVirtualizer::incidence(-90.0, 30.0, true), WithinAbs(rad(150.0), 1e-12));
    CHECK_THAT(SurroundVirtualizer::incidence(70.0, 90.0, false), WithinAbs(kPi / 2.0, 1e-12)); // straight above: as far from both
    CHECK_THAT(SurroundVirtualizer::incidence(70.0, 90.0, true), WithinAbs(kPi / 2.0, 1e-12));

    // The time difference of a top middle speaker (90 deg round, 30 up) is Woodworth's for 60 deg.
    const auto itd = [](double azimuth, double elevation)
    {
        return SurroundVirtualizer::earDelay(SurroundVirtualizer::incidence(azimuth, elevation, true))
               - SurroundVirtualizer::earDelay(SurroundVirtualizer::incidence(azimuth, elevation, false));
    };
    CHECK_THAT(itd(-90.0, 30.0), WithinAbs(woodworth(rad(60.0)), 1e-12));
    CHECK_THAT(itd(-90.0, 30.0), WithinAbs(0.000488, 0.000002)); // (0.66 ms at ear level)
    CHECK(std::abs(itd(-45.0, 30.0)) < std::abs(itd(-45.0, 0.0)));
    CHECK_THAT(itd(0.0, 30.0), WithinAbs(0.0, 1e-15));
}

TEST_CASE("Height virtualizer room: floor and ceiling reflections from where they come", "[surround][virtualizer][immersive]")
{
    constexpr double d = SurroundVirtualizer::kSpeakerDistance;
    constexpr double below = SurroundVirtualizer::kEarHeight;                                       // the floor
    constexpr double above = SurroundVirtualizer::kRoomHeight - SurroundVirtualizer::kEarHeight;    // the ceiling
    const auto top = SurroundVirtualizer::arrivals(-30.0, 30.0, 1.0);  // 5.1.4's Ltf
    const auto bed = SurroundVirtualizer::arrivals(-30.0, 0.0, 1.0);   // and L, under it

    // The direct sound.
    CHECK_THAT(top[0].distance, WithinAbs(d, 1e-12));
    CHECK_THAT(top[0].azimuth, WithinAbs(-30.0, 1e-9));
    CHECK_THAT(top[0].elevation, WithinAbs(30.0, 1e-9));
    CHECK(top[0].gain == 1.0);

    // Image sources: mirrored in the floor and the ceiling (horizontal distance r, height h).
    const double r = d * std::cos(rad(30.0)), h = d * std::sin(rad(30.0));
    CHECK_THAT(top[1].distance, WithinAbs(std::hypot(r, 2.0 * below + h), 1e-9)); // down to the floor and up
    CHECK_THAT(top[2].distance, WithinAbs(std::hypot(r, 2.0 * above - h), 1e-9)); // up to the ceiling and down
    CHECK_THAT(top[1].elevation, WithinAbs(-std::atan2(2.0 * below + h, r) * 180.0 / kPi, 1e-9));
    CHECK_THAT(top[2].elevation, WithinAbs(std::atan2(2.0 * above - h, r) * 180.0 / kPi, 1e-9));
    CHECK(top[2].elevation > 45.0); // (steeply from above)
    CHECK(top[1].elevation < -60.0);
    CHECK_THAT(top[1].azimuth, WithinAbs(-30.0, 1e-9)); // (the floor and the ceiling: the same bearing)
    CHECK_THAT(top[2].azimuth, WithinAbs(-30.0, 1e-9));
    CHECK(top[3].azimuth < -30.0);                       // the left wall's reflection: further left
    CHECK(std::abs(top[4].azimuth) < 30.0);              // the front wall's: further ahead
    // ...mirrored in the left wall (x) and the front wall (y), at the same height.
    const double x = r * std::sin(rad(30.0)), y = r * std::cos(rad(30.0));
    CHECK_THAT(top[3].distance, WithinAbs(std::sqrt(std::pow(SurroundVirtualizer::kRoomWidth - x, 2.0) + y * y + h * h), 1e-9));
    CHECK_THAT(top[4].distance, WithinAbs(std::sqrt(x * x + std::pow(SurroundVirtualizer::kRoomLength - y, 2.0) + h * h), 1e-9));

    // Height-aware: a top speaker's ceiling reflection comes before its floor reflection, an
    // ear-level speaker's after it.
    CHECK(top[2].distance < top[1].distance);
    CHECK(bed[1].distance < bed[2].distance);
    CHECK(top[2].distance < bed[2].distance);
    CHECK(top[1].distance > bed[1].distance);

    // The levels: spherical spreading from each surface, scaled by the room; none without one.
    for (std::size_t i = 1; i < top.size(); ++i)
    {
        CHECK_THAT(top[i].gain * top[i].distance, WithinRel(bed[i].gain * bed[i].distance, 1e-12));
        CHECK(top[i].gain < 1.0);
        CHECK(SurroundVirtualizer::arrivals(-30.0, 30.0, 0.0)[i].gain == 0.0);
        CHECK_THAT(SurroundVirtualizer::arrivals(-30.0, 30.0, 0.5)[i].gain, WithinRel(0.5 * top[i].gain, 1e-12));
    }
}

TEST_CASE("Height virtualizer pinna: the first notch rises from 4 to 12 kHz with elevation", "[surround][virtualizer][immersive]")
{
    // The notch: 4 kHz straight below, 6.9 kHz at ear level, 8.3 kHz at the top layer, 12 kHz above.
    CHECK_THAT(SurroundVirtualizer::pinnaCue(0.0, -90.0).notchFrequency, WithinAbs(4000.0, 1e-6));
    CHECK_THAT(SurroundVirtualizer::pinnaCue(0.0, 0.0).notchFrequency, WithinAbs(4000.0 * std::sqrt(3.0), 1e-6));
    CHECK_THAT(SurroundVirtualizer::pinnaCue(0.0, 30.0).notchFrequency, WithinAbs(4000.0 * std::pow(3.0, 2.0 / 3.0), 1e-6));
    CHECK_THAT(SurroundVirtualizer::pinnaCue(0.0, 90.0).notchFrequency, WithinAbs(12000.0, 1e-6));
    double previous = 0.0;
    for (double elevation = -90.0; elevation <= 90.0; elevation += 5.0)
    {
        const auto cue = SurroundVirtualizer::pinnaCue(0.0, elevation);
        CHECK(cue.notchFrequency > previous);
        CHECK(cue.notchFrequency >= 4000.0);
        CHECK(cue.notchFrequency <= 12000.0);
        previous = cue.notchFrequency;
        // 12 dB deep up to 30 degrees, fading out towards the zenith; the overhead band rising.
        CHECK_THAT(cue.notchDepth, WithinAbs(elevation <= 30.0 ? 12.0 : 12.0 * (90.0 - elevation) / 60.0, 1e-9));
        CHECK_THAT(cue.overheadGain, WithinAbs(4.0 * std::max(0.0, std::sin(rad(elevation))), 1e-9));
        CHECK(cue.rearShelf == 0.0); // (ahead)
    }
    // Behind: the high shelf, less of it for a source behind and above.
    CHECK_THAT(SurroundVirtualizer::pinnaCue(180.0, 0.0).rearShelf, WithinAbs(-8.0, 1e-9));
    CHECK_THAT(SurroundVirtualizer::pinnaCue(135.0, 30.0).rearShelf, WithinAbs(-8.0 * std::cos(rad(45.0)) * std::cos(rad(30.0)), 1e-9));
    CHECK(SurroundVirtualizer::pinnaCue(90.0, 0.0).rearShelf > -1e-9);

    // The filters as built: a notch where the cue says, nothing in the bass.
    for (const double elevation : {-45.0, 0.0, 30.0})
    {
        const double f = SurroundVirtualizer::pinnaCue(0.0, elevation).notchFrequency;
        const double atNotch = SurroundVirtualizer::pinnaGain(0.0, elevation, f, kRate);
        CHECK(atNotch < SurroundVirtualizer::pinnaGain(0.0, elevation, 1000.0, kRate) - 9.0);
        CHECK(atNotch < SurroundVirtualizer::pinnaGain(0.0, elevation, f / 1.5, kRate) - 6.0);
        CHECK(atNotch < SurroundVirtualizer::pinnaGain(0.0, elevation, f * 1.5, kRate) - 6.0);
    }
    // Overhead: no notch left, the 8 kHz band raised.
    CHECK(SurroundVirtualizer::pinnaGain(0.0, 90.0, 8000.0, kRate) > SurroundVirtualizer::pinnaGain(0.0, 90.0, 1000.0, kRate) + 3.0);

    // The cue of the top layer against the bed under it: more at the bed's notch, less at its own.
    const auto relative = [](double frequency)
    { return SurroundVirtualizer::pinnaGain(-30.0, 30.0, frequency, kRate) - SurroundVirtualizer::pinnaGain(-30.0, 0.0, frequency, kRate); };
    CHECK(relative(SurroundVirtualizer::pinnaCue(0.0, 0.0).notchFrequency) > 8.0);
    CHECK(relative(SurroundVirtualizer::pinnaCue(0.0, 30.0).notchFrequency) < -5.0);
    CHECK_THAT(relative(500.0), WithinAbs(0.0, 0.5));

    // Power-neutral over the audio band (white): the cue shapes the spectrum, not the level.
    for (const double elevation : {-60.0, 0.0, 30.0, 60.0, 90.0})
    {
        double power = 0.0;
        constexpr int kSteps = 4000;
        for (int i = 0; i < kSteps; ++i)
            power += std::pow(10.0, SurroundVirtualizer::pinnaGain(0.0, elevation, 20.0 + 19980.0 * (i + 0.5) / kSteps, kRate) / 10.0);
        INFO("elevation " << elevation);
        CHECK_THAT(power / kSteps, WithinRel(1.0, 0.03));
    }
}

TEST_CASE("Height virtualizer renders the top layer above the bed: Ltf, Rtf, Ltr, Rtr", "[surround][virtualizer][immersive]")
{
    const auto input = noise(24000, 3);

    SECTION("a top speaker has its own spectral cue, at the level of the bed speaker under it")
    {
        const auto layout = ChannelLayout::surround514(); // (Ltf right above L: both 30 deg to the left)
        const auto bed = renderFresh(layout, Speaker::left, input, 0.0);
        const auto top = renderFresh(layout, Speaker::topFrontLeft, input, 0.0);
        const double low = SurroundVirtualizer::pinnaCue(0.0, 0.0).notchFrequency;   // the bed's notch
        const double high = SurroundVirtualizer::pinnaCue(0.0, 30.0).notchFrequency; // the top's
        const double bedTilt = 10.0 * std::log10(bandEnergy(bed.left, low) / bandEnergy(bed.left, high));
        const double topTilt = 10.0 * std::log10(bandEnergy(top.left, low) / bandEnergy(top.left, high));
        INFO("bed " << bedTilt << " dB, top " << topTilt << " dB");
        CHECK(topTilt - bedTilt > 5.0); // (about 7 dB through band-passes as wide as the notches)
        CHECK(std::abs(10.0 * std::log10(energy(top.left, 4800) / energy(bed.left, 4800))) < 1.5);

        // Less lateral: a smaller time difference between the ears.
        const double bedLag = lagOf(bed.left, bed.right, 64) / kRate;
        const double topLag = lagOf(top.left, top.right, 64) / kRate;
        CHECK_THAT(bedLag, WithinAbs(woodworth(rad(30.0)), 0.00003));
        CHECK_THAT(topLag, WithinAbs(woodworth(std::asin(std::sin(rad(30.0)) * std::cos(rad(30.0)))), 0.00003));
        CHECK(topLag < bedLag - 0.00002);
    }

    SECTION("the top middles of 9.1.6: 60 degrees from the median plane, not 90")
    {
        const auto layout = ChannelLayout::surround916();
        const auto ears = renderFresh(layout, Speaker::topMiddleLeft, input, 0.0);
        CHECK_THAT(lagOf(ears.left, ears.right, 64) / kRate, WithinAbs(woodworth(rad(60.0)), 0.00003));
        CHECK(energy(ears.left) > 1.5 * energy(ears.right));
        const auto side = renderFresh(layout, Speaker::leftSurround, input, 0.0);
        CHECK(lagOf(side.left, side.right, 64) > lagOf(ears.left, ears.right, 64) + 4.0);
    }

    SECTION("7.1.4: all four tops are heard, mirrored left and right; the rear ones duller")
    {
        const auto layout = ChannelLayout::surround714();
        SurroundVirtualizer v;
        v.prepare(kRate);
        v.configure(layout, SurroundVirtualizer::Output::headphones, 0.0); // (the late reverberation is not mirrored)
        const auto ears = [&](Speaker speaker)
        {
            v.reset();
            return render(v, layout, speaker, input);
        };
        const auto ltf = ears(Speaker::topFrontLeft), rtf = ears(Speaker::topFrontRight);
        const auto ltr = ears(Speaker::topRearLeft), rtr = ears(Speaker::topRearRight);
        for (const auto* e : {&ltf, &rtf, &ltr, &rtr})
        {
            CHECK(energy(e->left) > 0.01 * energy(input));
            CHECK(energy(e->right) > 0.01 * energy(input));
        }
        CHECK_THAT(energy(ltf.left), WithinRel(energy(rtf.right), 1e-9));
        CHECK_THAT(energy(ltf.right), WithinRel(energy(rtf.left), 1e-9));
        CHECK_THAT(energy(ltr.left), WithinRel(energy(rtr.right), 1e-9));
        CHECK(energy(ltf.left) > energy(ltf.right));
        CHECK(energy(rtr.right) > energy(rtr.left));
        CHECK(highShare(ltr.left, 5000.0) < 0.85 * highShare(ltf.left, 5000.0)); // (behind: the pinna's shelf)
    }
}

TEST_CASE("Height virtualizer: the early reflections include the floor and the ceiling", "[surround][virtualizer][immersive]")
{
    const auto layout = ChannelLayout::surround514();
    std::vector<double> impulse(4800, 0.0);
    impulse[0] = 1.0;

    // When each path reaches the left ear (samples), from the geometry.
    const auto arrival = [](double azimuth, double elevation, std::size_t path)
    {
        const auto a = SurroundVirtualizer::arrivals(azimuth, elevation, 1.0)[path];
        const double theta = SurroundVirtualizer::incidence(a.azimuth, a.elevation, false);
        return 2.0 + ((a.distance - SurroundVirtualizer::kSpeakerDistance) / SurroundVirtualizer::kSpeedOfSound
                      + SurroundVirtualizer::earDelay(theta)) * kRate;
    };
    const auto around = [](const std::vector<double>& x, double at) // the energy within 4 samples
    { return energyBetween(x, static_cast<std::size_t>(at) - 3, static_cast<std::size_t>(at) + 5); };

    for (const auto& [speaker, elevation] : {std::pair{Speaker::topFrontLeft, 30.0}, std::pair{Speaker::left, 0.0}})
    {
        const auto wet = renderFresh(layout, speaker, impulse, 1.0);
        const auto dry = renderFresh(layout, speaker, impulse, 0.0);
        const double direct = around(wet.left, arrival(-30.0, elevation, 0));
        INFO((elevation > 0.0 ? "Ltf" : "L"));
        for (const std::size_t path : {std::size_t{1}, std::size_t{2}}) // the floor, the ceiling
        {
            const double at = arrival(-30.0, elevation, path);
            INFO("path " << path << " at " << at << " samples");
            CHECK(around(wet.left, at) > 0.01 * direct);
            CHECK(around(wet.left, at) > 100.0 * around(dry.left, at) + 1e-15);
        }
    }
    // The top speaker's ceiling reflection is the first to arrive; the bed speaker's floor one.
    CHECK(arrival(-30.0, 30.0, 2) < arrival(-30.0, 30.0, 1));
    CHECK(arrival(-30.0, 0.0, 1) < arrival(-30.0, 0.0, 2));
    for (const std::size_t wall : {std::size_t{3}, std::size_t{4}})
        CHECK(arrival(-30.0, 30.0, 2) < arrival(-30.0, 30.0, wall));
}

TEST_CASE("Height virtualizer: configuring and rendering 3D beds never touches the heap", "[surround][virtualizer][immersive][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();
    SurroundVirtualizer v;
    v.prepare(kRate);
    const auto input = noise(512, 9);
    std::vector<double> left(512), right(512);
    std::array<const double*, 16> pointers{};
    pointers.fill(input.data());

    const auto before = rt::getRealtimeViolationCount();
    {
        const rt::ScopedRealtimeSection section;
        double room = 0.0;
        for (const auto& layout : {ChannelLayout::surround714(), ChannelLayout::surround916(), ChannelLayout::surround514()})
            for (const auto output : {SurroundVirtualizer::Output::headphones, SurroundVirtualizer::Output::speakers})
            {
                v.configure(layout, output, room);
                v.process(pointers.data(), layout.numChannels, left.data(), right.data(), 512);
                room = room > 0.5 ? 0.0 : room + 0.4; // (reflections switched on and off)
            }
    }
    CHECK(rt::getRealtimeViolationCount() == before);
    CHECK(energy(left) > 0.0);
}

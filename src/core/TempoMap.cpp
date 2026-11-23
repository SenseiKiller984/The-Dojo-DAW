#include "core/TempoMap.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dojo::time
{
namespace
{
constexpr double kMinBpm = 1.0;
constexpr double kMaxBpm = 10'000.0;
constexpr int kCurveSteps = 128;
constexpr double kEpsilon = 1.0e-9;

double clampBpm(double bpm) noexcept { return std::isfinite(bpm) ? std::clamp(bpm, kMinBpm, kMaxBpm) : kDefaultTempoBpm; }

TimeSignature clampSignature(TimeSignature s) noexcept
{
    return {std::clamp(s.numerator, 1, 64), std::clamp(s.denominator, 1, 64)};
}
} // namespace

double tempoCurve(double x, double bend) noexcept
{
    x = std::clamp(x, 0.0, 1.0);
    const double exponent = std::exp2(3.0 * std::clamp(bend, -1.0, 1.0));
    return std::pow(x, exponent);
}

TempoMap::TempoMap()
    : TempoMap({TempoPoint{}}, {SignaturePoint{}})
{
}

TempoMap::TempoMap(std::vector<TempoPoint> points, std::vector<SignaturePoint> signatures)
{
    // --- Tempo points: sorted, from beat 0, one per position (the later one given wins) ---------
    for (auto& p : points)
    {
        p.beat = std::isfinite(p.beat) ? std::max(0.0, p.beat) : 0.0;
        p.bpm = clampBpm(p.bpm);
        p.bend = std::isfinite(p.bend) ? std::clamp(p.bend, -1.0, 1.0) : 0.0;
    }
    std::stable_sort(points.begin(), points.end(), [](const TempoPoint& a, const TempoPoint& b) { return a.beat < b.beat; });
    for (const auto& p : points)
    {
        if (!points_.empty() && std::abs(points_.back().beat - p.beat) < kEpsilon)
            points_.back() = p;
        else
            points_.push_back(p);
    }
    if (points_.empty())
        points_.push_back(TempoPoint{});
    if (points_.front().beat > 0.0)
        points_.insert(points_.begin(), TempoPoint{0.0, points_.front().bpm, TempoShape::jump, 0.0});

    // --- Segments: tempo linear in beats within each --------------------------------------------
    double seconds = 0.0;
    const auto add = [&](int point, double beat, double beats, double bpm0, double bpm1)
    {
        Segment s{beat, seconds, beats, bpm0, bpm1};
        if (std::isfinite(beats))
            seconds += secondsInto(s, beats);
        segments_.push_back(s);
        segmentPoint_.push_back(point);
    };
    for (std::size_t i = 0; i < points_.size(); ++i)
    {
        const auto& p = points_[i];
        const int index = static_cast<int>(i);
        if (i + 1 == points_.size())
        {
            add(index, p.beat, std::numeric_limits<double>::infinity(), p.bpm, p.bpm);
            break;
        }
        const auto& next = points_[i + 1];
        const double span = next.beat - p.beat;
        switch (p.shape)
        {
            case TempoShape::jump:
                add(index, p.beat, span, p.bpm, p.bpm);
                break;
            case TempoShape::linear:
                add(index, p.beat, span, p.bpm, next.bpm);
                break;
            case TempoShape::curve:
            {
                // Sub-ramps at even steps of the beats and at even steps of the tempo (where the curve
                // is steep, as x^e is at 0 for e < 1 and at 1 for e > 1), merged.
                const double exponent = std::exp2(3.0 * p.bend);
                std::vector<double> xs;
                xs.reserve(2 * kCurveSteps + 1);
                for (int k = 0; k <= kCurveSteps; ++k)
                {
                    const double u = static_cast<double>(k) / kCurveSteps;
                    xs.push_back(u);
                    xs.push_back(std::pow(u, 1.0 / exponent));
                }
                std::sort(xs.begin(), xs.end());
                xs.erase(std::unique(xs.begin(), xs.end(), [](double a, double b) { return b - a < 1.0e-12; }), xs.end());
                for (std::size_t k = 0; k + 1 < xs.size(); ++k)
                    add(index, p.beat + span * xs[k], span * (xs[k + 1] - xs[k]), p.bpm + (next.bpm - p.bpm) * tempoCurve(xs[k], p.bend),
                        p.bpm + (next.bpm - p.bpm) * tempoCurve(xs[k + 1], p.bend));
                break;
            }
        }
    }

    // --- Signatures: sorted bars from bar 0 -------------------------------------------------------
    for (auto& s : signatures)
    {
        s.bar = std::max<std::int64_t>(0, s.bar);
        s.signature = clampSignature(s.signature);
    }
    std::stable_sort(signatures.begin(), signatures.end(), [](const SignaturePoint& a, const SignaturePoint& b) { return a.bar < b.bar; });
    for (const auto& s : signatures)
    {
        if (!signatures_.empty() && signatures_.back().bar == s.bar)
            signatures_.back() = s;
        else
            signatures_.push_back(s);
    }
    if (signatures_.empty() || signatures_.front().bar > 0)
        signatures_.insert(signatures_.begin(), SignaturePoint{});

    double beat = 0.0;
    for (std::size_t k = 0; k < signatures_.size(); ++k)
    {
        const auto& s = signatures_[k];
        if (k > 0)
            beat += static_cast<double>(s.bar - runs_.back().bar) * runs_.back().length;
        runs_.push_back(BarRun{s.bar, beat, static_cast<double>(s.signature.numerator) * beatLengthInQuarterNotes(s.signature), s.signature});
    }
}

bool TempoMap::isConstant() const noexcept
{
    return points_.size() == 1 && runs_.size() == 1 && runs_.front().signature.numerator == 4 && runs_.front().signature.denominator == 4;
}

// --- Time ----------------------------------------------------------------------------------------------

double TempoMap::secondsInto(const Segment& s, double beats) noexcept
{
    const double change = s.bpm1 - s.bpm0;
    if (!std::isfinite(s.beats) || std::abs(change) < 1.0e-12)
        return beats * 60.0 / s.bpm0;
    const double slope = change / s.beats; // bpm per beat
    return 60.0 / slope * std::log1p(slope * beats / s.bpm0);
}

double TempoMap::beatsInto(const Segment& s, double seconds) noexcept
{
    const double change = s.bpm1 - s.bpm0;
    if (!std::isfinite(s.beats) || std::abs(change) < 1.0e-12)
        return seconds * s.bpm0 / 60.0;
    const double slope = change / s.beats;
    return s.bpm0 / slope * std::expm1(slope * seconds / 60.0);
}

std::size_t TempoMap::segmentAtBeat(double beat) const noexcept
{
    const auto it = std::upper_bound(segments_.begin(), segments_.end(), beat, [](double b, const Segment& s) { return b < s.beat; });
    return it == segments_.begin() ? 0 : static_cast<std::size_t>(it - segments_.begin()) - 1;
}

std::size_t TempoMap::segmentAtSeconds(double seconds) const noexcept
{
    const auto it =
        std::upper_bound(segments_.begin(), segments_.end(), seconds, [](double t, const Segment& s) { return t < s.seconds; });
    return it == segments_.begin() ? 0 : static_cast<std::size_t>(it - segments_.begin()) - 1;
}

double TempoMap::secondsAt(double beat) const noexcept
{
    const auto& first = segments_.front();
    if (beat <= 0.0)
        return beat * 60.0 / first.bpm0;
    const auto& s = segments_[segmentAtBeat(beat)];
    return s.seconds + secondsInto(s, std::isfinite(s.beats) ? std::min(beat - s.beat, s.beats) : beat - s.beat);
}

double TempoMap::beatAt(double seconds) const noexcept
{
    const auto& first = segments_.front();
    if (seconds <= 0.0)
        return seconds * first.bpm0 / 60.0;
    const auto& s = segments_[segmentAtSeconds(seconds)];
    const double beats = beatsInto(s, seconds - s.seconds);
    return s.beat + (std::isfinite(s.beats) ? std::min(beats, s.beats) : beats);
}

double TempoMap::bpmAtBeat(double beat) const noexcept
{
    if (beat <= 0.0)
        return segments_.front().bpm0;
    const auto& s = segments_[segmentAtBeat(beat)];
    if (!std::isfinite(s.beats) || s.beats <= 0.0)
        return s.bpm0;
    return s.bpm0 + (s.bpm1 - s.bpm0) * std::clamp((beat - s.beat) / s.beats, 0.0, 1.0);
}

int TempoMap::pointIndexAt(double beat) const noexcept
{
    const auto it =
        std::upper_bound(points_.begin(), points_.end(), beat + kEpsilon, [](double b, const TempoPoint& p) { return b < p.beat; });
    return it == points_.begin() ? 0 : static_cast<int>(it - points_.begin()) - 1;
}

// --- Bars ----------------------------------------------------------------------------------------------

std::size_t TempoMap::runAtBeat(double beat) const noexcept
{
    const auto it =
        std::upper_bound(runs_.begin(), runs_.end(), beat + kEpsilon, [](double b, const BarRun& r) { return b < r.beat; });
    return it == runs_.begin() ? 0 : static_cast<std::size_t>(it - runs_.begin()) - 1;
}

std::size_t TempoMap::runOfBar(std::int64_t bar) const noexcept
{
    const auto it = std::upper_bound(runs_.begin(), runs_.end(), bar, [](std::int64_t b, const BarRun& r) { return b < r.bar; });
    return it == runs_.begin() ? 0 : static_cast<std::size_t>(it - runs_.begin()) - 1;
}

TimeSignature TempoMap::signatureAt(double beat) const noexcept { return runs_[runAtBeat(beat)].signature; }

TimeSignature TempoMap::signatureOfBar(std::int64_t bar) const noexcept { return runs_[runOfBar(bar)].signature; }

std::int64_t TempoMap::barAt(double beat) const noexcept
{
    const auto& r = runs_[runAtBeat(beat)];
    return r.bar + static_cast<std::int64_t>(std::floor((beat - r.beat) / r.length + kEpsilon));
}

double TempoMap::beatOfBar(std::int64_t bar) const noexcept
{
    const auto& r = runs_[runOfBar(bar)];
    return r.beat + static_cast<double>(bar - r.bar) * r.length;
}

double TempoMap::barLength(std::int64_t bar) const noexcept { return runs_[runOfBar(bar)].length; }

BarsBeatsTicks TempoMap::toBarsBeatsTicks(double beat) const noexcept
{
    const auto bar = barAt(beat);
    const auto signature = signatureOfBar(bar);
    const double unit = beatLengthInQuarterNotes(signature);
    const double into = std::max(0.0, beat - beatOfBar(bar)) / unit;
    const auto whole = static_cast<int>(std::floor(into + kEpsilon));
    const auto ticksPerBeat = std::max(1, static_cast<int>(std::lround(kTicksPerQuarterNote * unit)));

    BarsBeatsTicks result;
    result.bar = bar + 1;
    result.beat = std::clamp(whole, 0, signature.numerator - 1) + 1;
    const double fraction = std::max(0.0, into - static_cast<double>(whole));
    result.tick = std::clamp(static_cast<int>(std::floor(fraction * ticksPerBeat + kEpsilon)), 0, ticksPerBeat - 1);
    return result;
}

double TempoMap::fromBarsBeatsTicks(BarsBeatsTicks position) const noexcept
{
    const auto bar = position.bar - 1;
    const auto signature = signatureOfBar(bar);
    const double unit = beatLengthInQuarterNotes(signature);
    const double ticksPerBeat = std::max(1.0, std::round(kTicksPerQuarterNote * unit));
    return beatOfBar(bar) + static_cast<double>(position.beat - 1) * unit + static_cast<double>(position.tick) / ticksPerBeat * unit;
}

} // namespace dojo::time

#pragma once

#include <cstddef>

namespace dojo
{

/**
    A warp marker: the moment `source` (seconds into the clip's file) plays `clipTime` seconds after
    the clip's start.

    A clip's warp map runs through its start (clip time 0 plays its source offset) and its markers
    (in order, both times increasing), and goes on past the last marker at the clip's own stretch
    (`stretch` timeline seconds per source second). Between two points the source plays at the
    rate that joins them. With no markers the map is the plain stretch: source = offset +
    clipTime / stretch.

    Pure and allocation-free: the session (editing) and the audio thread (playing) share it.
*/
struct WarpPoint
{
    double clipTime = 0.0;
    double source = 0.0;
};

namespace warp
{

/** The fastest a warped stretch may play its source (source seconds per clip second): as fast as
    the shortest stretch (0.25) plays it. */
inline constexpr double kMaxRate = 4.0;

/** The source time (seconds) that plays at `clipTime`. */
[[nodiscard]] inline double sourceAt(const WarpPoint* points, std::size_t count, double offset, double stretch, double clipTime) noexcept
{
    const double rate = stretch > 0.0 ? 1.0 / stretch : 1.0;
    if (count == 0 || clipTime <= 0.0)
        return offset + clipTime * rate;
    double t0 = 0.0, s0 = offset;
    for (std::size_t i = 0; i < count; ++i)
    {
        const double t1 = points[i].clipTime, s1 = points[i].source;
        if (clipTime <= t1)
            return t1 > t0 ? s0 + (clipTime - t0) * (s1 - s0) / (t1 - t0) : s1;
        t0 = t1;
        s0 = s1;
    }
    return s0 + (clipTime - t0) * rate;
}

/** The clip time at which `source` plays (the map's inverse). */
[[nodiscard]] inline double clipTimeOf(const WarpPoint* points, std::size_t count, double offset, double stretch, double source) noexcept
{
    const double safeStretch = stretch > 0.0 ? stretch : 1.0;
    if (count == 0 || source <= offset)
        return (source - offset) * safeStretch;
    double t0 = 0.0, s0 = offset;
    for (std::size_t i = 0; i < count; ++i)
    {
        const double t1 = points[i].clipTime, s1 = points[i].source;
        if (source <= s1)
            return s1 > s0 ? t0 + (source - s0) * (t1 - t0) / (s1 - s0) : t1;
        t0 = t1;
        s0 = s1;
    }
    return t0 + (source - s0) * safeStretch;
}

/** Source seconds per clip second at `clipTime` (the speed the source plays at there). */
[[nodiscard]] inline double rateAt(const WarpPoint* points, std::size_t count, double offset, double stretch, double clipTime) noexcept
{
    const double rate = stretch > 0.0 ? 1.0 / stretch : 1.0;
    if (count == 0 || clipTime < 0.0)
        return rate;
    double t0 = 0.0, s0 = offset;
    for (std::size_t i = 0; i < count; ++i)
    {
        const double t1 = points[i].clipTime, s1 = points[i].source;
        if (clipTime < t1)
            return t1 > t0 ? (s1 - s0) / (t1 - t0) : rate;
        t0 = t1;
        s0 = s1;
    }
    return rate;
}

} // namespace warp
} // namespace dojo

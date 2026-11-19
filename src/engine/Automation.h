#pragma once

#include <vector>

namespace dojo::engine
{

/** One automation breakpoint. `value` is normalised to [0, 1]. */
struct AutomationPoint
{
    double time = 0.0;
    double value = 0.0;
};

/**
    A spline automation curve.

    Points are joined with a monotone cubic Hermite spline (Fritsch–Carlson): the curve is smooth
    (C1) through every point but never overshoots between them, so a fade written as a ramp never
    dips below its start or rises above its end. Before the first point the curve holds the first
    value; after the last it holds the last.

    Build on the message thread (setPoints() allocates); evaluate anywhere — valueAt() is real-time
    safe (binary search, no allocation).
*/
class AutomationCurve
{
public:
    AutomationCurve() = default;
    explicit AutomationCurve(std::vector<AutomationPoint> points) { setPoints(std::move(points)); }

    /** Replaces the points (sorted by time here) and precomputes the spline tangents. */
    void setPoints(std::vector<AutomationPoint> points);

    [[nodiscard]] bool isEmpty() const noexcept { return points_.empty(); }
    [[nodiscard]] const std::vector<AutomationPoint>& getPoints() const noexcept { return points_; }

    /** Curve value at `seconds`, in [0, 1]. Returns `fallback` if the curve has no points. */
    [[nodiscard]] double valueAt(double seconds, double fallback = 0.0) const noexcept;

private:
    std::vector<AutomationPoint> points_;
    std::vector<double> tangents_;
};

} // namespace dojo::engine

#include "engine/Automation.h"

#include "engine/RenderGraph.h"

#include <algorithm>
#include <cmath>

namespace dojo::engine
{

void AutomationCurve::setPoints(std::vector<AutomationPoint> points)
{
    std::stable_sort(points.begin(), points.end(), [](const auto& a, const auto& b) { return a.time < b.time; });
    for (auto& p : points)
        p.value = std::clamp(p.value, 0.0, 1.0);

    points_ = std::move(points);
    const std::size_t n = points_.size();
    tangents_.assign(n, 0.0);

    if (n < 2)
        return;

    // Secant slopes.
    std::vector<double> secants(n - 1, 0.0);
    for (std::size_t i = 0; i + 1 < n; ++i)
    {
        const double dt = points_[i + 1].time - points_[i].time;
        secants[i] = dt > 0.0 ? (points_[i + 1].value - points_[i].value) / dt : 0.0;
    }

    // Initial tangents: average of neighbouring secants; one-sided at the ends.
    tangents_[0] = secants[0];
    tangents_[n - 1] = secants[n - 2];
    for (std::size_t i = 1; i + 1 < n; ++i)
        tangents_[i] = (secants[i - 1] * secants[i] <= 0.0) ? 0.0 : 0.5 * (secants[i - 1] + secants[i]);

    // Fritsch–Carlson: limit tangents so each segment stays monotone.
    for (std::size_t i = 0; i + 1 < n; ++i)
    {
        if (std::abs(secants[i]) < 1.0e-15)
        {
            tangents_[i] = 0.0;
            tangents_[i + 1] = 0.0;
            continue;
        }

        const double a = tangents_[i] / secants[i];
        const double b = tangents_[i + 1] / secants[i];
        const double magnitude = a * a + b * b;

        if (magnitude > 9.0)
        {
            const double tau = 3.0 / std::sqrt(magnitude);
            tangents_[i] = tau * a * secants[i];
            tangents_[i + 1] = tau * b * secants[i];
        }
    }
}

double AutomationCurve::valueAt(double seconds, double fallback) const noexcept
{
    const std::size_t n = points_.size();
    if (n == 0)
        return fallback;

    if (seconds <= points_.front().time)
        return points_.front().value;
    if (seconds >= points_.back().time)
        return points_.back().value;

    const auto upper = std::upper_bound(points_.begin(), points_.end(), seconds,
                                        [](double t, const AutomationPoint& p) { return t < p.time; });
    const auto i = static_cast<std::size_t>(std::distance(points_.begin(), upper)) - 1;

    const auto& p0 = points_[i];
    const auto& p1 = points_[i + 1];
    const double h = p1.time - p0.time;
    if (h <= 0.0)
        return p1.value;

    const double t = (seconds - p0.time) / h;
    const double t2 = t * t;
    const double t3 = t2 * t;

    const double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const double h10 = t3 - 2.0 * t2 + t;
    const double h01 = -2.0 * t3 + 3.0 * t2;
    const double h11 = t3 - t2;

    const double value = h00 * p0.value + h10 * h * tangents_[i] + h01 * p1.value + h11 * h * tangents_[i + 1];
    return std::clamp(value, 0.0, 1.0);
}

double automationToGain(double normalised) noexcept
{
    if (normalised <= 1.0e-4)
        return 0.0;
    const double db = -60.0 + 66.0 * std::clamp(normalised, 0.0, 1.0);
    return std::pow(10.0, db / 20.0);
}

double gainToAutomation(double linearGain) noexcept
{
    if (linearGain <= 0.001) // -60 dB
        return 0.0;
    const double db = 20.0 * std::log10(linearGain);
    return std::clamp((db + 60.0) / 66.0, 0.0, 1.0);
}

} // namespace dojo::engine

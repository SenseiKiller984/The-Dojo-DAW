#pragma once

#include <algorithm>
#include <atomic>

namespace dojo::devices
{

/** Stores the larger of `value` and what is there (for meters read on the message thread). */
inline void storeMax(std::atomic<float>& target, float value) noexcept
{
    float previous = target.load(std::memory_order_relaxed);
    while (value > previous && !target.compare_exchange_weak(previous, value, std::memory_order_relaxed))
    {
    }
}

/** Over-threshold amount with a soft knee of `knee` dB: 0 below, `over` well above, quadratic between. */
inline double softOver(double over, double knee) noexcept
{
    if (knee <= 0.0)
        return std::max(0.0, over);
    if (over <= -0.5 * knee)
        return 0.0;
    if (over >= 0.5 * knee)
        return over;
    const double t = over + 0.5 * knee;
    return t * t / (2.0 * knee);
}

/** The slope of softOver(). */
inline double softOverSlope(double over, double knee) noexcept
{
    if (knee <= 0.0)
        return over > 0.0 ? 1.0 : 0.0;
    if (over <= -0.5 * knee)
        return 0.0;
    if (over >= 0.5 * knee)
        return 1.0;
    return (over + 0.5 * knee) / knee;
}

} // namespace dojo::devices

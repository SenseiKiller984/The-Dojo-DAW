#pragma once

#include <atomic>

namespace dojo::rt
{

/** Lock-free "raise to maximum": stores `value` into `target` only if it is larger than the value
    currently held. Used by the audio thread to accumulate peak levels that the UI later collects
    with exchange(0) — so a transient peak is never missed between two UI frames.

    The CAS loop is lock-free: it only retries if another thread changed the value in the meantime,
    and with a single writer (the audio thread) racing a single exchanger (the UI) it retries at most
    a handful of times. */
template <typename T>
inline void atomicStoreMax(std::atomic<T>& target, T value) noexcept
{
    static_assert(std::atomic<T>::is_always_lock_free, "atomicStoreMax requires a lock-free atomic type");

    auto current = target.load(std::memory_order_relaxed);

    while (current < value && !target.compare_exchange_weak(current, value, std::memory_order_release,
                                                            std::memory_order_relaxed))
    {
    }
}

} // namespace dojo::rt

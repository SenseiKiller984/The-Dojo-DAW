#pragma once

#include <cstdint>

namespace dojo::rt
{

/**
    Marks the current thread as executing real-time-critical code for the lifetime of the object.

    The audio callback opens one of these on entry. While any section is open on a thread, the
    real-time allocation trap (see AllocationTrap.cpp, linked into debug/dev builds and the test
    suite) records every heap allocation or deallocation made by that thread as a violation.

    Sections nest and are strictly per-thread; constructing one costs a single thread-local
    increment and is itself real-time safe.
*/
class ScopedRealtimeSection final
{
public:
    ScopedRealtimeSection() noexcept;
    ~ScopedRealtimeSection() noexcept;

    ScopedRealtimeSection(const ScopedRealtimeSection&) = delete;
    ScopedRealtimeSection& operator=(const ScopedRealtimeSection&) = delete;
    ScopedRealtimeSection(ScopedRealtimeSection&&) = delete;
    ScopedRealtimeSection& operator=(ScopedRealtimeSection&&) = delete;
};

/** True if the calling thread is currently inside a ScopedRealtimeSection. */
[[nodiscard]] bool isInRealtimeSection() noexcept;

/** Records a real-time violation (heap traffic, blocking call ...). Lock-free; callable anywhere. */
void reportRealtimeViolation() noexcept;

/** Total number of violations recorded since process start (all threads). */
[[nodiscard]] std::uint64_t getRealtimeViolationCount() noexcept;

/** True if the allocation trap has been linked into this executable, i.e. whether violation counts
    reflect heap activity at all. */
[[nodiscard]] bool isAllocationTrapInstalled() noexcept;

namespace detail
{
/** Called once by the allocation trap's static initialiser. */
void markAllocationTrapInstalled() noexcept;
} // namespace detail

} // namespace dojo::rt

#include "core/rt/RealtimeGuard.h"

#include <atomic>

namespace dojo::rt
{
namespace
{
// Constant-initialised thread_local: lives in static TLS, needs no guard variable and no dynamic
// initialisation, so it is safe to touch from inside operator new on any thread at any time.
thread_local int realtimeSectionDepth = 0;

std::atomic<std::uint64_t> violationCount{0};
std::atomic<bool> allocationTrapInstalled{false};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
} // namespace

ScopedRealtimeSection::ScopedRealtimeSection() noexcept
{
    ++realtimeSectionDepth;
}

ScopedRealtimeSection::~ScopedRealtimeSection() noexcept
{
    --realtimeSectionDepth;
}

bool isInRealtimeSection() noexcept
{
    return realtimeSectionDepth > 0;
}

void reportRealtimeViolation() noexcept
{
    violationCount.fetch_add(1, std::memory_order_relaxed);
}

std::uint64_t getRealtimeViolationCount() noexcept
{
    return violationCount.load(std::memory_order_relaxed);
}

bool isAllocationTrapInstalled() noexcept
{
    return allocationTrapInstalled.load(std::memory_order_relaxed);
}

namespace detail
{
void markAllocationTrapInstalled() noexcept
{
    allocationTrapInstalled.store(true, std::memory_order_relaxed);
}
} // namespace detail

} // namespace dojo::rt

#include "core/rt/RealtimeGuard.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>
#include <vector>

using namespace dojo::rt;

namespace
{
// Defeats optimisers that may elide a new/delete pair whose result is unused.
void* volatile sink = nullptr;
} // namespace

TEST_CASE("The allocation trap is linked into the test executable", "[rt][guard]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();
}

TEST_CASE("Realtime sections nest and are scoped", "[rt][guard]")
{
    REQUIRE_FALSE(isInRealtimeSection());
    {
        const ScopedRealtimeSection outer;
        REQUIRE(isInRealtimeSection());
        {
            const ScopedRealtimeSection inner;
            REQUIRE(isInRealtimeSection());
        }
        REQUIRE(isInRealtimeSection());
    }
    REQUIRE_FALSE(isInRealtimeSection());
}

TEST_CASE("Realtime sections are per-thread", "[rt][guard]")
{
    const ScopedRealtimeSection section;
    bool otherThreadIsRealtime = true;

    std::thread([&otherThreadIsRealtime] { otherThreadIsRealtime = isInRealtimeSection(); }).join();

    REQUIRE(isInRealtimeSection());
    REQUIRE_FALSE(otherThreadIsRealtime);
}

TEST_CASE("Heap activity inside a realtime section is counted as a violation", "[rt][guard]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    const auto before = getRealtimeViolationCount();
    {
        const ScopedRealtimeSection section;
        auto* p = new int(42); // allocation -> violation
        sink = p;
        delete p; // deallocation -> violation
    }
    REQUIRE(getRealtimeViolationCount() - before == 2);
}

TEST_CASE("Heap activity outside realtime sections is not counted", "[rt][guard]")
{
    const auto before = getRealtimeViolationCount();

    std::vector<int> v(1024, 7);
    auto p = std::make_unique<double[]>(256);
    sink = p.get();

    std::thread([] { sink = std::make_unique<int>(1).get(); }).join();

    REQUIRE(getRealtimeViolationCount() == before);
}

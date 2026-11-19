#include "core/rt/SpscQueue.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <thread>
#include <vector>

using dojo::rt::SpscQueue;

TEST_CASE("SpscQueue rounds capacity up to a power of two", "[rt][spsc]")
{
    CHECK(SpscQueue<int>(1).capacity() == 2);
    CHECK(SpscQueue<int>(5).capacity() == 8);
    CHECK(SpscQueue<int>(64).capacity() == 64);
    CHECK(SpscQueue<int>(1000).capacity() == 1024);
}

TEST_CASE("SpscQueue is FIFO and reports empty/full correctly", "[rt][spsc]")
{
    SpscQueue<int> queue(4);
    int value = -1;

    REQUIRE(queue.emptyApprox());
    REQUIRE_FALSE(queue.tryPop(value));
    REQUIRE(value == -1); // untouched on failure

    for (int i = 0; i < 4; ++i)
        REQUIRE(queue.tryPush(i));

    REQUIRE(queue.sizeApprox() == 4);
    REQUIRE_FALSE(queue.tryPush(99)); // full: every slot is usable, no sentinel slot

    for (int i = 0; i < 4; ++i)
    {
        REQUIRE(queue.tryPop(value));
        REQUIRE(value == i);
    }

    REQUIRE_FALSE(queue.tryPop(value));
    REQUIRE(queue.emptyApprox());
}

TEST_CASE("SpscQueue wraps around many times", "[rt][spsc]")
{
    SpscQueue<std::uint32_t> queue(8);
    std::uint32_t next = 0;
    std::uint32_t expected = 0;

    for (int round = 0; round < 10'000; ++round)
    {
        const int burst = 1 + (round % 8);

        for (int i = 0; i < burst; ++i)
            REQUIRE(queue.tryPush(next++));

        for (int i = 0; i < burst; ++i)
        {
            std::uint32_t value = 0;
            REQUIRE(queue.tryPop(value));
            REQUIRE(value == expected++);
        }
    }
}

TEST_CASE("SpscQueue::drain consumes in order and honours maxItems", "[rt][spsc]")
{
    SpscQueue<int> queue(16);
    for (int i = 0; i < 10; ++i)
        REQUIRE(queue.tryPush(i));

    std::vector<int> seen;
    seen.reserve(16);

    CHECK(queue.drain([&seen](const int& v) noexcept { seen.push_back(v); }, 4) == 4);
    CHECK(queue.drain([&seen](const int& v) noexcept { seen.push_back(v); }) == 6);
    CHECK(queue.drain([&seen](const int& v) noexcept { seen.push_back(v); }) == 0);

    REQUIRE(seen.size() == 10);
    for (int i = 0; i < 10; ++i)
        CHECK(seen[static_cast<std::size_t>(i)] == i);
}

TEST_CASE("SpscQueue transfers every element exactly once across threads", "[rt][spsc][threads]")
{
    // A deliberately tiny queue maximises contention on the full/empty edges. Run under
    // -DDOJO_SANITIZER=thread to have TSan verify the memory ordering as well.
    constexpr std::uint64_t kCount = 2'000'000;
    SpscQueue<std::uint64_t> queue(16);

    std::thread producer(
        [&queue]
        {
            for (std::uint64_t i = 0; i < kCount;)
                if (queue.tryPush(i))
                    ++i;
        });

    std::uint64_t expected = 0;
    std::uint64_t outOfOrder = 0;

    while (expected < kCount)
    {
        std::uint64_t value = 0;
        if (queue.tryPop(value))
        {
            outOfOrder += (value != expected) ? 1 : 0;
            ++expected;
        }
    }

    producer.join();

    REQUIRE(outOfOrder == 0);
    REQUIRE(queue.emptyApprox());
}

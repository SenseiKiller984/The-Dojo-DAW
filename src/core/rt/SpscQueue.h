#pragma once

#include "core/Types.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <limits>
#include <memory>
#include <type_traits>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324) // structure was padded due to alignment specifier (intentional)
#endif

namespace dojo::rt
{

/**
    Bounded, lock-free, wait-free single-producer / single-consumer ring buffer.

    This is the primitive behind every UI -> audio (and audio -> UI) message path in The Dojo.

    Real-time guarantees
    --------------------
    - All storage is allocated once, in the constructor. tryPush() and tryPop() never allocate,
      never lock, never block and never make system calls. Each completes in a bounded number of
      steps regardless of what the other thread is doing (wait-free).
    - T must be trivially copyable, so copying an element can never run user code, allocate or
      throw. Commands are therefore plain value types (or raw pointers to pre-allocated objects).

    Threading contract
    ------------------
    Exactly one thread may call the producer functions (tryPush) and exactly one thread may call
    the consumer functions (tryPop, drain) at any given time. The roles may be handed over to a
    different thread only if that hand-over is externally synchronised (happens-before).

    Implementation notes
    --------------------
    - Indices increase monotonically and are masked into a power-of-two buffer, so all `capacity()`
      slots are usable (no "one empty slot" sentinel) and wrap-around is handled by unsigned
      arithmetic.
    - The producer and consumer indices live on separate cache lines. Each side additionally keeps a
      private cached copy of the other side's index, so in the common case an operation touches only
      memory owned by the calling thread (no cache-line ping-pong).
    - Release/acquire ordering on the index stores/loads publishes the slot contents.
*/
template <typename T>
class SpscQueue final
{
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue elements must be trivially copyable so that pushing/popping can never "
                  "allocate, lock or throw on the audio thread");
    static_assert(std::is_default_constructible_v<T>, "SpscQueue elements must be default constructible");
    static_assert(std::atomic<std::size_t>::is_always_lock_free, "size_t atomics must be lock-free");

public:
    /** Creates a queue that can hold at least `minimumCapacity` elements (rounded up to a power of
        two). Allocates; call from a non-real-time thread. */
    explicit SpscQueue(std::size_t minimumCapacity)
        : capacity_(roundUpToPowerOfTwo(std::max<std::size_t>(minimumCapacity, 2)))
        , mask_(capacity_ - 1)
        , slots_(std::make_unique<T[]>(capacity_))
    {
    }

    SpscQueue(const SpscQueue&) = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&) = delete;
    SpscQueue& operator=(SpscQueue&&) = delete;

    //==============================================================================================
    // Producer side

    /** Appends a copy of `value`. Returns false (and leaves the queue unchanged) if it is full. */
    [[nodiscard]] bool tryPush(const T& value) noexcept
    {
        const auto tail = tail_.load(std::memory_order_relaxed);

        if (tail - producerCachedHead_ == capacity_)
        {
            producerCachedHead_ = head_.load(std::memory_order_acquire);

            if (tail - producerCachedHead_ == capacity_)
                return false;
        }

        slots_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    //==============================================================================================
    // Consumer side

    /** Moves the oldest element into `out`. Returns false (leaving `out` untouched) if empty. */
    [[nodiscard]] bool tryPop(T& out) noexcept
    {
        const auto head = head_.load(std::memory_order_relaxed);

        if (head == consumerCachedTail_)
        {
            consumerCachedTail_ = tail_.load(std::memory_order_acquire);

            if (head == consumerCachedTail_)
                return false;
        }

        out = slots_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    /** Pops up to `maxItems` elements, invoking `fn(const T&)` for each in FIFO order.
        Returns the number of elements consumed. `fn` must itself be real-time safe. */
    template <typename Fn>
    std::size_t drain(Fn&& fn, std::size_t maxItems = std::numeric_limits<std::size_t>::max()) noexcept
    {
        static_assert(std::is_nothrow_invocable_v<Fn&, const T&>, "drain() callbacks must be noexcept");

        std::size_t count = 0;
        T item{};

        while (count < maxItems && tryPop(item))
        {
            fn(static_cast<const T&>(item));
            ++count;
        }

        return count;
    }

    //==============================================================================================
    // Either side (results are snapshots and may be stale by the time they are used)

    [[nodiscard]] std::size_t sizeApprox() const noexcept
    {
        const auto head = head_.load(std::memory_order_acquire);
        const auto tail = tail_.load(std::memory_order_acquire);
        return static_cast<std::size_t>(tail - head);
    }

    [[nodiscard]] bool emptyApprox() const noexcept { return sizeApprox() == 0; }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    static constexpr std::size_t roundUpToPowerOfTwo(std::size_t value) noexcept
    {
        std::size_t result = 1;
        while (result < value)
            result <<= 1;
        return result;
    }

    // Producer-owned cache line: written only by the producer.
    alignas(kCacheLineSize) std::atomic<std::size_t> tail_{0};
    std::size_t producerCachedHead_ = 0;

    // Consumer-owned cache line: written only by the consumer.
    alignas(kCacheLineSize) std::atomic<std::size_t> head_{0};
    std::size_t consumerCachedTail_ = 0;

    // Immutable after construction; read by both sides.
    alignas(kCacheLineSize) const std::size_t capacity_;
    const std::size_t mask_;
    const std::unique_ptr<T[]> slots_;
};

} // namespace dojo::rt

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

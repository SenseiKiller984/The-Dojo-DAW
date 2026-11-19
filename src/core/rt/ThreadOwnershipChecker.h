#pragma once

#include <atomic>
#include <thread>

namespace dojo::rt
{

/**
    Debug aid that enforces a "single thread" role, e.g. "the one producer of this SPSC queue".

    The first thread that calls isOwnedByCallingThread() claims ownership; afterwards the call returns
    true only on that thread. Wrap the call in an assertion at the top of role-restricted functions.
    Lock-free; it can be called from the audio thread.
*/
class ThreadOwnershipChecker final
{
public:
    [[nodiscard]] bool isOwnedByCallingThread() noexcept
    {
        const auto self = std::this_thread::get_id();
        auto expected = std::thread::id{};

        if (owner_.compare_exchange_strong(expected, self, std::memory_order_acq_rel))
            return true; // first caller claims the role

        return expected == self;
    }

    /** Releases ownership so that another thread may claim the role (externally synchronised). */
    void reset() noexcept { owner_.store(std::thread::id{}, std::memory_order_release); }

private:
    std::atomic<std::thread::id> owner_{};
};

} // namespace dojo::rt

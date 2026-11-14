#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <semaphore>
#include <vector>

namespace dojo::engine
{

/**
    Real-time worker threads for processing independent tracks in parallel.

    run() is called by the audio thread with a batch of independent tasks (e.g. all tracks of one
    dependency level). It wakes up to N-1 workers, takes tasks itself as well (work-sharing through
    an atomic counter), and returns once every task is done. No locks and no allocation on the
    calling thread; waking a worker is a semaphore post (a non-blocking syscall), and the final join
    is a short spin on an atomic counter.

    Workers run at real-time priority where the OS allows it, and wrap each task in a
    ScopedRealtimeSection + ScopedNoDenormals, so the allocation trap covers them too.
*/
class WorkerPool final
{
public:
    using TaskFn = void (*)(void* context, int taskIndex) noexcept;

    /** `numWorkers` extra threads (0 = everything runs on the caller). */
    explicit WorkerPool(int numWorkers);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    /** Runs fn(context, i) for i in [0, numTasks), in parallel. Real-time safe. */
    void run(int numTasks, TaskFn fn, void* context) noexcept;

    [[nodiscard]] int getNumWorkers() const noexcept { return static_cast<int>(workers_.size()); }

    /** A sensible worker count for this machine: hardware threads - 1, at most 7. */
    static int defaultWorkerCount();

private:
    class Worker;
    void participate() noexcept;

    // The current job is one 64-bit word: task count in the high half, next unclaimed task in the
    // low half. Tasks are claimed with compare-and-swap on the whole word, so a worker that wakes
    // late can never claim a task index that belongs to a previous (or not yet published) job.
    static constexpr std::uint64_t pack(std::uint32_t count, std::uint32_t next) noexcept
    {
        return (static_cast<std::uint64_t>(count) << 32) | next;
    }

    std::atomic<TaskFn> fn_{nullptr};
    std::atomic<void*> context_{nullptr};
    std::atomic<std::uint64_t> job_{0};
    std::atomic<int> remaining_{0};
    std::atomic<bool> exiting_{false};
    std::counting_semaphore<1024> wake_{0};
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace dojo::engine

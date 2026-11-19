#include "engine/WorkerPool.h"

#include "core/rt/RealtimeGuard.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <thread>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define DOJO_CPU_RELAX() _mm_pause()
#else
#define DOJO_CPU_RELAX() std::this_thread::yield()
#endif

namespace dojo::engine
{

class WorkerPool::Worker final : public juce::Thread
{
public:
    Worker(WorkerPool& pool, int index)
        : juce::Thread("Dojo audio worker " + juce::String(index))
        , pool_(pool)
    {
        // Real-time priority when permitted (Linux needs RLIMIT_RTPRIO); otherwise the highest normal one.
        if (!startRealtimeThread(juce::Thread::RealtimeOptions{}.withPriority(8)))
            startThread(juce::Thread::Priority::highest);
    }

    void run() override
    {
        for (;;)
        {
            pool_.wake_.acquire();
            if (pool_.exiting_.load(std::memory_order_acquire))
                return;

            pool_.participate();
        }
    }

private:
    WorkerPool& pool_;
};

int WorkerPool::defaultWorkerCount()
{
    const auto hardware = static_cast<int>(std::thread::hardware_concurrency());
    return std::clamp(hardware - 1, 0, 7);
}

WorkerPool::WorkerPool(int numWorkers)
{
    for (int i = 0; i < numWorkers; ++i)
        workers_.push_back(std::make_unique<Worker>(*this, i + 1));
}

WorkerPool::~WorkerPool()
{
    exiting_.store(true, std::memory_order_release);
    wake_.release(static_cast<std::ptrdiff_t>(workers_.size()));

    for (auto& worker : workers_)
        worker->stopThread(2000);
}

void WorkerPool::participate() noexcept
{
    const rt::ScopedRealtimeSection realtimeSection;
    const juce::ScopedNoDenormals noDenormals;

    auto job = job_.load(std::memory_order_acquire);

    for (;;)
    {
        const auto count = static_cast<std::uint32_t>(job >> 32);
        const auto next = static_cast<std::uint32_t>(job & 0xffffffffu);
        if (next >= count)
            return;

        if (!job_.compare_exchange_weak(job, pack(count, next + 1), std::memory_order_acq_rel,
                                        std::memory_order_acquire))
            continue; // `job` now holds the fresh value; retry

        const auto fn = fn_.load(std::memory_order_acquire);
        fn(context_.load(std::memory_order_acquire), static_cast<int>(next));
        remaining_.fetch_sub(1, std::memory_order_acq_rel);
        job = job_.load(std::memory_order_acquire);
    }
}

void WorkerPool::run(int numTasks, TaskFn fn, void* context) noexcept
{
    if (numTasks <= 0)
        return;

    if (numTasks == 1 || workers_.empty())
    {
        for (int i = 0; i < numTasks; ++i)
            fn(context, i);
        return;
    }

    fn_.store(fn, std::memory_order_release);
    context_.store(context, std::memory_order_release);
    remaining_.store(numTasks, std::memory_order_release);
    job_.store(pack(static_cast<std::uint32_t>(numTasks), 0), std::memory_order_release); // publish last

    const int helpers = std::min(static_cast<int>(workers_.size()), numTasks - 1);
    wake_.release(helpers);

    participate();

    while (remaining_.load(std::memory_order_acquire) > 0)
        DOJO_CPU_RELAX();

    job_.store(0, std::memory_order_release); // late-waking workers find nothing to do
}

} // namespace dojo::engine

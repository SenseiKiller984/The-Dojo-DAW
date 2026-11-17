#include "engine/TrackTaps.h"

#include <algorithm>

namespace dojo::engine
{

const TrackTaps::Tap* TrackTaps::find(std::uint32_t trackId) const noexcept
{
    if (trackId == 0)
        return nullptr;
    for (const auto& tap : taps_)
        if (tap.trackId.load(std::memory_order_acquire) == trackId)
            return &tap;
    return nullptr;
}

TrackTaps::Tap* TrackTaps::find(std::uint32_t trackId) noexcept
{
    return const_cast<Tap*>(static_cast<const TrackTaps&>(*this).find(trackId)); // (the same search)
}

bool TrackTaps::watch(std::uint32_t trackId) noexcept
{
    if (trackId == 0)
        return false;
    if (auto* tap = find(trackId))
    {
        ++tap->watchers;
        return true;
    }
    for (auto& tap : taps_)
        if (tap.trackId.load(std::memory_order_relaxed) == 0)
        {
            for (auto& sample : tap.ring)
                sample.store(0.0f, std::memory_order_relaxed);
            tap.written.store(0, std::memory_order_relaxed);
            tap.watchers = 1;
            tap.trackId.store(trackId, std::memory_order_release);
            inUse_.fetch_add(1, std::memory_order_release);
            return true;
        }
    return false;
}

void TrackTaps::unwatch(std::uint32_t trackId) noexcept
{
    if (auto* tap = find(trackId); tap != nullptr && --tap->watchers <= 0)
    {
        tap->watchers = 0;
        tap->trackId.store(0, std::memory_order_release);
        inUse_.fetch_sub(1, std::memory_order_release);
    }
}

bool TrackTaps::isWatched(std::uint32_t trackId) const noexcept
{
    return find(trackId) != nullptr;
}

bool TrackTaps::copy(std::uint32_t trackId, float* destination, int count) const noexcept
{
    const auto* tap = find(trackId);
    if (tap == nullptr)
        return false;
    count = std::clamp(count, 0, kSize);
    const std::uint32_t end = tap->written.load(std::memory_order_acquire);
    for (int i = 0; i < count; ++i)
    {
        const std::uint32_t index = (end - static_cast<std::uint32_t>(count) + static_cast<std::uint32_t>(i)) & static_cast<std::uint32_t>(kSize - 1);
        destination[i] = tap->ring[index].load(std::memory_order_relaxed);
    }
    return true;
}

void TrackTaps::write(std::uint32_t trackId, const Sample* const* channels, int numChannels, int numSamples) noexcept
{
    if (inUse_.load(std::memory_order_acquire) == 0 || trackId == 0 || numChannels <= 0)
        return;
    for (auto& tap : taps_)
    {
        if (tap.trackId.load(std::memory_order_acquire) != trackId)
            continue;
        std::uint32_t at = tap.written.load(std::memory_order_relaxed);
        const double scale = 1.0 / static_cast<double>(numChannels);
        for (int i = 0; i < numSamples; ++i, ++at)
        {
            double sum = 0.0;
            for (int ch = 0; ch < numChannels; ++ch)
                sum += channels[ch][i];
            tap.ring[at & static_cast<std::uint32_t>(kSize - 1)].store(static_cast<float>(sum * scale), std::memory_order_relaxed);
        }
        tap.written.store(at, std::memory_order_release);
        return;
    }
}

} // namespace dojo::engine

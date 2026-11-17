#pragma once

#include "core/Types.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace dojo::engine
{

/**
    Taps of chosen tracks' sound, for spectrum displays (the EQ showing other tracks' spectra over
    its own, to see where they mask each other).

    The UI watches a track by its id; from then on the audio thread writes that track's post-fader
    output (summed to mono) into the tap's ring each block, and the UI reads the newest samples at
    its own pace. Lock-free on both sides. Nothing is written while no tap is in use; otherwise a
    track costs a few compares per block. Watches are counted (each watch() needs its unwatch()), so
    several displays can follow the same track.

    watch / unwatch / copy: the message thread. write: the audio thread (any worker; one track is
    only ever written by one worker at a time).
*/
class TrackTaps
{
public:
    static constexpr int kTaps = 16;
    static constexpr int kSize = 8192; ///< samples kept per tap (a power of two)

    TrackTaps() = default;
    TrackTaps(const TrackTaps&) = delete;
    TrackTaps& operator=(const TrackTaps&) = delete;

    /** Starts (or joins) watching a track. False when every tap is taken. */
    bool watch(std::uint32_t trackId) noexcept;
    /** Ends one watch of a track (its tap is freed with the last). */
    void unwatch(std::uint32_t trackId) noexcept;
    [[nodiscard]] bool isWatched(std::uint32_t trackId) const noexcept;
    /** The newest `count` samples (at most kSize) of a watched track, oldest first. False if not watched. */
    bool copy(std::uint32_t trackId, float* destination, int count) const noexcept;

    /** A track's output for this block (its first `numChannels` channels), if it is watched. */
    void write(std::uint32_t trackId, const Sample* const* channels, int numChannels, int numSamples) noexcept;

private:
    struct Tap
    {
        std::atomic<std::uint32_t> trackId{0};
        int watchers = 0; ///< (message thread only)
        std::atomic<std::uint32_t> written{0};
        std::array<std::atomic<float>, kSize> ring{};
    };

    [[nodiscard]] const Tap* find(std::uint32_t trackId) const noexcept;
    [[nodiscard]] Tap* find(std::uint32_t trackId) noexcept;

    std::array<Tap, kTaps> taps_;
    std::atomic<int> inUse_{0};
};

} // namespace dojo::engine

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace dojo::video::mf
{

/** A movie's picture copied (its compressed samples, not decoded) into an MP4 with a new sound. */
struct MuxRequest
{
    std::wstring video;
    std::wstring destination;
    double from = 0.0;               ///< seconds into the movie (its key frame there)
    double to = 0.0;                 ///< seconds into the movie where the result ends (> from)
    int sampleRate = 48000;          ///< 44100 or 48000 (what Media Foundation's AAC encoder takes)
    int channels = 2;                ///< 1, 2 or 6
    std::uint32_t aacBytesPerSecond = 24000; ///< 12000, 16000, 20000 or 24000 (96 to 192 kbps)
};

/** Fills `interleaved` with up to `frames` frames of 16-bit PCM; returns how many it filled (0: the end). */
using PullAudio = std::function<int(std::int16_t* interleaved, int frames)>;

/** Writes the MP4 with Media Foundation's sink writer (the picture passed through, the sound
    encoded to AAC). Kept free of JUCE (the Windows headers come first in its translation unit). */
bool mux(const MuxRequest& request, const PullAudio& pull, const std::function<bool(double)>& progress, std::string& error);

/** The time (seconds) of the key frame at or before `seconds`, where Media Foundation's reader
    lands when it seeks there; 0 if it cannot tell. */
double keyFrameAtOrBefore(const std::wstring& video, double seconds);

} // namespace dojo::video::mf

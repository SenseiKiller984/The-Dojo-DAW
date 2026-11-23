#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace dojo::test::mf
{

/** Writes an H.264 MP4 with Media Foundation's encoder: `frames` frames of width x height BGRA
    (top row first) painted by `paint`, a key frame at least every `keyFrameInterval` frames. False
    (with `error`) where Media Foundation or its H.264 encoder is not there. Windows only. */
bool writeH264Movie(const std::wstring& path, int frames, int fps, int keyFrameInterval, int width, int height,
                    const std::function<void(int frame, std::uint8_t* bgra)>& paint, std::string& error);

/** A movie's first sound, decoded by Media Foundation to interleaved float samples. */
bool readSound(const std::wstring& path, std::vector<float>& interleaved, int& channels, int& sampleRate, std::string& error);

} // namespace dojo::test::mf

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dojo::video::mf
{

/**
    Windows Media Foundation video reader (IMFSourceReader, frames converted to 32-bit BGRX by MF's
    video processor). Kept free of JUCE so that its translation unit can include the Windows headers
    first. One thread at a time; COM is initialised on the calling thread as needed.
*/
class Reader final
{
public:
    Reader();
    ~Reader();

    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    /** Opens `path` (UTF-16). Returns false and sets `error` if Media Foundation cannot read it. */
    bool open(const std::wstring& path, std::string& error);

    [[nodiscard]] int getWidth() const noexcept;
    [[nodiscard]] int getHeight() const noexcept;
    [[nodiscard]] double getFrameRate() const noexcept;
    [[nodiscard]] double getDuration() const noexcept;
    [[nodiscard]] std::string getCodec() const;

    void seek(double seconds);

    /** The next frame as width x height BGRA pixels (top row first, opaque) and its time in seconds.
        False at the end of the stream or on an error. */
    bool readNext(std::vector<std::uint8_t>& bgra, int& width, int& height, double& time);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dojo::video::mf

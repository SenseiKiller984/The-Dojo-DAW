#pragma once

// Reads BW64 / RIFF WAVE files back chunk by chunk (for the ADM tests).

#include "engine/io/Bw64Writer.h"

#include <juce_core/juce_core.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

namespace dojo::test::bw64
{

/** A BW64/RIFF file read back chunk by chunk. */
struct Chunk
{
    juce::String id;
    std::uint64_t size = 0;
    juce::MemoryBlock data;
    std::uint64_t offset = 0; ///< of its payload
};
struct ParsedFile
{
    juce::String header;
    std::uint32_t headerSize = 0;
    std::vector<Chunk> chunks;
    [[nodiscard]] const Chunk* find(const char* id) const
    {
        for (const auto& c : chunks)
            if (c.id == id)
                return &c;
        return nullptr;
    }
};

inline std::uint32_t u32(const juce::MemoryBlock& m, std::size_t at)
{
    const auto* b = static_cast<const std::uint8_t*>(m.getData()) + at;
    return static_cast<std::uint32_t>(b[0]) | (static_cast<std::uint32_t>(b[1]) << 8) | (static_cast<std::uint32_t>(b[2]) << 16)
           | (static_cast<std::uint32_t>(b[3]) << 24);
}
inline std::uint16_t u16(const juce::MemoryBlock& m, std::size_t at)
{
    const auto* b = static_cast<const std::uint8_t*>(m.getData()) + at;
    return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}
inline std::uint64_t u64(const juce::MemoryBlock& m, std::size_t at) { return u32(m, at) | (static_cast<std::uint64_t>(u32(m, at + 4)) << 32); }
inline juce::String text(const juce::MemoryBlock& m, std::size_t at, std::size_t length)
{
    const auto* start = static_cast<const char*>(m.getData()) + at;
    std::size_t size = 0;
    while (size < length && start[size] != 0)
        ++size; // (fixed-size fields are padded with zeros)
    return juce::String(start, size);
}

inline ParsedFile parse(const juce::File& file)
{
    juce::MemoryBlock all;
    REQUIRE(file.loadFileAsData(all));
    ParsedFile parsed;
    parsed.header = text(all, 0, 4);
    parsed.headerSize = u32(all, 4);
    REQUIRE(text(all, 8, 4) == "WAVE");
    std::uint64_t ds64Data = 0;
    std::size_t at = 12;
    while (at + 8 <= all.getSize())
    {
        Chunk chunk;
        chunk.id = text(all, at, 4);
        chunk.size = u32(all, at + 4);
        if (chunk.id == "data" && chunk.size == 0xFFFFFFFFu)
            chunk.size = ds64Data;
        chunk.offset = at + 8;
        REQUIRE(chunk.offset + chunk.size <= all.getSize());
        chunk.data.append(static_cast<const char*>(all.getData()) + chunk.offset, static_cast<std::size_t>(chunk.size));
        if (chunk.id == "ds64")
            ds64Data = u64(chunk.data, 8);
        at = static_cast<std::size_t>(chunk.offset + chunk.size + (chunk.size & 1u));
        parsed.chunks.push_back(std::move(chunk));
    }
    CHECK(at == all.getSize());
    return parsed;
}

inline std::vector<engine::adm::TrackEntry> chnaEntries(const Chunk& chna)
{
    std::vector<engine::adm::TrackEntry> entries;
    const int uids = u16(chna.data, 2);
    for (int i = 0; i < uids; ++i)
    {
        const auto at = static_cast<std::size_t>(4 + 40 * i);
        entries.push_back({u16(chna.data, at), text(chna.data, at + 2, 12), text(chna.data, at + 14, 14), text(chna.data, at + 28, 11)});
    }
    return entries;
}


} // namespace dojo::test::bw64

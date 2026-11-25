#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

/**
    Light compile-time string obfuscation: a few licensing strings are stored XOR-encoded in the
    binary and decoded at run time, so they are not plainly greppable. This is not encryption — it
    only raises the bar against casual inspection of the binary.

        const auto s = DOJO_OBF("secret");   // std::string, decoded at run time
*/
namespace dojo::licensing::obf
{

/** A deterministic keystream byte for position `i` (no secret; just a scrambler). */
constexpr std::uint8_t keyAt(std::size_t i) noexcept
{
    std::uint32_t x = 0x9e3779b9u ^ (static_cast<std::uint32_t>(i) * 2654435761u + 0x7f4a7c15u);
    x ^= x >> 15;
    x *= 0x2c1b3c6du;
    x ^= x >> 12;
    x *= 0x297a2d39u;
    x ^= x >> 15;
    return static_cast<std::uint8_t>(x);
}

template <std::size_t N>
struct Obfuscated
{
    std::array<char, N> bytes{};

    constexpr explicit Obfuscated(const char (&text)[N]) noexcept
    {
        for (std::size_t i = 0; i < N; ++i)
            bytes[i] = static_cast<char>(static_cast<std::uint8_t>(text[i]) ^ keyAt(i));
    }

    [[nodiscard]] std::string decode() const
    {
        std::string out(N > 0 ? N - 1 : 0, '\0');
        for (std::size_t i = 0; i + 1 < N; ++i)
            out[i] = static_cast<char>(static_cast<std::uint8_t>(bytes[i]) ^ keyAt(i));
        return out;
    }
};

} // namespace dojo::licensing::obf

// The encoded bytes live in a `static constexpr` (so the plaintext is not in the binary); decode at
// the point of use.
#define DOJO_OBF(str)                                                                                                  \
    ([] {                                                                                                              \
        static constexpr ::dojo::licensing::obf::Obfuscated<sizeof(str)> dojo_obf_value(str);                         \
        return dojo_obf_value.decode();                                                                               \
    }())

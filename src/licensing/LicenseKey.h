#pragma once

#include "licensing/Crypto.h"

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <optional>

/**
    The Dojo's product keys: Ed25519-signed, written in Crockford Base32.

        payload    4 bytes   version (4 bits) | tier (2 bits) | 0 (2 bits), serial (24 bits)
        signature 64 bytes   Ed25519 (RFC 8032) of "TheDojo/license/v1" + the payload
        check      2 bytes   CRC-16/CCITT of the 68 bytes above (catches typos before the signature)

    70 bytes = 560 bits = 112 Base32 characters, shown as "DOJO1-" and 14 groups of 8:

        DOJO1-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX-XXXXXXXX-...

    Only the holder of the private key can sign a payload; The Dojo holds only the public key.
    Reading is lenient (case, dashes, spaces and line breaks are ignored; O reads as 0, I and L as 1).
*/
namespace dojo::licensing
{

enum class Tier : std::uint8_t
{
    none = 0,
    demo = 1,
    basic = 2,
    pro = 3,
};

[[nodiscard]] juce::String getTierName(Tier tier);

inline constexpr const char* kKeyPrefix = "DOJO1";
inline constexpr int kKeyVersion = 1;
inline constexpr int kPayloadBytes = 4;
inline constexpr int kKeyCharacters = 112; ///< after the prefix
inline constexpr std::uint32_t kMaxSerial = 0xffffffu;

struct Payload
{
    Tier tier = Tier::none;
    std::uint32_t serial = 0; ///< 0 .. kMaxSerial: who the key was issued to (the issuer's records)

    [[nodiscard]] std::array<std::uint8_t, kPayloadBytes> toBytes() const noexcept;
    [[nodiscard]] static std::optional<Payload> fromBytes(const std::array<std::uint8_t, kPayloadBytes>& bytes) noexcept;
};

/** What a payload's signature signs (domain-separated, so no other signature can be replayed as a key). */
[[nodiscard]] std::array<std::uint8_t, 22> signedMessage(const Payload& payload) noexcept;

/** A key's text from a payload and its signature. */
[[nodiscard]] juce::String encodeKey(const Payload& payload, const crypto::Signature& signature);

/** Signs a payload into a key (tests and the key generator; never in the application). */
[[nodiscard]] juce::String makeKey(const Payload& payload, const crypto::Seed& privateKey);

struct ParsedKey
{
    Payload payload;
    crypto::Signature signature{};
};

/** Reads a key's text (no signature check). Nothing, and why in `error`, if it is not a key. */
[[nodiscard]] std::optional<ParsedKey> parseKey(const juce::String& text, juce::String& error);

/** Reads a key and checks its signature with `publicKey`. Nothing, and why in `error`, if it is not valid. */
[[nodiscard]] std::optional<Payload> verifyKey(const juce::String& text, const crypto::PublicKey& publicKey, juce::String& error);

/** The key's text normalised for storing and comparing (upper case, grouped as shown above). */
[[nodiscard]] juce::String normaliseKey(const juce::String& text);

// Crockford Base32 (exposed for tests)
[[nodiscard]] juce::String toBase32(const std::uint8_t* data, std::size_t size);
[[nodiscard]] bool fromBase32(const juce::String& text, std::uint8_t* out, std::size_t size);
[[nodiscard]] std::uint16_t crc16(const std::uint8_t* data, std::size_t size) noexcept;

} // namespace dojo::licensing

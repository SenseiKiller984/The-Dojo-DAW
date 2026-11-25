#include "licensing/LicenseKey.h"

#include <cstring>

namespace dojo::licensing
{
namespace
{
constexpr const char* kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
constexpr const char kDomain[] = "TheDojo/license/v1"; // 18 bytes
constexpr int kSignatureBytes = 64;
constexpr int kRawBytes = kPayloadBytes + kSignatureBytes; // 68
constexpr int kCheckedBytes = kRawBytes + 2;               // 70 = 560 bits = 112 characters
constexpr int kGroup = 8;

/** A Crockford character's value: O as 0, I and L as 1; -1 for anything else (U included). */
int valueOf(juce::juce_wchar c) noexcept
{
    c = juce::CharacterFunctions::toUpperCase(c);
    if (c == 'O')
        return 0;
    if (c == 'I' || c == 'L')
        return 1;
    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == static_cast<char>(c))
            return i;
    return -1;
}

/** Letters and digits only, upper case (separators and whitespace dropped). */
juce::String compact(const juce::String& text)
{
    juce::String out;
    out.preallocateBytes(static_cast<std::size_t>(text.length()));
    for (auto c : text)
        if (juce::CharacterFunctions::isLetterOrDigit(c))
            out += juce::CharacterFunctions::toUpperCase(c);
    return out;
}
} // namespace

juce::String getTierName(Tier tier)
{
    switch (tier)
    {
    case Tier::demo: return "Free Trial";
    case Tier::basic: return "Basic";
    case Tier::pro: return "Pro";
    case Tier::none: break;
    }
    return "No License";
}

std::array<std::uint8_t, kPayloadBytes> Payload::toBytes() const noexcept
{
    return {static_cast<std::uint8_t>((kKeyVersion << 4) | ((static_cast<int>(tier) & 3) << 2)),
            static_cast<std::uint8_t>((serial >> 16) & 0xff), static_cast<std::uint8_t>((serial >> 8) & 0xff),
            static_cast<std::uint8_t>(serial & 0xff)};
}

std::optional<Payload> Payload::fromBytes(const std::array<std::uint8_t, kPayloadBytes>& bytes) noexcept
{
    const int version = bytes[0] >> 4;
    const int tier = (bytes[0] >> 2) & 3;
    if (version != kKeyVersion || (bytes[0] & 3) != 0 || tier == 0)
        return std::nullopt;
    Payload payload;
    payload.tier = static_cast<Tier>(tier);
    payload.serial = (static_cast<std::uint32_t>(bytes[1]) << 16) | (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
    return payload;
}

std::array<std::uint8_t, 22> signedMessage(const Payload& payload) noexcept
{
    std::array<std::uint8_t, 22> message{};
    std::memcpy(message.data(), kDomain, 18);
    const auto bytes = payload.toBytes();
    std::memcpy(message.data() + 18, bytes.data(), bytes.size());
    return message;
}

std::uint16_t crc16(const std::uint8_t* data, std::size_t size) noexcept
{
    // CRC-16/CCITT-FALSE: polynomial 0x1021, initial 0xFFFF.
    std::uint16_t crc = 0xffff;
    for (std::size_t i = 0; i < size; ++i)
    {
        crc = static_cast<std::uint16_t>(crc ^ (static_cast<std::uint16_t>(data[i]) << 8));
        for (int bit = 0; bit < 8; ++bit)
            crc = static_cast<std::uint16_t>((crc & 0x8000) != 0 ? (crc << 1) ^ 0x1021 : crc << 1);
    }
    return crc;
}

juce::String toBase32(const std::uint8_t* data, std::size_t size)
{
    juce::String out;
    std::uint32_t buffer = 0;
    int bits = 0;
    for (std::size_t i = 0; i < size; ++i)
    {
        buffer = (buffer << 8) | data[i];
        bits += 8;
        while (bits >= 5)
        {
            bits -= 5;
            out += kAlphabet[(buffer >> bits) & 31];
        }
    }
    if (bits > 0)
        out += kAlphabet[(buffer << (5 - bits)) & 31];
    return out;
}

bool fromBase32(const juce::String& text, std::uint8_t* out, std::size_t size)
{
    if (static_cast<std::size_t>(text.length()) != (size * 8 + 4) / 5)
        return false;
    std::uint32_t buffer = 0;
    int bits = 0;
    std::size_t written = 0;
    for (auto c : text)
    {
        const int value = valueOf(c);
        if (value < 0)
            return false;
        buffer = (buffer << 5) | static_cast<std::uint32_t>(value);
        bits += 5;
        if (bits >= 8)
        {
            bits -= 8;
            if (written < size)
                out[written++] = static_cast<std::uint8_t>((buffer >> bits) & 0xff);
        }
        buffer &= (1u << bits) - 1u;
    }
    // The padding bits must be zero (one text per key).
    return written == size && buffer == 0;
}

juce::String encodeKey(const Payload& payload, const crypto::Signature& signature)
{
    std::array<std::uint8_t, kCheckedBytes> raw{};
    const auto bytes = payload.toBytes();
    std::memcpy(raw.data(), bytes.data(), bytes.size());
    std::memcpy(raw.data() + kPayloadBytes, signature.data(), signature.size());
    const auto crc = crc16(raw.data(), kRawBytes);
    raw[kRawBytes] = static_cast<std::uint8_t>(crc >> 8);
    raw[kRawBytes + 1] = static_cast<std::uint8_t>(crc & 0xff);

    const auto body = toBase32(raw.data(), raw.size());
    jassert(body.length() == kKeyCharacters);
    juce::String text = kKeyPrefix;
    for (int i = 0; i < body.length(); i += kGroup)
        text << "-" << body.substring(i, i + kGroup);
    return text;
}

juce::String makeKey(const Payload& payload, const crypto::Seed& privateKey)
{
    const auto message = signedMessage(payload);
    return encodeKey(payload, crypto::sign(privateKey, message));
}

std::optional<ParsedKey> parseKey(const juce::String& text, juce::String& error)
{
    auto body = compact(text);
    if (body.isEmpty())
    {
        error = "Enter a license key.";
        return std::nullopt;
    }
    if (!body.startsWith(kKeyPrefix))
    {
        error = "A license key for The Dojo starts with " + juce::String(kKeyPrefix) + "-.";
        return std::nullopt;
    }
    body = body.substring(juce::String(kKeyPrefix).length());
    if (body.length() != kKeyCharacters)
    {
        error = "A license key has " + juce::String(kKeyCharacters) + " characters after " + kKeyPrefix + "- (this one has " +
                juce::String(body.length()) + "). Paste the whole key.";
        return std::nullopt;
    }
    std::array<std::uint8_t, kCheckedBytes> raw{};
    if (!fromBase32(body, raw.data(), raw.size()))
    {
        error = "This license key has a character that cannot be in a key (U, or a symbol). Check it for typos.";
        return std::nullopt;
    }
    const auto crc = static_cast<std::uint16_t>((raw[kRawBytes] << 8) | raw[kRawBytes + 1]);
    if (crc != crc16(raw.data(), kRawBytes))
    {
        error = "This license key has a typo. Check it, or paste it again.";
        return std::nullopt;
    }
    std::array<std::uint8_t, kPayloadBytes> payloadBytes{};
    std::memcpy(payloadBytes.data(), raw.data(), payloadBytes.size());
    const auto payload = Payload::fromBytes(payloadBytes);
    if (!payload.has_value())
    {
        error = "This license key is for another version of The Dojo.";
        return std::nullopt;
    }
    ParsedKey parsed;
    parsed.payload = *payload;
    std::memcpy(parsed.signature.data(), raw.data() + kPayloadBytes, parsed.signature.size());
    return parsed;
}

std::optional<Payload> verifyKey(const juce::String& text, const crypto::PublicKey& publicKey, juce::String& error)
{
    const auto parsed = parseKey(text, error);
    if (!parsed.has_value())
        return std::nullopt;
    const auto message = signedMessage(parsed->payload);
    if (!crypto::verify(publicKey, message, parsed->signature))
    {
        error = "This license key is not valid.";
        return std::nullopt;
    }
    return parsed->payload;
}

juce::String normaliseKey(const juce::String& text)
{
    juce::String error;
    if (const auto parsed = parseKey(text, error))
        return encodeKey(parsed->payload, parsed->signature);
    return compact(text);
}

} // namespace dojo::licensing

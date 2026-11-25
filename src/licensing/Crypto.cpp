#include "licensing/Crypto.h"

#include <monocypher-ed25519.h>
#include <monocypher.h>

namespace dojo::licensing::crypto
{

bool verify(const PublicKey& publicKey, std::span<const std::uint8_t> message, const Signature& signature) noexcept
{
    return crypto_ed25519_check(signature.data(), publicKey.data(), message.data(), message.size()) == 0;
}

PublicKey publicKeyOf(const Seed& seed) noexcept
{
    std::array<std::uint8_t, 32> copy = seed; // (crypto_ed25519_key_pair wipes the seed it is given)
    std::array<std::uint8_t, 64> secret{};
    PublicKey publicKey{};
    crypto_ed25519_key_pair(secret.data(), publicKey.data(), copy.data());
    crypto_wipe(secret.data(), secret.size());
    return publicKey;
}

Signature sign(const Seed& seed, std::span<const std::uint8_t> message) noexcept
{
    std::array<std::uint8_t, 32> copy = seed;
    std::array<std::uint8_t, 64> secret{};
    PublicKey publicKey{};
    crypto_ed25519_key_pair(secret.data(), publicKey.data(), copy.data());
    Signature signature{};
    crypto_ed25519_sign(signature.data(), secret.data(), message.data(), message.size());
    crypto_wipe(secret.data(), secret.size());
    return signature;
}

void keyedHash(std::span<std::uint8_t> out, std::span<const std::uint8_t> key, std::span<const std::uint8_t> message) noexcept
{
    crypto_blake2b_keyed(out.data(), out.size(), key.data(), key.size(), message.data(), message.size());
}

bool equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept
{
    if (a.size() != b.size())
        return false;
    std::uint8_t difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference = static_cast<std::uint8_t>(difference | (a[i] ^ b[i]));
    return difference == 0;
}

} // namespace dojo::licensing::crypto

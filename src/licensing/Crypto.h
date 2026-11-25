#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

/**
    The cryptography licensing uses, over Monocypher (third_party/monocypher): Ed25519 signatures
    (RFC 8032, SHA-512) and BLAKE2b keyed hashes. Nothing here allocates or keeps secrets around:
    secret material is wiped after use.
*/
namespace dojo::licensing::crypto
{

using PublicKey = std::array<std::uint8_t, 32>;
using Seed = std::array<std::uint8_t, 32>;     ///< an Ed25519 private key (RFC 8032's 32-byte secret)
using Signature = std::array<std::uint8_t, 64>;

/** True if `signature` is `publicKey`'s Ed25519 signature of `message` (strict: S < L). */
[[nodiscard]] bool verify(const PublicKey& publicKey, std::span<const std::uint8_t> message, const Signature& signature) noexcept;

/** The public key of a private key (seed). */
[[nodiscard]] PublicKey publicKeyOf(const Seed& seed) noexcept;

/** Signs `message` with a private key (seed): for tests and key generation, never in the application. */
[[nodiscard]] Signature sign(const Seed& seed, std::span<const std::uint8_t> message) noexcept;

/** BLAKE2b of `message` keyed with `key` (up to 64 bytes), `size` bytes long (1..64). */
void keyedHash(std::span<std::uint8_t> out, std::span<const std::uint8_t> key, std::span<const std::uint8_t> message) noexcept;

/** Constant-time comparison. */
[[nodiscard]] bool equal(std::span<const std::uint8_t> a, std::span<const std::uint8_t> b) noexcept;

} // namespace dojo::licensing::crypto

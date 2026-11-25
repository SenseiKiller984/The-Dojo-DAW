#pragma once

#include "licensing/Crypto.h"
#include "licensing/LicenseKey.h"

/**
    The production public key (the private half never leaves the key issuer's computer) and the
    keys taken back.

    To change key pairs: run the private key generator's "init" (or "pubkey"), paste the two arrays
    it prints into PublicKey.cpp, and replace the free-trial key in the README. Keys signed with the
    old private key stop working.
*/
namespace dojo::licensing
{

[[nodiscard]] crypto::PublicKey getProductionPublicKey() noexcept;

/** Keys that no longer work (leaked or refunded): their tier and serial. */
[[nodiscard]] bool isRevoked(const Payload& payload) noexcept;

} // namespace dojo::licensing

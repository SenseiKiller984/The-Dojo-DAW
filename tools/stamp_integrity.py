#!/usr/bin/env python3
"""
Stamp The Dojo's executable with the keyed BLAKE2b hash of its own bytes, for the run-time tamper
check (src/licensing/Integrity.cpp). Run as a post-build step on the final executable.

    python stamp_integrity.py <path-to-executable>

Layout (must match Integrity.cpp): a 16-byte marker followed by 32 bytes that start as zero and are
replaced here with blake2b-256(key, file-with-those-32-bytes-zeroed). The key is
blake2b-256(key=seed, seed), seed being the fixed string below (kept in sync with the C++).
"""

import hashlib
import sys

# Must equal dojo_integrity_block's first 16 bytes in src/licensing/Integrity.cpp.
MARKER = bytes([0x4b, 0x9d, 0x2e, 0xf1, 0x5a, 0x83, 0xc7, 0x60,
                0x1d, 0xb4, 0x39, 0xee, 0x72, 0x08, 0xa6, 0xcf])
HASH_LEN = 32
# Must equal the DOJO_OBF("...") seed string in Integrity.cpp (hashKey()).
SEED = b"TheDojo/integrity/v1/keystream-material-xyzzy"


def hash_key():
    return hashlib.blake2b(SEED, digest_size=HASH_LEN, key=SEED).digest()


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: stamp_integrity.py <executable>")
    path = sys.argv[1]
    with open(path, "rb") as f:
        data = bytearray(f.read())

    at = data.find(MARKER)
    if at < 0:
        sys.exit("stamp_integrity: marker not found in %s (is it a Dojo executable?)" % path)
    if data.find(MARKER, at + 1) >= 0:
        sys.exit("stamp_integrity: marker found more than once in %s" % path)

    hash_at = at + len(MARKER)
    data[hash_at:hash_at + HASH_LEN] = b"\x00" * HASH_LEN
    digest = hashlib.blake2b(bytes(data), digest_size=HASH_LEN, key=hash_key()).digest()
    data[hash_at:hash_at + HASH_LEN] = digest

    with open(path, "wb") as f:
        f.write(data)
    print("stamp_integrity: stamped %s (marker at %d)" % (path, at))


if __name__ == "__main__":
    main()

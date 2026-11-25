#include "licensing/Integrity.h"

#include "licensing/Crypto.h"
#include "licensing/Obfuscation.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

#ifndef DOJO_TAMPER_CHECK
 #define DOJO_TAMPER_CHECK 1
#endif

// The stamp: a 16-byte marker and 32 zero bytes, patched after the build with the keyed hash of the
// whole executable taken with these 32 bytes zeroed. Kept in its own object, referenced below so the
// linker cannot drop it. tools/stamp_integrity.py must agree byte-for-byte with this layout.
extern "C" {
alignas(16) volatile const unsigned char dojo_integrity_block[48] = {
    0x4b, 0x9d, 0x2e, 0xf1, 0x5a, 0x83, 0xc7, 0x60, 0x1d, 0xb4, 0x39, 0xee, 0x72, 0x08, 0xa6, 0xcf,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
}

namespace dojo::licensing::integrity
{
namespace
{
constexpr int kMarkerBytes = 16;
constexpr int kHashBytes = 32;

std::atomic<int> cached{static_cast<int>(Status::disabled)};
std::atomic<bool> ran{false};

/** The key the hash is taken with (obfuscated so it is not a clean 32-byte run in the binary). */
std::array<std::uint8_t, 32> hashKey()
{
    // Derived from an obfuscated string, so the raw key bytes are not stored directly.
    const auto seed = DOJO_OBF("TheDojo/integrity/v1/keystream-material-xyzzy");
    std::array<std::uint8_t, 32> key{};
    crypto::keyedHash(key, {reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size()},
                      {reinterpret_cast<const std::uint8_t*>(seed.data()), seed.size()});
    return key;
}

/** The marker, and a reference to the block so it is kept and matches the file. */
std::array<std::uint8_t, 16> markerBytes()
{
    std::array<std::uint8_t, 16> m{};
    for (int i = 0; i < kMarkerBytes; ++i)
        m[static_cast<std::size_t>(i)] = dojo_integrity_block[i];
    return m;
}

/** The first offset of the 16-byte marker in `image`, or -1. */
std::ptrdiff_t findMarker(const std::vector<std::uint8_t>& image)
{
    const auto m = markerBytes();
    if (image.size() < static_cast<std::size_t>(kMarkerBytes + kHashBytes))
        return -1;
    const auto it = std::search(image.begin(), image.end(), m.begin(), m.end());
    return it == image.end() ? -1 : std::distance(image.begin(), it);
}

Status evaluate(const std::vector<std::uint8_t>& image)
{
    const auto at = findMarker(image);
    if (at < 0)
        return Status::unstamped; // the marker is always compiled in; absent only in odd packaging
    const auto hashAt = static_cast<std::size_t>(at) + kMarkerBytes;

    std::array<std::uint8_t, 32> stored{};
    std::memcpy(stored.data(), image.data() + hashAt, kHashBytes);
    if (std::all_of(stored.begin(), stored.end(), [](std::uint8_t b) { return b == 0; }))
        return Status::unstamped; // built but not stamped

    // Hash the whole image with the stored-hash field zeroed (as the stamp tool did).
    std::vector<std::uint8_t> copy = image;
    std::memset(copy.data() + hashAt, 0, kHashBytes);
    std::array<std::uint8_t, 32> actual{};
    const auto key = hashKey();
    crypto::keyedHash(actual, key, copy);
    return crypto::equal(actual, stored) ? Status::ok : Status::tampered;
}
} // namespace

namespace detail
{
std::array<std::uint8_t, 16> marker() { return markerBytes(); }
Status evaluateImage(const std::vector<std::uint8_t>& image) { return evaluate(image); }

bool stampImage(std::vector<std::uint8_t>& image)
{
    const auto at = findMarker(image);
    if (at < 0)
        return false;
    const auto hashAt = static_cast<std::size_t>(at) + kMarkerBytes;
    std::memset(image.data() + hashAt, 0, kHashBytes);
    std::array<std::uint8_t, 32> hash{};
    crypto::keyedHash(hash, hashKey(), image);
    std::memcpy(image.data() + hashAt, hash.data(), kHashBytes);
    return true;
}
} // namespace detail

Status check()
{
    if (ran.load())
        return static_cast<Status>(cached.load());

    Status result = Status::disabled;
#if DOJO_TAMPER_CHECK
    const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::MemoryBlock block;
    if (!exe.existsAsFile() || !exe.loadFileAsData(block) || block.getSize() == 0)
    {
        result = Status::unreadable;
    }
    else
    {
        const auto* p = static_cast<const std::uint8_t*>(block.getData());
        std::vector<std::uint8_t> image(p, p + block.getSize());
        result = evaluate(image);
    }
#endif
    cached.store(static_cast<int>(result));
    ran.store(true);
    return result;
}

Status status() noexcept { return static_cast<Status>(cached.load()); }
bool isTampered() noexcept { return status() == Status::tampered; }

bool isDebuggerPresent() noexcept
{
#if JUCE_WINDOWS
    if (::IsDebuggerPresent())
        return true;
    BOOL remote = FALSE;
    if (::CheckRemoteDebuggerPresent(::GetCurrentProcess(), &remote) && remote)
        return true;
    return false;
#elif JUCE_LINUX || JUCE_BSD
    const auto status = juce::File("/proc/self/status").loadFileAsString();
    const auto line = status.fromFirstOccurrenceOf("TracerPid:", false, false).upToFirstOccurrenceOf("\n", false, false).trim();
    return line.isNotEmpty() && line.getIntValue() > 0;
#else
    return false;
#endif
}

} // namespace dojo::licensing::integrity

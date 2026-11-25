#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <vector>

/**
    A tamper check: at start-up The Dojo hashes its own executable (keyed BLAKE2b) and compares the
    result with a value stamped into the binary after the build (tools/stamp_integrity.py). A
    modified copy does not match, and the application locks (as with no license).

    It is a deterrent, not a guarantee: whoever can patch the binary can also patch this check. It
    is gated by the DOJO_TAMPER_CHECK build option, and an unstamped binary (every byte of the
    stamp still zero, e.g. a plain developer build) is not enforced.
*/
namespace dojo::licensing::integrity
{

enum class Status
{
    disabled,   ///< the build option is off
    unstamped,  ///< built but not stamped (e.g. a developer build): not enforced
    ok,         ///< the executable matches its stamp
    tampered,   ///< the executable has been modified
    unreadable, ///< the executable could not be read (not enforced)
};

/** Hashes this executable and caches the result; later calls return the cached status. */
Status check();
/** The last result of check() (disabled until check() has run). */
[[nodiscard]] Status status() noexcept;
/** True once check() found the executable modified. */
[[nodiscard]] bool isTampered() noexcept;

/** Whether a debugger is attached to this process (best effort; used only to mute the free trial). */
[[nodiscard]] bool isDebuggerPresent() noexcept;

namespace detail
{
/** The 16-byte marker that locates the stamp in the binary. */
[[nodiscard]] std::array<std::uint8_t, 16> marker();
/** Evaluates an in-memory image the way check() evaluates the file (for tests). */
[[nodiscard]] Status evaluateImage(const std::vector<std::uint8_t>& image);
/** Stamps an image in place, as the build tool stamps the file (for tests). False if no marker. */
bool stampImage(std::vector<std::uint8_t>& image);
} // namespace detail

} // namespace dojo::licensing::integrity

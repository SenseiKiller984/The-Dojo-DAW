#pragma once

#include <cstddef>
#include <cstdint>

namespace dojo
{

/** Internal mixing precision. Every bus, track and native DSP module processes 64-bit floating
    point; conversion to and from the driver's 32-bit float happens only at the device I/O edge. */
using Sample = double;

/** The sample format exchanged with audio hardware drivers. */
using IoSample = float;

/** Absolute timeline positions and durations, in samples. Signed so that pre-roll and negative
    offsets are representable; 64 bits covers millions of years at any practical sample rate. */
using SampleCount = std::int64_t;

/** Assumed destructive-interference (cache line) size used to pad data shared between threads so
    that a producer and a consumer never write to the same line (false sharing).

    std::hardware_destructive_interference_size is intentionally avoided: its value may differ
    between compilers and tuning flags, which makes it ABI-unstable (GCC warns about exactly this). */
inline constexpr std::size_t kCacheLineSize = 64;

} // namespace dojo

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <complex>
#include <numbers>
#include <vector>

/**
    Shared by the audio editors (pitch and spectral): a real FFT with its own buffers, windows and
    phase helpers. One RealFft per thread (the transforms are not re-entrant).
*/
namespace dojo::audioedit
{

class RealFft
{
public:
    explicit RealFft(int order);

    [[nodiscard]] int size() const noexcept { return size_; }
    [[nodiscard]] int bins() const noexcept { return size_ / 2 + 1; }
    /** `size()` real samples to `bins()` complex bins. */
    void forward(const float* in, std::complex<float>* out);
    /** `bins()` complex bins to `size()` real samples; inverse(forward(x)) is x. */
    void inverse(const std::complex<float>* in, float* out);

private:
    juce::dsp::FFT fft_;
    int size_;
    std::vector<float> work_;
};

/** A periodic Hann window (sums to a constant at hops of size / 2 and size / 4). */
[[nodiscard]] std::vector<float> hannWindow(int size);

/** A phase wrapped to (-pi, pi]. */
[[nodiscard]] inline double princarg(double phase) noexcept
{
    constexpr double twoPi = 2.0 * std::numbers::pi;
    return phase - twoPi * std::round(phase / twoPi);
}

/** The smallest power-of-two order whose size is at least `samples`. */
[[nodiscard]] int orderFor(double samples) noexcept;

/** MIDI note number (fractional) of a frequency, and back. */
[[nodiscard]] inline double hzToMidi(double hz) noexcept { return 69.0 + 12.0 * std::log2(hz / 440.0); }
[[nodiscard]] inline double midiToHz(double midi) noexcept { return 440.0 * std::exp2((midi - 69.0) / 12.0); }

} // namespace dojo::audioedit

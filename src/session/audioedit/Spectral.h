#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <vector>

/**
    Spectral editing: operations on time-frequency regions of audio, applied by STFT (Hann windows of
    about 46 ms, four times overlapped).

      gain      the region louder or quieter (soft edges: 10 ms in time, a twelfth of an octave in
                frequency)
      erase     the region removed
      repair    the region rebuilt from what surrounds it: each bin's level interpolated (in dB)
                between the frames just before and just after, its phase carried on from before at
                its own frequency - for clicks, coughs, chair creaks, a phone ringing
      denoise   noise reduction from a learned profile (the noise's average spectrum): spectral
                subtraction with over-subtraction `sensitivity`, at most `reductionDb`, the gains
                smoothed across neighbouring bins and over time against "musical noise"
      dehum     mains hum and its harmonics notched (50 or 60 Hz, or found in the audio)

    Only the spans the operations cover are processed (with 50 ms margins); everything else is the
    input, sample for sample, and each span is crossfaded in over 10 ms. Times are seconds into the
    audio given; frequencies Hz.
*/
namespace dojo::audioedit::spectral
{

enum class Kind
{
    gain,
    erase,
    repair,
    denoise,
    dehum,
};

struct Operation
{
    Kind kind = Kind::gain;
    double start = 0.0;
    double end = 0.0;
    double low = 0.0;          ///< Hz
    double high = 1.0e6;       ///< Hz (above Nyquist: to the top)
    double gainDb = -12.0;     ///< gain
    double amount = 1.0;       ///< 0..1: how much of the result (repair, denoise, dehum)
    std::vector<float> profile; ///< denoise: the noise's magnitude per bin (see learnProfile)
    double reductionDb = 18.0; ///< denoise: the most it takes away
    double sensitivity = 1.5;  ///< denoise: over-subtraction
    double humHz = 0.0;        ///< dehum: the fundamental (0: found in the audio)
    int harmonics = 8;         ///< dehum
    double depthDb = 40.0;     ///< dehum: the notches' depth
    bool enabled = true;
};

/** The FFT size the operations use at a sample rate (2048 at 44.1/48 kHz). */
[[nodiscard]] int fftSizeFor(double sampleRate) noexcept;

/** The average magnitude per bin of [start, end) (all channels): a noise profile for denoise. */
[[nodiscard]] std::vector<float> learnProfile(const juce::AudioBuffer<float>& audio, double sampleRate, double start, double end);

/** The mains frequency in the audio: about 50 or 60 Hz, refined to a twentieth of a hertz. */
[[nodiscard]] double detectHum(const juce::AudioBuffer<float>& audio, double sampleRate);

/** What a set of operations does to the audio (only the enabled ones count). */
[[nodiscard]] std::uint64_t keyOf(const std::vector<Operation>& operations);

/** The operations applied, in order: a new buffer the size of `audio` (empty if cancelled). */
[[nodiscard]] juce::AudioBuffer<float> apply(const juce::AudioBuffer<float>& audio, double sampleRate, const std::vector<Operation>& operations,
                                             const std::atomic<bool>* cancel = nullptr);

/** A spectrogram for display: rows on a log-frequency axis from `lowHz` to `highHz` (row 0 lowest),
    levels 0..255 for -110..0 dB, `frames` columns every `hop` seconds (frame i centred at i * hop). */
struct Spectrogram
{
    int frames = 0;
    int rows = 0;
    double hop = 0.01;
    double lowHz = 20.0;
    double highHz = 20000.0;
    double duration = 0.0;
    std::vector<std::uint8_t> levels; ///< frames x rows, frame-major

    [[nodiscard]] std::uint8_t at(int frame, int row) const noexcept { return levels[static_cast<std::size_t>(frame * rows + row)]; }
    [[nodiscard]] double rowHz(double row) const noexcept;
    [[nodiscard]] double hzRow(double hz) const noexcept;
};

[[nodiscard]] Spectrogram analyse(const juce::AudioBuffer<float>& audio, double sampleRate, int rows = 384, const std::atomic<bool>* cancel = nullptr);

} // namespace dojo::audioedit::spectral

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <vector>

/**
    Spectra for displays, and where two of them mask each other: the EQ shows its own track's
    spectrum with other tracks' over it (their sound from the engine's taps, or its sidechain), and
    shades the frequencies where another track is about as loud as, or louder than, this one.
*/
namespace dojo::ui::spectrum
{

/** `count` frequencies from `low` to `high`, evenly spaced on a log scale. */
[[nodiscard]] std::vector<double> logFrequencies(double low, double high, int count);

/** A Hann-windowed FFT of the newest samples, read out at chosen frequencies. */
class Analyser
{
public:
    explicit Analyser(int order = 12);

    /** How many samples one analysis takes. */
    [[nodiscard]] int size() const noexcept { return size_; }

    /**
        The level (dB; a full-scale sine reads 0) of `samples` (size() of them, oldest first) at each
        of `frequencies`: the strongest bin around each (bins between two frequencies count for the
        nearer one; below one bin per frequency, interpolated). `levels` keeps its previous values to
        smooth with: a rise follows at `rise`, a fall at `fall` (0 .. 1, 1 = at once); empty, it starts
        afresh.
    */
    void analyse(const float* samples, double sampleRate, const std::vector<double>& frequencies, std::vector<float>& levels,
                 float rise = 0.6f, float fall = 0.25f);

    static constexpr float kFloorDb = -100.0f;

private:
    int size_;
    juce::dsp::FFT fft_;
    std::vector<float> buffer_;
};

/**
    How much `other` masks `own` at each point, 0 .. 1: nothing where either is under `floorDb`;
    otherwise 0 with the other 6 dB or more below, 0.5 level with it, 1 at 6 dB or more above.
*/
[[nodiscard]] std::vector<float> masking(const std::vector<float>& own, const std::vector<float>& other, float floorDb = -60.0f);

/** A stretch of frequencies where the masking reaches `threshold`. */
struct Range
{
    double low = 0.0, high = 0.0;
    float amount = 0.0f; ///< the masking summed over the stretch (how much of it, how strongly)
};
/** The stretches where `amount` (at `frequencies`) reaches `threshold`, the strongest first. */
[[nodiscard]] std::vector<Range> maskedRanges(const std::vector<double>& frequencies, const std::vector<float>& amount, float threshold = 0.5f);

} // namespace dojo::ui::spectrum

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace dojo::devices::dsp
{

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

/** Frequency of a (fractional) MIDI note. */
inline double noteToFrequency(double note) noexcept
{
    return 440.0 * std::exp2((note - 69.0) / 12.0);
}

/**
    ADSR envelope: linear attack, exponential decay and release (RC curves, as analogue envelopes
    sound). Times are the time to (nearly) complete a segment. Real-time safe.
*/
class Envelope final
{
public:
    enum class Stage
    {
        idle,
        attack,
        decay,
        sustain,
        release,
    };

    void setParameters(double attackSeconds, double decaySeconds, double sustainLevel, double releaseSeconds, double sampleRate) noexcept;
    void noteOn() noexcept;
    void noteOff() noexcept;
    void reset() noexcept
    {
        stage_ = Stage::idle;
        level_ = 0.0;
    }

    double next() noexcept;
    /** Advances `n` samples; returns the level at the end. */
    double advance(int n) noexcept;

    [[nodiscard]] bool isActive() const noexcept { return stage_ != Stage::idle; }
    [[nodiscard]] bool isReleasing() const noexcept { return stage_ == Stage::release; }
    [[nodiscard]] double getLevel() const noexcept { return level_; }
    [[nodiscard]] Stage getStage() const noexcept { return stage_; }

private:
    Stage stage_ = Stage::idle;
    double level_ = 0.0;
    double attackStep_ = 1.0;
    double decayCoefficient_ = 1.0;
    double releaseCoefficient_ = 1.0;
    double sustain_ = 1.0;
};

/** Zero-delay-feedback state-variable filter (Cytomic / Zavalishin), one channel. */
class StateVariableFilter final
{
public:
    enum class Mode
    {
        lowPass,
        highPass,
        bandPass,
        notch,
    };

    /** `cutoff` in Hz, `resonance` 0..1 (self-oscillation is avoided). */
    void setParameters(double cutoff, double resonance, double sampleRate) noexcept
    {
        const double g = std::tan(kPi * std::clamp(cutoff, 10.0, sampleRate * 0.49) / sampleRate);
        k_ = 2.0 - 1.96 * std::clamp(resonance, 0.0, 1.0);
        a1_ = 1.0 / (1.0 + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    double process(double x, Mode mode) noexcept
    {
        const double v3 = x - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        switch (mode)
        {
        case Mode::lowPass: return v2;
        case Mode::highPass: return x - k_ * v1 - v2;
        case Mode::bandPass: return v1;
        case Mode::notch: return x - k_ * v1;
        }
        return v2;
    }

    void reset() noexcept { ic1_ = ic2_ = 0.0; }

private:
    double ic1_ = 0.0, ic2_ = 0.0;
    double k_ = 2.0, a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;
};

/** Filter types offered by the synths: 12 and 24 dB/oct low-pass, high-pass, band-pass, notch. */
class MultiModeFilter final
{
public:
    enum class Type
    {
        low12,
        low24,
        high12,
        band,
        notch,
    };

    void setParameters(double cutoff, double resonance, double sampleRate) noexcept
    {
        first_.setParameters(cutoff, resonance, sampleRate);
        // The second stage of the 24 dB slope is barely resonant: one peak, not two.
        second_.setParameters(cutoff, resonance * 0.25, sampleRate);
    }

    double process(double x, Type type) noexcept
    {
        switch (type)
        {
        case Type::low12: return first_.process(x, StateVariableFilter::Mode::lowPass);
        case Type::low24: return second_.process(first_.process(x, StateVariableFilter::Mode::lowPass), StateVariableFilter::Mode::lowPass);
        case Type::high12: return first_.process(x, StateVariableFilter::Mode::highPass);
        case Type::band: return first_.process(x, StateVariableFilter::Mode::bandPass);
        case Type::notch: return first_.process(x, StateVariableFilter::Mode::notch);
        }
        return x;
    }

    void reset() noexcept
    {
        first_.reset();
        second_.reset();
    }

private:
    StateVariableFilter first_, second_;
};

/** Low-frequency oscillator: sine, triangle, saw, square, random (sample & hold). Output -1..1. */
class Lfo final
{
public:
    enum class Shape
    {
        sine,
        triangle,
        saw,
        square,
        random,
    };

    void reset(double phase = 0.0) noexcept
    {
        phase_ = phase;
        held_ = 0.0;
    }

    /** Advances by `n` samples at `rateHz`; returns the value at the new position. */
    double advance(double rateHz, int n, double sampleRate, Shape shape) noexcept;

private:
    double phase_ = 0.0;
    double held_ = 0.0;
    std::uint32_t random_ = 0x9E3779B9u;
};

/**
    A band-limited, morphable single-cycle wavetable: `frames` waveforms of kSize samples, each
    stored at kLevels bandwidths (every level halves the highest harmonic: 1024, 512, .. 2), so a
    note never plays harmonics above Nyquist. Built once from a time-domain description.
*/
class BandLimitedWavetable final
{
public:
    static constexpr int kSize = 2048;
    static constexpr int kLevels = 10;

    /** `shape(frame position 0..1, phase 0..1)` describes the waveform; it is analysed with an FFT
        and re-synthesised per level. Every frame is normalised to a peak of 1. */
    BandLimitedWavetable(const char* name, int frames, const std::function<double(double, double)>& shape);

    [[nodiscard]] const char* getName() const noexcept { return name_; }
    [[nodiscard]] int getNumFrames() const noexcept { return frames_; }

    /** The level to use for a phase increment (cycles per sample). */
    [[nodiscard]] static int levelFor(double increment) noexcept
    {
        const double allowed = 0.5 / std::max(1.0e-9, increment); // highest harmonic below Nyquist
        int level = 0;
        while (level < kLevels - 1 && static_cast<double>(1024 >> level) > allowed)
            ++level;
        return level;
    }

    /** Sample at `phase` (0..1), morph `position` (0..1), bandwidth `level`. */
    [[nodiscard]] float read(double phase, double position, int level) const noexcept
    {
        const double framePosition = std::clamp(position, 0.0, 1.0) * static_cast<double>(frames_ - 1);
        const int frame = std::min(static_cast<int>(framePosition), frames_ - 1);
        const auto fraction = static_cast<float>(framePosition - frame);
        const float a = readFrame(frame, level, phase);
        if (fraction <= 0.0f || frame + 1 >= frames_)
            return a;
        return a + (readFrame(frame + 1, level, phase) - a) * fraction;
    }

    /** One cycle of a frame at full bandwidth (for drawing). */
    [[nodiscard]] const float* getFrame(int frame, int level = 0) const noexcept
    {
        return data_.data() + (static_cast<std::size_t>(std::clamp(frame, 0, frames_ - 1)) * kLevels + static_cast<std::size_t>(level)) * kStride;
    }

private:
    static constexpr std::size_t kStride = kSize + 1; ///< one guard sample for interpolation

    [[nodiscard]] float readFrame(int frame, int level, double phase) const noexcept
    {
        const float* table = getFrame(frame, level);
        const double position = (phase - std::floor(phase)) * kSize;
        const int index = static_cast<int>(position);
        const auto fraction = static_cast<float>(position - index);
        return table[index] + (table[index + 1] - table[index]) * fraction;
    }

    const char* name_;
    int frames_;
    std::vector<float> data_; ///< [frame][level][kStride]
};

} // namespace dojo::devices::dsp

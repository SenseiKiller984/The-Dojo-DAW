#pragma once

#include "devices/dsp/Biquad.h"
#include "devices/dsp/SynthCore.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace dojo::devices::dsp
{

//==================================================================================================
/** Tempo-sync divisions for LFOs and delays: 1/64 .. 8 bars, with dotted and triplet values. */
inline const juce::StringArray& syncDivisionNames()
{
    static const juce::StringArray names{"1/64", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T",
                                         "1/4",  "1/4D", "1/2T",  "1/2",  "1/2D",  "1 bar", "2 bars", "4 bars", "8 bars"};
    return names;
}

/** Beats (quarter notes) of a division. */
inline double syncDivisionBeats(int index) noexcept
{
    constexpr std::array<double, 18> beats{0.0625, 0.125, 1.0 / 6.0, 0.25, 0.375, 1.0 / 3.0, 0.5, 0.75, 2.0 / 3.0,
                                           1.0,    1.5,   4.0 / 3.0, 2.0,  3.0,   4.0,       8.0, 16.0, 32.0};
    return beats[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(beats.size()) - 1))];
}

/** Where the transport is (what tempo-synced effects need). */
struct TransportState
{
    double bpm = 120.0;
    double ppq = 0.0;
    double seconds = 0.0;
    bool playing = false;
};

inline TransportState readTransport(juce::AudioPlayHead* head) noexcept
{
    TransportState t;
    if (head != nullptr)
        if (const auto position = head->getPosition())
        {
            t.bpm = std::max(1.0, position->getBpm().orFallback(120.0));
            t.ppq = position->getPpqPosition().orFallback(0.0);
            t.seconds = position->getTimeInSeconds().orFallback(t.ppq * 60.0 / t.bpm);
            t.playing = position->getIsPlaying();
        }
    return t;
}

//==================================================================================================
/** LFO waveforms of the modulation effects and the LFO device. */
enum class Waveform
{
    sine,
    triangle,
    sawUp,
    sawDown,
    square,
    sampleHold,   ///< a new random value each cycle
    smoothRandom, ///< random values joined by smooth curves
    wander,       ///< two smooth random lines (slow and faster) for an organic drift
};

inline const juce::StringArray& waveformNames()
{
    static const juce::StringArray names{"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Smooth Random", "Wander"};
    return names;
}

/** A deterministic random value in -1..1 for cycle `k` (the same cycle always gives the same value,
    so phase-shifted copies of a random LFO agree with each other). */
inline double hashRandom(long long k, std::uint32_t seed) noexcept
{
    auto x = static_cast<std::uint64_t>(k) * 0x9E3779B97F4A7C15ull + seed * 0xBF58476D1CE4E5B9ull;
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBull;
    x ^= x >> 31;
    return static_cast<double>(x >> 11) / static_cast<double>(1ull << 52) - 1.0;
}

/** The waveform's value (-1..1) at a total phase (cycles since the start, any size). */
inline double waveformAt(Waveform w, double totalPhase, std::uint32_t seed = 1) noexcept
{
    const double cycle = std::floor(totalPhase);
    const double p = totalPhase - cycle;
    const auto k = static_cast<long long>(cycle);
    switch (w)
    {
    case Waveform::sine: return std::sin(kTwoPi * p);
    case Waveform::triangle: return p < 0.25 ? 4.0 * p : p < 0.75 ? 2.0 - 4.0 * p : 4.0 * p - 4.0;
    case Waveform::sawUp: return 2.0 * p - 1.0;
    case Waveform::sawDown: return 1.0 - 2.0 * p;
    case Waveform::square: return p < 0.5 ? 1.0 : -1.0;
    case Waveform::sampleHold: return hashRandom(k, seed);
    case Waveform::smoothRandom:
    {
        const double a = hashRandom(k, seed), b = hashRandom(k + 1, seed);
        const double t = 0.5 - 0.5 * std::cos(kPi * p);
        return a + (b - a) * t;
    }
    case Waveform::wander:
    {
        const auto smooth = [&](double phase, std::uint32_t s)
        {
            const double c = std::floor(phase);
            const auto i = static_cast<long long>(c);
            const double t = 0.5 - 0.5 * std::cos(kPi * (phase - c));
            const double a = hashRandom(i, s), b = hashRandom(i + 1, s);
            return a + (b - a) * t;
        };
        return std::clamp(0.75 * smooth(totalPhase, seed) + 0.35 * smooth(totalPhase * 3.1, seed + 7u), -1.0, 1.0);
    }
    }
    return 0.0;
}

/**
    An LFO for effects: free-running at a rate in Hz, or locked to the song position (a cycle every
    `beats` quarter notes) while the transport plays. value() reads it at any phase offset (stereo
    LFOs), consistently for the random shapes too.
*/
class ModLfo final
{
public:
    void reset(double phase = 0.0) noexcept { phase_ = phase; }

    /** Advances `n` samples. With `sync` and a playing transport the phase follows `ppqAtEnd`. */
    void advance(int n, double sampleRate, double rateHz, bool sync, double beats, const TransportState& t,
                 double ppqAtEnd) noexcept
    {
        if (sync && t.playing)
            phase_ = ppqAtEnd / std::max(1.0e-6, beats);
        else if (sync)
            phase_ += n / sampleRate * (t.bpm / 60.0) / std::max(1.0e-6, beats);
        else
            phase_ += rateHz * n / sampleRate;
    }

    [[nodiscard]] double value(Waveform w, double phaseOffset = 0.0, std::uint32_t seed = 1) const noexcept
    {
        return waveformAt(w, phase_ + phaseOffset, seed);
    }
    [[nodiscard]] double getPhase() const noexcept { return phase_; }

private:
    double phase_ = 0.0;
};

//==================================================================================================
/** Peak envelope follower with separate attack and release (linear level out). */
class EnvelopeFollower final
{
public:
    void setTimes(double attackSeconds, double releaseSeconds, double sampleRate) noexcept
    {
        attack_ = onePoleCoefficient(attackSeconds, sampleRate);
        release_ = onePoleCoefficient(releaseSeconds, sampleRate);
    }
    double process(double input) noexcept
    {
        const double x = std::abs(input);
        level_ = x + (level_ - x) * (x > level_ ? attack_ : release_);
        return level_;
    }
    [[nodiscard]] double getLevel() const noexcept { return level_; }
    void reset() noexcept { level_ = 0.0; }

    /** A level mapped to 0..1 from -60 .. 0 dBFS (what modulation amounts scale). */
    [[nodiscard]] static double normalise(double level) noexcept
    {
        return std::clamp((gainToDb(level) + 60.0) / 60.0, 0.0, 1.0);
    }

private:
    double attack_ = 0.0, release_ = 0.0, level_ = 0.0;
};

//==================================================================================================
/** Fourth-order Linkwitz-Riley crossover: low + high sum to an all-pass (flat magnitude). */
class LinkwitzRiley final
{
public:
    void setFrequency(double frequency, double sampleRate) noexcept
    {
        constexpr double q = 0.70710678118654752;
        low_ = BiquadCoefficients::make(BiquadCoefficients::Shape::highCut, sampleRate, frequency, q, 0.0);
        high_ = BiquadCoefficients::make(BiquadCoefficients::Shape::lowCut, sampleRate, frequency, q, 0.0);
    }
    void split(double x, double& low, double& high) noexcept
    {
        low = lp_[1].process(lp_[0].process(x, low_), low_);
        high = hp_[1].process(hp_[0].process(x, high_), high_);
    }
    /** The same phase shift as split() without splitting (keeps other bands aligned). */
    double allPass(double x) noexcept
    {
        double l = 0.0, h = 0.0;
        split(x, l, h);
        return l + h;
    }
    void reset() noexcept
    {
        for (auto& b : lp_)
            b.reset();
        for (auto& b : hp_)
            b.reset();
    }

private:
    BiquadCoefficients low_, high_;
    std::array<Biquad, 2> lp_, hp_;
};

//==================================================================================================
/** A delay line read at fractional positions (4-point Hermite). Sized once, then real-time safe. */
class DelayLine final
{
public:
    void setMaximum(int samples)
    {
        int size = 1;
        while (size < samples + 4)
            size <<= 1;
        buffer_.assign(static_cast<std::size_t>(size), 0.0);
        mask_ = size - 1;
        write_ = 0;
    }
    void reset() noexcept
    {
        std::fill(buffer_.begin(), buffer_.end(), 0.0);
        write_ = 0;
    }
    void push(double x) noexcept
    {
        buffer_[static_cast<std::size_t>(write_)] = x;
        write_ = (write_ + 1) & mask_;
    }
    /** The sample `delay` samples before the most recently pushed one (delay >= 0). */
    [[nodiscard]] double read(double delay) const noexcept
    {
        delay = std::clamp(delay, 0.0, static_cast<double>(mask_ - 3));
        const double position = static_cast<double>(write_ - 1) - delay;
        const double floorPosition = std::floor(position);
        const auto i = static_cast<int>(floorPosition);
        const double f = position - floorPosition;
        const auto at = [this](int k) { return buffer_[static_cast<std::size_t>(k & mask_)]; };
        const double xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
        const double c = (x1 - xm1) * 0.5;
        const double v = x0 - x1;
        const double w = c + v;
        const double a = w + v + (x2 - x0) * 0.5;
        const double b = w + a;
        return ((a * f - b) * f + c) * f + x0;
    }
    [[nodiscard]] int capacity() const noexcept { return mask_ - 3; }

private:
    std::vector<double> buffer_{0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
    int mask_ = 7;
    int write_ = 0;
};

//==================================================================================================
/** Topology-preserving one-pole filter: low-pass and high-pass outputs. */
class OnePole final
{
public:
    void setFrequency(double frequency, double sampleRate) noexcept
    {
        const double g = std::tan(kPi * std::clamp(frequency, 1.0, sampleRate * 0.49) / sampleRate);
        g_ = g / (1.0 + g);
    }
    double lowPass(double x) noexcept
    {
        const double v = (x - s_) * g_;
        const double y = v + s_;
        s_ = y + v;
        return y;
    }
    double highPass(double x) noexcept { return x - lowPass(x); }
    void reset() noexcept { s_ = 0.0; }

private:
    double g_ = 0.5, s_ = 0.0;
};

/** DC blocker (about 10 Hz). */
class DcBlocker final
{
public:
    void prepare(double sampleRate) noexcept { r_ = 1.0 - kTwoPi * 10.0 / sampleRate; }
    double process(double x) noexcept
    {
        const double y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = y;
        return y;
    }
    void reset() noexcept { x1_ = y1_ = 0.0; }

private:
    double r_ = 0.999, x1_ = 0.0, y1_ = 0.0;
};

//==================================================================================================
/**
    A 90-degree phase splitter (two all-pass chains; Olli Niemitalo's design): `inPhase` and
    `quadrature` differ by 90 degrees within a degree or so from 20 Hz to 20 kHz at 44.1-96 kHz.
    The basis of single-sideband frequency shifting.
*/
class HilbertTransformer final
{
public:
    void process(double x, double& inPhase, double& quadrature) noexcept
    {
        static constexpr std::array<double, 4> a{0.6923878, 0.9360654322959, 0.9882295226860, 0.9987488452737};
        static constexpr std::array<double, 4> b{0.4021921162426, 0.8561710882420, 0.9722909545651, 0.9952884791278};
        inPhase = delayed_; // (the first chain's output is the one delayed by a sample)
        delayed_ = chain(a_, a, x);
        quadrature = chain(b_, b, x);
    }
    void reset() noexcept
    {
        a_ = {};
        b_ = {};
        delayed_ = 0.0;
    }

private:
    struct Stage
    {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
    };
    static double chain(std::array<Stage, 4>& stages, const std::array<double, 4>& coefficients, double x) noexcept
    {
        for (std::size_t i = 0; i < 4; ++i)
        {
            auto& s = stages[i];
            const double c = coefficients[i] * coefficients[i];
            const double y = c * (x + s.y2) - s.x2;
            s.x2 = s.x1;
            s.x1 = x;
            s.y2 = s.y1;
            s.y1 = y;
            x = y;
        }
        return x;
    }

    std::array<Stage, 4> a_{}, b_{};
    double delayed_ = 0.0;
};

/** A fast xorshift noise source (-1..1). */
class Noise final
{
public:
    explicit Noise(std::uint32_t seed = 0x2545F491u) : state_(seed == 0 ? 1u : seed) {}
    double next() noexcept
    {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return static_cast<double>(state_) / 2147483648.0 - 1.0;
    }

private:
    std::uint32_t state_;
};

} // namespace dojo::devices::dsp

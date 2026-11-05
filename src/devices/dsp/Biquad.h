#pragma once

#include <algorithm>
#include <cmath>
#include <complex>

namespace dojo::devices::dsp
{

/** Normalised (a0 = 1) biquad coefficients, from the RBJ "Audio EQ Cookbook". */
struct BiquadCoefficients
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    enum class Shape
    {
        lowCut,   ///< 2nd-order high-pass (12 dB/oct)
        lowShelf,
        peak,
        highShelf,
        highCut,  ///< 2nd-order low-pass
    };

    static BiquadCoefficients make(Shape shape, double sampleRate, double frequency, double q, double gainDb) noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        frequency = std::clamp(frequency, 5.0, sampleRate * 0.49);
        q = std::max(0.025, q);

        const double w0 = 2.0 * pi * frequency / sampleRate;
        const double cosW = std::cos(w0);
        const double alpha = std::sin(w0) / (2.0 * q);
        const double A = std::pow(10.0, gainDb / 40.0);
        const double sqrtA2alpha = 2.0 * std::sqrt(A) * alpha;

        double b0 = 1, b1 = 0, b2 = 0, a0 = 1, a1 = 0, a2 = 0;
        switch (shape)
        {
        case Shape::lowCut:
            b0 = (1.0 + cosW) / 2.0; b1 = -(1.0 + cosW); b2 = (1.0 + cosW) / 2.0;
            a0 = 1.0 + alpha; a1 = -2.0 * cosW; a2 = 1.0 - alpha;
            break;
        case Shape::highCut:
            b0 = (1.0 - cosW) / 2.0; b1 = 1.0 - cosW; b2 = (1.0 - cosW) / 2.0;
            a0 = 1.0 + alpha; a1 = -2.0 * cosW; a2 = 1.0 - alpha;
            break;
        case Shape::peak:
            b0 = 1.0 + alpha * A; b1 = -2.0 * cosW; b2 = 1.0 - alpha * A;
            a0 = 1.0 + alpha / A; a1 = -2.0 * cosW; a2 = 1.0 - alpha / A;
            break;
        case Shape::lowShelf:
            b0 = A * ((A + 1.0) - (A - 1.0) * cosW + sqrtA2alpha);
            b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosW);
            b2 = A * ((A + 1.0) - (A - 1.0) * cosW - sqrtA2alpha);
            a0 = (A + 1.0) + (A - 1.0) * cosW + sqrtA2alpha;
            a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosW);
            a2 = (A + 1.0) + (A - 1.0) * cosW - sqrtA2alpha;
            break;
        case Shape::highShelf:
            b0 = A * ((A + 1.0) + (A - 1.0) * cosW + sqrtA2alpha);
            b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosW);
            b2 = A * ((A + 1.0) + (A - 1.0) * cosW - sqrtA2alpha);
            a0 = (A + 1.0) - (A - 1.0) * cosW + sqrtA2alpha;
            a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosW);
            a2 = (A + 1.0) - (A - 1.0) * cosW - sqrtA2alpha;
            break;
        }

        return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }

    /** |H| at `frequency`. */
    [[nodiscard]] double magnitudeAt(double frequency, double sampleRate) const noexcept
    {
        constexpr double pi = 3.14159265358979323846;
        const double w = 2.0 * pi * frequency / sampleRate;
        const std::complex<double> z1 = std::polar(1.0, -w);
        const std::complex<double> z2 = z1 * z1;
        const auto numerator = b0 + b1 * z1 + b2 * z2;
        const auto denominator = 1.0 + a1 * z1 + a2 * z2;
        return std::abs(numerator / denominator);
    }
};

/** Transposed direct form II biquad state. */
struct Biquad
{
    double z1 = 0.0;
    double z2 = 0.0;

    double process(double x, const BiquadCoefficients& c) noexcept
    {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0; }
};

/** One-pole smoothing coefficient for a time constant in seconds. */
inline double onePoleCoefficient(double seconds, double sampleRate) noexcept
{
    return seconds <= 0.0 ? 0.0 : std::exp(-1.0 / (seconds * sampleRate));
}

inline double dbToGain(double db) noexcept { return std::pow(10.0, db / 20.0); }
inline double gainToDb(double gain) noexcept { return 20.0 * std::log10(std::max(gain, 1.0e-12)); }

/** 4-point Hermite interpolation of a float array (out-of-range reads as 0). */
inline double hermite(const float* data, long long length, double position) noexcept
{
    const double floorPosition = std::floor(position);
    const auto i = static_cast<long long>(floorPosition);
    const double f = position - floorPosition;
    const auto at = [&](long long k) { return (k >= 0 && k < length) ? static_cast<double>(data[k]) : 0.0; };
    const double xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
    const double c = (x1 - xm1) * 0.5;
    const double v = x0 - x1;
    const double w = c + v;
    const double a = w + v + (x2 - x0) * 0.5;
    const double bNeg = w + a;
    return (((a * f) - bNeg) * f + c) * f + x0;
}

} // namespace dojo::devices::dsp

#include "devices/dsp/SynthCore.h"

#include <juce_dsp/juce_dsp.h>

namespace dojo::devices::dsp
{

//==================================================================================================
// Envelope

void Envelope::setParameters(double attackSeconds, double decaySeconds, double sustainLevel, double releaseSeconds,
                             double sampleRate) noexcept
{
    const auto coefficient = [sampleRate](double seconds)
    {
        // Reaches ~99 % of the way in `seconds`.
        return 1.0 - std::exp(-4.6 / std::max(1.0, seconds * sampleRate));
    };
    attackStep_ = 1.0 / std::max(1.0, attackSeconds * sampleRate);
    decayCoefficient_ = coefficient(decaySeconds);
    releaseCoefficient_ = coefficient(releaseSeconds);
    sustain_ = std::clamp(sustainLevel, 0.0, 1.0);
}

void Envelope::noteOn() noexcept
{
    stage_ = Stage::attack; // from the current level: retriggering does not click
}

void Envelope::noteOff() noexcept
{
    if (stage_ != Stage::idle)
        stage_ = Stage::release;
}

double Envelope::next() noexcept
{
    switch (stage_)
    {
    case Stage::idle:
        return 0.0;
    case Stage::attack:
        level_ += attackStep_;
        if (level_ >= 1.0)
        {
            level_ = 1.0;
            stage_ = Stage::decay;
        }
        break;
    case Stage::decay:
        level_ += (sustain_ - level_) * decayCoefficient_;
        if (std::abs(level_ - sustain_) < 1.0e-4)
        {
            level_ = sustain_;
            stage_ = Stage::sustain;
        }
        break;
    case Stage::sustain:
        level_ = sustain_;
        break;
    case Stage::release:
        level_ -= level_ * releaseCoefficient_;
        if (level_ < 1.0e-5)
        {
            level_ = 0.0;
            stage_ = Stage::idle;
        }
        break;
    }
    return level_;
}

double Envelope::advance(int n) noexcept
{
    for (int i = 0; i < n; ++i)
        next();
    return level_;
}

//==================================================================================================
// LFO

double Lfo::advance(double rateHz, int n, double sampleRate, Shape shape) noexcept
{
    phase_ += rateHz * n / sampleRate;
    if (phase_ >= 1.0)
    {
        phase_ -= std::floor(phase_);
        random_ ^= random_ << 13;
        random_ ^= random_ >> 17;
        random_ ^= random_ << 5;
        held_ = static_cast<double>(random_) / 2147483648.0 - 1.0;
    }

    switch (shape)
    {
    case Shape::sine: return std::sin(kTwoPi * phase_);
    case Shape::triangle: return phase_ < 0.5 ? 4.0 * phase_ - 1.0 : 3.0 - 4.0 * phase_;
    case Shape::saw: return 2.0 * phase_ - 1.0;
    case Shape::square: return phase_ < 0.5 ? 1.0 : -1.0;
    case Shape::random: return held_;
    }
    return 0.0;
}

//==================================================================================================
// Band-limited wavetable

BandLimitedWavetable::BandLimitedWavetable(const char* name, int frames, const std::function<double(double, double)>& shape)
    : name_(name)
    , frames_(std::max(1, frames))
    , data_(static_cast<std::size_t>(frames_) * kLevels * kStride, 0.0f)
{
    constexpr int kOrder = 11; // 2^11 = kSize
    static_assert((1 << kOrder) == kSize);
    juce::dsp::FFT fft(kOrder);
    std::vector<float> spectrum(2 * kSize);
    std::vector<float> work(2 * kSize);

    for (int frame = 0; frame < frames_; ++frame)
    {
        const double position = frames_ > 1 ? static_cast<double>(frame) / (frames_ - 1) : 0.0;

        // Analyse the described cycle.
        std::fill(spectrum.begin(), spectrum.end(), 0.0f);
        double mean = 0.0;
        for (int i = 0; i < kSize; ++i)
        {
            const double value = shape(position, static_cast<double>(i) / kSize);
            spectrum[static_cast<std::size_t>(i)] = static_cast<float>(value);
            mean += value;
        }
        mean /= kSize;
        for (int i = 0; i < kSize; ++i)
            spectrum[static_cast<std::size_t>(i)] -= static_cast<float>(mean); // no DC
        fft.performRealOnlyForwardTransform(spectrum.data(), true);

        float peak = 0.0f;
        for (int level = 0; level < kLevels; ++level)
        {
            const int maxHarmonic = 1024 >> level;
            std::copy(spectrum.begin(), spectrum.end(), work.begin());
            // Interleaved complex bins: keep 1..maxHarmonic (and their mirror), clear the rest.
            for (int bin = 0; bin <= kSize / 2; ++bin)
                if (bin == 0 || bin > maxHarmonic)
                {
                    work[static_cast<std::size_t>(2 * bin)] = 0.0f;
                    work[static_cast<std::size_t>(2 * bin + 1)] = 0.0f;
                    if (bin > 0 && bin < kSize / 2)
                    {
                        work[static_cast<std::size_t>(2 * (kSize - bin))] = 0.0f;
                        work[static_cast<std::size_t>(2 * (kSize - bin) + 1)] = 0.0f;
                    }
                }
            fft.performRealOnlyInverseTransform(work.data());

            float* table = data_.data() + (static_cast<std::size_t>(frame) * kLevels + static_cast<std::size_t>(level)) * kStride;
            for (int i = 0; i < kSize; ++i)
                table[i] = work[static_cast<std::size_t>(i)];
            table[kSize] = table[0];
            if (level == 0)
                for (int i = 0; i < kSize; ++i)
                    peak = std::max(peak, std::abs(table[i]));
        }

        // The same scale for every level of a frame: frames are equally loud, levels consistent.
        const float scale = peak > 1.0e-6f ? 1.0f / peak : 0.0f;
        for (int level = 0; level < kLevels; ++level)
        {
            float* table = data_.data() + (static_cast<std::size_t>(frame) * kLevels + static_cast<std::size_t>(level)) * kStride;
            for (std::size_t i = 0; i < kStride; ++i)
                table[i] *= scale;
        }
    }
}

} // namespace dojo::devices::dsp

#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <memory>
#include <vector>

namespace dojo::devices::dsp
{

/**
    Runs a stereo non-linearity at a higher sample rate (half-band FIR filters, integer latency) and
    keeps a dry copy delayed by the same latency, so dry/wet mixing never comb-filters.
    prepare() allocates; process() is real-time safe.
*/
class Oversampler final
{
public:
    /** `factorLog2`: 1 = 2x, 2 = 4x. */
    void prepare(int factorLog2, int maximumBlockSize)
    {
        factor_ = 1 << factorLog2;
        oversampling_ = std::make_unique<juce::dsp::Oversampling<double>>(
            2, static_cast<std::size_t>(factorLog2), juce::dsp::Oversampling<double>::filterHalfBandFIREquiripple, true, true);
        oversampling_->initProcessing(static_cast<std::size_t>(maximumBlockSize));
        oversampling_->reset();
        latency_ = static_cast<int>(std::lround(oversampling_->getLatencyInSamples()));
        for (auto& d : dry_)
            d.assign(static_cast<std::size_t>(latency_ + maximumBlockSize + 1), 0.0);
        dryWrite_ = 0;
    }

    [[nodiscard]] int getLatency() const noexcept { return latency_; }
    [[nodiscard]] int getFactor() const noexcept { return factor_; }

    /** Upsamples `channels` (2 x n), calls `shape(double* data, std::size_t count, int channel)` on each
        oversampled channel and downsamples back in place. The dry input is kept (see dry()): the
        channels as they came in, or `dryChannels` when given (the signal before some pre-processing). */
    template <typename Shape>
    void process(double* const* channels, int n, Shape&& shape, const double* const* dryChannels = nullptr) noexcept
    {
        if (oversampling_ == nullptr || n <= 0)
            return;
        const int size = static_cast<int>(dry_[0].size());
        const double* const* source = dryChannels != nullptr ? dryChannels : channels;
        for (int i = 0; i < n; ++i)
            for (std::size_t ch = 0; ch < 2; ++ch)
                dry_[ch][static_cast<std::size_t>((dryWrite_ + i) % size)] = source[ch][i];

        juce::dsp::AudioBlock<double> block(channels, 2, 0, static_cast<std::size_t>(n));
        auto up = oversampling_->processSamplesUp(block);
        for (std::size_t ch = 0; ch < 2; ++ch)
            shape(up.getChannelPointer(ch), up.getNumSamples(), static_cast<int>(ch));
        oversampling_->processSamplesDown(block);
        dryStart_ = dryWrite_;
        dryWrite_ = (dryWrite_ + n) % size;
    }

    /** The dry input of sample `i` of the last processed block, delayed by the latency. */
    [[nodiscard]] double dry(int channel, int i) const noexcept
    {
        const int size = static_cast<int>(dry_[0].size());
        const int slot = ((dryStart_ + i - latency_) % size + size) % size;
        return dry_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
    }

    void reset() noexcept
    {
        if (oversampling_ != nullptr)
            oversampling_->reset();
        for (auto& d : dry_)
            std::fill(d.begin(), d.end(), 0.0);
    }

private:
    std::unique_ptr<juce::dsp::Oversampling<double>> oversampling_;
    std::array<std::vector<double>, 2> dry_;
    int dryWrite_ = 0, dryStart_ = 0, latency_ = 0, factor_ = 1;
};

} // namespace dojo::devices::dsp

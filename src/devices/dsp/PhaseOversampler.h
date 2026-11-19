#pragma once

#include <juce_dsp/juce_dsp.h>

#include <array>
#include <cmath>
#include <memory>
#include <vector>

namespace dojo::devices::dsp
{

/**
    Oversampling with a choice of factor and of filter phase, for non-linear processing (compressors,
    clippers) that must not alias.

    - Factor: Off (1x), 2x, 4x or 8x.
    - Phase: Minimum (IIR half-band filters: the lowest latency, the phase bends near the top of
      the band), Intermediate (a shorter linear-phase first stage, minimum-phase stages after it: a
      medium latency) or Linear (linear-phase FIR filters at every stage: the highest latency, the
      best phase alignment of the upper frequencies).
    - Every setting has an integer number of samples of latency at DC: the filters' fractional
      delay is topped up with a first-order Thiran all-pass, so the device can report it exactly and
      the latency-compensated dry signal (dry()) lines up.

    prepare() builds every combination (it allocates); select() and process() are real-time safe,
    so the factor and phase can change while playing (e.g. a different factor for offline renders).
*/
class PhaseOversampler final
{
public:
    static constexpr int kChannels = 2;
    static constexpr int kFactorChoices = 4; ///< Off, 2x, 4x, 8x
    static constexpr int kPhaseChoices = 3;  ///< Minimum, Intermediate, Linear

    static const juce::StringArray& factorNames()
    {
        static const juce::StringArray names{"Off", "2x", "4x", "8x"};
        return names;
    }
    static const juce::StringArray& phaseNames()
    {
        static const juce::StringArray names{"Minimum Phase", "Intermediate", "Linear Phase"};
        return names;
    }

    void prepare(int maximumBlockSize)
    {
        maxBlock_ = std::max(1, maximumBlockSize);
        int longest = 0;
        for (int f = 1; f < kFactorChoices; ++f)
            for (int p = 0; p < kPhaseChoices; ++p)
            {
                auto& engine = engines_[static_cast<std::size_t>(f - 1)][static_cast<std::size_t>(p)];
                engine.oversampling = std::make_unique<juce::dsp::Oversampling<double>>(static_cast<std::size_t>(kChannels));
                for (int stage = 0; stage < f; ++stage)
                {
                    using Type = juce::dsp::Oversampling<double>::FilterType;
                    const bool fir = p == 2 || (p == 1 && stage == 0);
                    const bool relaxed = p == 1 && stage == 0; // (the shorter first stage)
                    const float widthUp = (relaxed ? 0.12f : 0.10f) * (stage == 0 ? 0.5f : 1.0f);
                    const float widthDown = (relaxed ? 0.15f : 0.12f) * (stage == 0 ? 0.5f : 1.0f);
                    const float stopUp = (relaxed ? -70.0f : -90.0f) + 10.0f * static_cast<float>(stage);
                    const float stopDown = (relaxed ? -60.0f : -75.0f) + 10.0f * static_cast<float>(stage);
                    engine.oversampling->addOversamplingStage(fir ? Type::filterHalfBandFIREquiripple : Type::filterHalfBandPolyphaseIIR,
                                                              widthUp, stopUp, widthDown, stopDown);
                }
                engine.oversampling->initProcessing(static_cast<std::size_t>(maxBlock_));
                engine.oversampling->reset();
                // Top the filters' delay up to a whole number of samples (0.5 .. 1.5 more, where a
                // first-order Thiran all-pass is well behaved).
                const double filters = engine.oversampling->getLatencyInSamples();
                engine.latency = static_cast<int>(std::ceil(filters + 0.5));
                const double extra = engine.latency - filters;
                engine.thiran = (1.0 - extra) / (1.0 + extra);
                engine.thiranState.fill({});
                longest = std::max(longest, engine.latency);
            }
        for (auto& d : dry_)
            d.assign(static_cast<std::size_t>(longest + maxBlock_ + 1), 0.0);
        dryWrite_ = dryStart_ = 0;
    }

    /** Factor choice (0 Off .. 3 8x) and phase choice (0 Minimum .. 2 Linear). */
    void select(int factor, int phase) noexcept
    {
        factor = std::clamp(factor, 0, kFactorChoices - 1);
        phase = std::clamp(phase, 0, kPhaseChoices - 1);
        if (factor == factor_ && phase == phase_)
            return;
        factor_ = factor;
        phase_ = phase;
        if (auto* engine = current())
        {
            engine->oversampling->reset();
            engine->thiranState.fill({});
        }
    }

    [[nodiscard]] int getFactorChoice() const noexcept { return factor_; }
    /** The processing rate's multiple of the base rate (1, 2, 4 or 8). */
    [[nodiscard]] int getRateMultiple() const noexcept { return 1 << factor_; }
    [[nodiscard]] int getLatency() const noexcept { return latencyOf(factor_, phase_); }
    [[nodiscard]] int latencyOf(int factor, int phase) const noexcept
    {
        if (factor <= 0)
            return 0;
        const auto& engine = engines_[static_cast<std::size_t>(std::min(factor, kFactorChoices - 1) - 1)]
                                     [static_cast<std::size_t>(std::clamp(phase, 0, kPhaseChoices - 1))];
        return engine.latency;
    }

    /**
        Runs `shape(double** channels, std::size_t count)` on both channels at the processing rate
        (the channels are processed together, so stereo-linked processing sees both) and returns the
        result in place, delayed by getLatency().
    */
    template <typename Shape>
    void process(double* const* channels, int n, Shape&& shape) noexcept
    {
        if (n <= 0)
            return;
        const int size = static_cast<int>(dry_[0].size());
        for (int i = 0; i < n; ++i)
            for (std::size_t ch = 0; ch < kChannels; ++ch)
                dry_[ch][static_cast<std::size_t>((dryWrite_ + i) % size)] = channels[ch][i];
        dryStart_ = dryWrite_;
        dryWrite_ = (dryWrite_ + n) % size;

        auto* engine = current();
        if (engine == nullptr)
        {
            double* data[kChannels] = {channels[0], channels[1]};
            shape(data, static_cast<std::size_t>(n));
            return;
        }

        juce::dsp::AudioBlock<double> block(channels, kChannels, 0, static_cast<std::size_t>(n));
        auto up = engine->oversampling->processSamplesUp(block);
        double* data[kChannels] = {up.getChannelPointer(0), up.getChannelPointer(1)};
        shape(data, up.getNumSamples());
        engine->oversampling->processSamplesDown(block);

        // The fractional rest of the delay (exact at DC).
        const double a = engine->thiran;
        for (std::size_t ch = 0; ch < kChannels; ++ch)
        {
            auto& state = engine->thiranState[ch];
            double* x = channels[ch];
            for (int i = 0; i < n; ++i)
            {
                const double y = a * x[i] + state[0] - a * state[1];
                state[0] = x[i];
                state[1] = y;
                x[i] = y;
            }
        }
    }

    /** The input of sample `i` of the last processed block, delayed by the latency (for dry/wet). */
    [[nodiscard]] double dry(int channel, int i) const noexcept
    {
        const int size = static_cast<int>(dry_[0].size());
        const int slot = ((dryStart_ + i - getLatency()) % size + size) % size;
        return dry_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(slot)];
    }

    void reset() noexcept
    {
        for (auto& row : engines_)
            for (auto& engine : row)
                if (engine.oversampling != nullptr)
                {
                    engine.oversampling->reset();
                    engine.thiranState.fill({});
                }
        for (auto& d : dry_)
            std::fill(d.begin(), d.end(), 0.0);
    }

private:
    struct Engine
    {
        std::unique_ptr<juce::dsp::Oversampling<double>> oversampling;
        int latency = 0;
        double thiran = 0.0;
        std::array<std::array<double, 2>, kChannels> thiranState{}; ///< x[n-1], y[n-1]
    };

    Engine* current() noexcept
    {
        if (factor_ <= 0)
            return nullptr;
        auto& engine = engines_[static_cast<std::size_t>(factor_ - 1)][static_cast<std::size_t>(phase_)];
        return engine.oversampling != nullptr ? &engine : nullptr;
    }

    std::array<std::array<Engine, kPhaseChoices>, kFactorChoices - 1> engines_;
    std::array<std::vector<double>, kChannels> dry_{std::vector<double>(2, 0.0), std::vector<double>(2, 0.0)};
    int maxBlock_ = 512;
    int dryWrite_ = 0, dryStart_ = 0;
    int factor_ = 0, phase_ = 2;
};

} // namespace dojo::devices::dsp

#pragma once

#include "devices/dsp/SynthCore.h"

#include <algorithm>
#include <cmath>

namespace dojo::devices::dsp
{

/** Filter responses of the analog-modelled filters. */
enum class FilterMode
{
    lowPass,
    highPass,
    bandPass,
    notch,
};

/**
    Circuit models:
    - clean: a linear state-variable filter (exactly the textbook response);
    - warm: an OTA-style state-variable filter whose resonance saturates (rounder, self-limiting);
    - ladder: a four-pole transistor ladder (tanh at every stage, 2x oversampled; 12 dB modes are
      taken from its second stage, high- and band-pass by mixing the stages, as on multimode ladders);
    - diode: an aggressive two-pole filter with an asymmetric diode clipper in the resonance loop
      (it screams and self-oscillates).
*/
enum class FilterCircuit
{
    clean,
    warm,
    ladder,
    diode,
};

/** Every output of a filter for one input sample. */
struct FilterOutputs
{
    double lowPass = 0.0, highPass = 0.0, bandPass = 0.0, notch = 0.0;

    [[nodiscard]] double get(FilterMode mode) const noexcept
    {
        switch (mode)
        {
        case FilterMode::lowPass: return lowPass;
        case FilterMode::highPass: return highPass;
        case FilterMode::bandPass: return bandPass;
        case FilterMode::notch: return notch;
        }
        return lowPass;
    }

    /** Continuous morph: 0 low-pass, 1/3 band-pass, 2/3 high-pass, 1 notch. */
    [[nodiscard]] double morph(double position) const noexcept
    {
        const double p = std::clamp(position, 0.0, 1.0) * 3.0;
        if (p < 1.0)
            return lowPass + (bandPass - lowPass) * p;
        if (p < 2.0)
            return bandPass + (highPass - bandPass) * (p - 1.0);
        return highPass + (notch - highPass) * (p - 2.0);
    }
};

/** Soft saturation used inside the filters (tanh, cheap rational form, slope 1 at 0). */
inline double softSaturate(double x) noexcept
{
    if (x > 3.0)
        return 1.0;
    if (x < -3.0)
        return -1.0;
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

/** Asymmetric diode-pair clipping (the positive side clips harder). */
inline double diodeClip(double x) noexcept
{
    return x >= 0.0 ? softSaturate(x * 1.3) / 1.3 : softSaturate(x * 0.75) / 0.75;
}

/** Two-pole state-variable filter (trapezoidal integration) with a choice of non-linearity. */
class NonlinearSvf final
{
public:
    /** `resonance` 0..1: 1 self-oscillates in the warm and diode circuits (clean stops just short). */
    void set(double cutoff, double resonance, double sampleRate, FilterCircuit circuit) noexcept
    {
        circuit_ = circuit;
        const double g = std::tan(kPi * std::clamp(cutoff, 8.0, sampleRate * 0.47) / sampleRate);
        resonance = std::clamp(resonance, 0.0, 1.0);
        switch (circuit)
        {
        case FilterCircuit::clean: k_ = 2.0 - 1.97 * resonance; break;
        case FilterCircuit::warm: k_ = 2.0 - 2.02 * resonance; break;
        case FilterCircuit::ladder:
        case FilterCircuit::diode: k_ = 2.0 - 2.1 * resonance; break;
        }
        a1_ = 1.0 / (1.0 + g * (g + k_));
        a2_ = g * a1_;
        a3_ = g * a2_;
    }

    FilterOutputs process(double x) noexcept
    {
        const double v3 = x - ic2_;
        const double v1 = a1_ * ic1_ + a2_ * v3;
        const double v2 = ic2_ + a2_ * ic1_ + a3_ * v3;
        ic1_ = 2.0 * v1 - ic1_;
        ic2_ = 2.0 * v2 - ic2_;
        if (circuit_ == FilterCircuit::warm)
            ic1_ = softSaturate(ic1_ * 0.5) * 2.0;
        else if (circuit_ != FilterCircuit::clean)
            ic1_ = diodeClip(ic1_ * 0.7) / 0.7;
        if (!std::isfinite(ic1_) || !std::isfinite(ic2_))
            ic1_ = ic2_ = 0.0;

        FilterOutputs out;
        out.lowPass = v2;
        out.bandPass = v1 * std::max(0.05, k_); // unity peak gain
        out.highPass = x - k_ * v1 - v2;
        out.notch = x - k_ * v1;
        return out;
    }

    void reset() noexcept { ic1_ = ic2_ = 0.0; }

private:
    FilterCircuit circuit_ = FilterCircuit::clean;
    double k_ = 2.0, a1_ = 1.0, a2_ = 0.0, a3_ = 0.0;
    double ic1_ = 0.0, ic2_ = 0.0;
};

/**
    Four-pole transistor ladder (after Huovilainen): a tanh in every stage, the feedback taken half a
    sample late and the whole filter run at twice the sample rate. Multimode outputs mix the stages.
*/
class LadderFilter final
{
public:
    struct Outputs
    {
        FilterOutputs pole2, pole4; ///< 12 dB and 24 dB families
    };

    void set(double cutoff, double resonance, double sampleRate) noexcept
    {
        const double fc = std::clamp(cutoff, 8.0, sampleRate * 0.45) / sampleRate;
        const double fc2 = fc * fc;
        const double fc3 = fc2 * fc;
        const double fcr = 1.8730 * fc3 + 0.4955 * fc2 - 0.6490 * fc + 0.9988;
        const double acr = -3.9364 * fc2 + 1.8409 * fc + 0.9968;
        tune_ = 1.0 - std::exp(-kTwoPi * fc * 0.5 * fcr); // (0.5: oversampled)
        feedback_ = 4.0 * std::clamp(resonance, 0.0, 1.05) * acr;
    }

    Outputs process(double x) noexcept
    {
        double u = 0.0;
        for (int pass = 0; pass < 2; ++pass)
        {
            u = x - feedback_ * delayed_;
            const double t0 = softSaturate(u);
            stage_[0] += tune_ * (t0 - tanh_[0]);
            tanh_[0] = softSaturate(stage_[0]);
            stage_[1] += tune_ * (tanh_[0] - tanh_[1]);
            tanh_[1] = softSaturate(stage_[1]);
            stage_[2] += tune_ * (tanh_[1] - tanh_[2]);
            tanh_[2] = softSaturate(stage_[2]);
            stage_[3] += tune_ * (tanh_[2] - softSaturate(stage_[3]));
            delayed_ = 0.5 * (stage_[3] + previous_);
            previous_ = stage_[3];
        }
        if (!std::isfinite(delayed_))
            reset();

        const double y1 = stage_[0], y2 = stage_[1], y3 = stage_[2], y4 = stage_[3];
        Outputs out;
        out.pole4.lowPass = y4;
        out.pole4.highPass = u - 4.0 * y1 + 6.0 * y2 - 4.0 * y3 + y4;
        out.pole4.bandPass = 4.0 * (y2 - 2.0 * y3 + y4);
        out.pole4.notch = u - out.pole4.bandPass; // (exactly zero at the cutoff)
        out.pole2.lowPass = y2;
        out.pole2.highPass = u - 2.0 * y1 + y2;
        out.pole2.bandPass = 2.0 * (y1 - y2);
        out.pole2.notch = u - 2.0 * (y1 - y2);
        // The ladder loses about half its passband gain to resonance: give some back.
        const double makeup = 1.0 + 0.5 * feedback_ / 4.0;
        for (auto* o : {&out.pole2, &out.pole4})
        {
            o->lowPass *= makeup;
            o->highPass *= makeup;
            o->bandPass *= makeup;
            o->notch *= makeup;
        }
        return out;
    }

    void reset() noexcept
    {
        for (auto& s : stage_)
            s = 0.0;
        for (auto& t : tanh_)
            t = 0.0;
        delayed_ = previous_ = 0.0;
    }

private:
    double tune_ = 0.5, feedback_ = 0.0;
    double stage_[4] = {0.0, 0.0, 0.0, 0.0};
    double tanh_[3] = {0.0, 0.0, 0.0};
    double delayed_ = 0.0, previous_ = 0.0;
};

/**
    One channel of an analog-modelled multimode filter: a response (or a morph between them), a 12 or
    24 dB slope and a circuit. Set at control rate, process per sample.
*/
class CharacterFilter final
{
public:
    void set(double cutoff, double resonance, double sampleRate, FilterCircuit circuit, bool steep) noexcept
    {
        circuit_ = circuit;
        steep_ = steep;
        if (circuit == FilterCircuit::ladder)
        {
            ladder_.set(cutoff, resonance, sampleRate);
            return;
        }
        first_.set(cutoff, resonance, sampleRate, circuit);
        // The second pole pair of the 24 dB slope is barely resonant: one peak, not two.
        second_.set(cutoff, resonance * 0.2, sampleRate, circuit == FilterCircuit::diode ? FilterCircuit::warm : circuit);
    }

    /** `morph` < 0: the plain `mode`; 0..1: a morph from low-pass through band-pass and high-pass to notch. */
    double process(double x, FilterMode mode, double morph = -1.0) noexcept
    {
        if (circuit_ == FilterCircuit::ladder)
        {
            const auto o = ladder_.process(x);
            const auto& family = steep_ ? o.pole4 : o.pole2;
            return morph >= 0.0 ? family.morph(morph) : family.get(mode);
        }
        const auto a = first_.process(x);
        const double y = morph >= 0.0 ? a.morph(morph) : a.get(mode);
        if (!steep_)
            return y;
        const auto b = second_.process(y);
        return morph >= 0.0 ? b.morph(morph) : b.get(mode);
    }

    void reset() noexcept
    {
        first_.reset();
        second_.reset();
        ladder_.reset();
    }

private:
    FilterCircuit circuit_ = FilterCircuit::clean;
    bool steep_ = false;
    NonlinearSvf first_, second_;
    LadderFilter ladder_;
};

} // namespace dojo::devices::dsp

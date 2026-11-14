#pragma once

#include "devices/dsp/SynthCore.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cmath>
#include <cstdint>

namespace dojo::devices::dsp
{

//==================================================================================================
/** PolyBLEP residual: smooths a unit step at phase 0 (t: phase 0..1, dt: increment per sample). */
inline double polyBlep(double t, double dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}

/** Band-limited (PolyBLEP) analog-style oscillator. Phase 0..1. */
class BlepOscillator final
{
public:
    enum class Wave
    {
        sine,
        triangle,
        saw,
        pulse,
    };

    void reset(double phase = 0.0) noexcept
    {
        phase_ = phase - std::floor(phase);
        wrapped_ = false;
    }

    /** One sample at `dt` cycles per sample (pulse width 0..1 for pulses); advances the phase. */
    double next(Wave wave, double dt, double width = 0.5) noexcept
    {
        dt = std::clamp(dt, 0.0, 0.49);
        const double p = phase_;
        double y = 0.0;
        switch (wave)
        {
        case Wave::sine: y = std::sin(kTwoPi * p); break;
        case Wave::triangle: y = p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p; break;
        case Wave::saw: y = 2.0 * p - 1.0 - polyBlep(p, dt); break;
        case Wave::pulse:
        {
            const double w = std::clamp(width, 0.02, 0.98);
            y = (p < w ? 1.0 : -1.0) + polyBlep(p, dt);
            double q = p - w;
            if (q < 0.0)
                q += 1.0;
            y -= polyBlep(q, dt);
            y += 1.0 - 2.0 * w; // (no DC at any width)
            break;
        }
        }
        phase_ += dt;
        wrapped_ = phase_ >= 1.0;
        if (wrapped_)
            phase_ -= 1.0;
        return y;
    }

    /** Hard sync: restart where the master wrapped (`masterPhase` is how far past its wrap it is,
        `ratio` this oscillator's increment over the master's). */
    void sync(double masterPhase, double masterDt, double dt) noexcept
    {
        phase_ = std::clamp(masterPhase / std::max(1.0e-9, masterDt), 0.0, 1.0) * dt;
    }

    [[nodiscard]] double getPhase() const noexcept { return phase_; }
    [[nodiscard]] bool justWrapped() const noexcept { return wrapped_; }

private:
    double phase_ = 0.0;
    bool wrapped_ = false;
};

//==================================================================================================
/**
    Per-note expression for polyphonic and MPE instruments.
    - Off: the pitch wheel, channel pressure and CC 74 (slide) of any channel apply to every note;
      polyphonic aftertouch applies to its note.
    - MPE (lower zone): channel 1 is the master (its bend, range `masterBendRange`, moves every
      note); each note on channels 2..16 has its own bend (`memberBendRange`, 48 semitones by
      default), pressure and slide.
*/
class NoteExpressions final
{
public:
    void setMpe(bool on, double memberBendRange, double masterBendRange) noexcept
    {
        mpe_ = on;
        memberRange_ = memberBendRange;
        masterRange_ = masterBendRange;
    }

    void reset() noexcept
    {
        bend_.fill(0.0);
        pressure_.fill(0.0);
        slide_.fill(0.0);
        polyPressure_.fill(0.0);
        modWheel_ = 0.0;
    }

    /** Takes pitch bend, pressure, CC 1 and CC 74; true if `m` was one of them. */
    bool handle(const juce::MidiMessage& m) noexcept
    {
        const auto ch = static_cast<std::size_t>(std::clamp(m.getChannel(), 1, 16));
        if (m.isPitchWheel())
        {
            const double value = (m.getPitchWheelValue() - 8192) / 8192.0;
            bend_[ch] = value;
            bend_[0] = value;
            return true;
        }
        if (m.isChannelPressure())
        {
            const double value = m.getChannelPressureValue() / 127.0;
            pressure_[ch] = value;
            pressure_[0] = value;
            return true;
        }
        if (m.isAftertouch())
        {
            polyPressure_[static_cast<std::size_t>(m.getNoteNumber() & 127)] = m.getAfterTouchValue() / 127.0;
            return true;
        }
        if (m.isController() && m.getControllerNumber() == 74)
        {
            const double value = m.getControllerValue() / 127.0;
            slide_[ch] = value;
            slide_[0] = value;
            return true;
        }
        if (m.isController() && m.getControllerNumber() == 1)
        {
            modWheel_ = m.getControllerValue() / 127.0;
            return true;
        }
        return false;
    }

    /** A new note on `channel`: MPE controllers send its starting expression before the note, so
        nothing is reset here; poly pressure starts at zero. */
    void noteOn(int note) noexcept { polyPressure_[static_cast<std::size_t>(note & 127)] = 0.0; }

    [[nodiscard]] double bend(int channel) const noexcept
    {
        if (!mpe_)
            return bend_[0] * masterRange_;
        const double master = bend_[1] * masterRange_;
        return channel == 1 ? master : master + bend_[index(channel)] * memberRange_;
    }
    [[nodiscard]] double pressure(int channel, int note) const noexcept
    {
        const double poly = polyPressure_[static_cast<std::size_t>(note & 127)];
        return std::max(poly, mpe_ ? std::max(pressure_[index(channel)], pressure_[1]) : pressure_[0]);
    }
    [[nodiscard]] double slide(int channel) const noexcept { return mpe_ ? slide_[index(channel)] : slide_[0]; }
    [[nodiscard]] double modWheel() const noexcept { return modWheel_; }
    [[nodiscard]] bool isMpe() const noexcept { return mpe_; }

private:
    static std::size_t index(int channel) noexcept { return static_cast<std::size_t>(std::clamp(channel, 1, 16)); }

    bool mpe_ = false;
    double memberRange_ = 48.0, masterRange_ = 2.0;
    std::array<double, 17> bend_{}, pressure_{}, slide_{}; ///< [0] = the last of any channel
    std::array<double, 128> polyPressure_{};
    double modWheel_ = 0.0;
};

//==================================================================================================
/** The held keys of a monophonic instrument (last note priority). */
class NoteStack final
{
public:
    struct Key
    {
        int note = 0, channel = 1;
        double velocity = 0.0;
    };

    void clear() noexcept { size_ = 0; }
    void push(int note, int channel, double velocity) noexcept
    {
        remove(note, channel);
        if (size_ == static_cast<int>(keys_.size()))
        {
            for (int i = 1; i < size_; ++i)
                keys_[static_cast<std::size_t>(i - 1)] = keys_[static_cast<std::size_t>(i)];
            --size_;
        }
        keys_[static_cast<std::size_t>(size_++)] = {note, channel, velocity};
    }
    void remove(int note, int channel) noexcept
    {
        for (int i = 0; i < size_; ++i)
            if (keys_[static_cast<std::size_t>(i)].note == note && keys_[static_cast<std::size_t>(i)].channel == channel)
            {
                for (int j = i + 1; j < size_; ++j)
                    keys_[static_cast<std::size_t>(j - 1)] = keys_[static_cast<std::size_t>(j)];
                --size_;
                return;
            }
    }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] const Key& top() const noexcept { return keys_[static_cast<std::size_t>(std::max(0, size_ - 1))]; }

private:
    std::array<Key, 32> keys_{};
    int size_ = 0;
};

/**
    The voice for a new note among the first `count`: the voice already playing the note on that
    channel (retrigger), a free one, the oldest released one, or the oldest. `V` needs active, note,
    channel and age; `releasing(v)` says whether a voice is in its release.
*/
template <typename V, std::size_t N, typename Releasing>
V& pickVoice(std::array<V, N>& voices, int count, int note, int channel, Releasing&& releasing) noexcept
{
    count = std::clamp(count, 1, static_cast<int>(N));
    V* chosen = nullptr;
    for (int i = 0; i < count; ++i)
    {
        auto& v = voices[static_cast<std::size_t>(i)];
        if (v.active && v.note == note && v.channel == channel)
            return v;
        if (!v.active && chosen == nullptr)
            chosen = &v;
    }
    if (chosen != nullptr)
        return *chosen;
    for (int i = 0; i < count; ++i)
    {
        auto& v = voices[static_cast<std::size_t>(i)];
        if (releasing(v) && (chosen == nullptr || v.age < chosen->age))
            chosen = &v;
    }
    if (chosen != nullptr)
        return *chosen;
    chosen = &voices[0];
    for (int i = 1; i < count; ++i)
        if (voices[static_cast<std::size_t>(i)].age < chosen->age)
            chosen = &voices[static_cast<std::size_t>(i)];
    return *chosen;
}

} // namespace dojo::devices::dsp

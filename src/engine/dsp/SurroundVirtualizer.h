#pragma once

#include "core/Types.h"
#include "engine/ChannelLayout.h"

#include <array>
#include <vector>

namespace dojo::engine::dsp
{

/**
    The advanced surround virtualizer: a multichannel mix on two ears (headphones) or two speakers.

    Every loudspeaker of the layout is a virtual source at its standard place (ITU-R BS.775 / Dolby:
    L/R at +-30 deg, C at 0, surrounds at +-110 deg in 5.1 and +-90 deg in 7.1, rears at +-150 deg),
    2 m away in a room 5 m x 6 m. Each source reaches each ear along four paths - the direct sound
    and its first reflections from the floor, the nearest side wall and the front or back wall
    (image sources) - and every path is rendered with the spherical-head model of Brown and Duda
    (1998), which matches measured HRTFs well in the horizontal plane:
      - interaural time difference: tau(theta) = -(a/c) cos(theta) up to 90 deg from the ear,
        (a/c)(theta - pi/2) beyond (Woodworth), head radius a = 8.75 cm, c = 343 m/s, applied as
        a fractional delay (third-order Lagrange)
      - head shadow: H(s) = (alpha(theta) s + beta) / (s + beta), beta = 2c/a, with
        alpha(theta) = (1 + alpha_min/2) + (1 - alpha_min/2) cos(theta / theta_min * pi),
        alpha_min = 0.1, theta_min = 150 deg (+6 dB of treble facing the ear, -20 dB in its shadow),
        bilinear-transformed
    plus a pinna cue for sources behind (a high shelf, down to -8 dB at 4 kHz directly behind),
    and a short late reverberation (a four-line feedback delay network, 0.35 s) for externalisation.
    The LFE plays on both sides below 120 Hz. The level matches the ITU fold-down's for a full mix.

    On speakers, the binaural signal goes through a recursive crosstalk canceller (RACE) for a
    listener in the sweet spot of speakers at +-30 deg: each side subtracts the other's output,
    delayed by the head's path difference (0.26 ms) and band-limited to 250 Hz - 6 kHz.

    configure() and process() are real-time safe; prepare() allocates.
*/
class SurroundVirtualizer
{
public:
    enum class Output
    {
        headphones,
        speakers,
    };

    static constexpr double kHeadRadius = 0.0875;  ///< metres
    static constexpr double kSpeedOfSound = 343.0; ///< metres per second

    void prepare(double sampleRate);
    /** The layout of the signal and how to render it. `room` 0..1 scales the reflections and the
        reverberation (0: anechoic). */
    void configure(const ChannelLayout& layout, Output output, double room) noexcept;
    void reset() noexcept;
    /** `in[c]` feeds the layout's speaker c (numChannels of them). */
    void process(const Sample* const* in, int numChannels, Sample* left, Sample* right, int numSamples) noexcept;

    [[nodiscard]] const ChannelLayout& getLayout() const noexcept { return layout_; }
    [[nodiscard]] Output getOutput() const noexcept { return output_; }
    [[nodiscard]] double getRoom() const noexcept { return room_; }
    [[nodiscard]] double getSampleRate() const noexcept { return sampleRate_; }

    // The geometry and the head model (for tests and the documentation).
    /** A speaker's azimuth in degrees (0 ahead, positive to the right). */
    [[nodiscard]] static double azimuthOf(Speaker speaker, bool sevenOne) noexcept;
    /** The angle (radians, 0..pi) between a source at `azimuthDegrees` and an ear (`rightEar`). */
    [[nodiscard]] static double incidence(double azimuthDegrees, bool rightEar) noexcept;
    /** The Brown-Duda delay (seconds, >= 0) of an ear at incidence `theta`. */
    [[nodiscard]] static double earDelay(double theta) noexcept;
    /** The head-shadow alpha at incidence `theta` (2 facing the ear, 0.1 at 150 deg). */
    [[nodiscard]] static double shadowAlpha(double theta) noexcept;
    /** The head-shadow filter's gain (linear) at `frequency` for incidence `theta` (analog prototype). */
    [[nodiscard]] static double shadowGain(double theta, double frequency) noexcept;

private:
    static constexpr int kMaxSources = kMaxChannels;
    static constexpr int kPaths = 4; ///< direct, floor, side wall, front/back wall
    static constexpr int kLateLines = 4;

    struct Biquad
    {
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;
        double process(double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
        void clear() noexcept { z1 = z2 = 0.0; }
    };

    struct EarPath
    {
        double delay = 0.0; ///< samples
        double gain = 0.0;
        double b0 = 1.0, b1 = 0.0, a1 = 0.0; ///< head shadow
        double x1 = 0.0, y1 = 0.0;
    };

    struct Source
    {
        bool active = false;
        bool lfe = false;
        Biquad pinna; ///< rear high shelf
        Biquad lfeLow1, lfeLow2;
        std::vector<double> line; ///< delay line (power-of-two size)
        std::array<std::array<EarPath, 2>, kPaths> paths{};
    };

    void setHeadShadow(EarPath& path, double theta) const noexcept;
    [[nodiscard]] double readDelayed(const std::vector<double>& line, int write, double delay) const noexcept;

    double sampleRate_ = 48000.0;
    ChannelLayout layout_;
    Output output_ = Output::headphones;
    double room_ = 0.5;
    double normalise_ = 1.0;
    int mask_ = 0;
    int write_ = 0;
    std::array<Source, kMaxSources> sources_{};

    // Late reverberation.
    std::array<std::vector<double>, kLateLines> late_{};
    std::array<int, kLateLines> lateLength_{};
    std::array<double, kLateLines> lateGain_{};
    std::array<double, kLateLines> lateDamp_{};
    std::array<int, kLateLines> lateWrite_{};
    double lateMix_ = 0.0;
    double dampCoefficient_ = 0.0;

    // Crosstalk cancellation (speakers).
    std::vector<double> xtcLeft_, xtcRight_;
    int xtcWrite_ = 0;
    int xtcMask_ = 0;
    double xtcDelay_ = 0.0;
    double xtcGain_ = 0.88;
    Biquad xtcHighL_, xtcLowL_, xtcHighR_, xtcLowR_;
};

/** The speakers a device with `numOutputs` outputs (2, 4, 6 or 8) carries, in its channel order:
    the WAVE/WASAPI order (L R C LFE BL BR SL SR) or, for ALSA, its own (L R RL RR C LFE SL SR). */
[[nodiscard]] ChannelLayout deviceLayout(int numOutputs, bool alsaOrder) noexcept;

} // namespace dojo::engine::dsp

#pragma once

#include "core/Types.h"
#include "engine/ChannelLayout.h"

#include <array>
#include <vector>

namespace dojo::engine::dsp
{

/**
    The advanced surround virtualizer: a multichannel mix, its heights included, on two ears
    (headphones) or two speakers.

    Every loudspeaker of the layout is a virtual source at its standard place, 2 m away in a room
    5 m wide, 6 m long and 2.7 m high, the ears 1.2 m above the floor:
      - at ear level (ITU-R BS.775 / Dolby): L/R at +-30 deg, C at 0, the wides at +-60, the
        surrounds at +-110 in 5.1 and 5.1.4 and +-90 (the sides) in the 7.x family, the rears at +-150;
      - the top layer 30 deg up (ITU-R BS.2051, as the ADM common definitions place it): the top
        fronts (Ltf/Rtf) at +-30 in 5.1.4 and +-45 in 7.1.4 and 9.1.6, the top middles at +-90, the
        top rears (Ltr/Rtr) at +-110 in 5.1.4 and +-135 otherwise.
    Each source reaches each ear along five paths: the direct sound and its first reflections from
    the floor, the ceiling, the nearest side wall and the front or back wall (image sources, each
    arriving from its own direction: a top speaker's ceiling reflection comes early and steeply
    from above, its floor reflection late and from far below). Every path is rendered with
      - the spherical-head model of Brown and Duda (1998), with the angle between the path and each
        ear taken in three dimensions (a source above is less lateral: smaller time and level
        differences, as on a real head):
          - interaural time difference: tau(theta) = -(a/c) cos(theta) up to 90 deg from the ear,
            (a/c)(theta - pi/2) beyond (Woodworth), head radius a = 8.75 cm, c = 343 m/s, applied
            as a fractional delay (third-order Lagrange);
          - head shadow: H(s) = (alpha(theta) s + beta) / (s + beta), beta = 2c/a, with
            alpha(theta) = (1 + alpha_min/2) + (1 - alpha_min/2) cos(theta / theta_min * pi),
            alpha_min = 0.1, theta_min = 150 deg (+6 dB of treble facing the ear, -20 dB in its
            shadow), bilinear-transformed;
      - a pinna model of the spectral cues for elevation and front/back (the notches and peaks of
        a parametric HRTF, after Hebrank and Wright 1974 and Iida et al. 2007):
          - the first pinna notch, which rises with elevation, log-linearly from 4 kHz straight
            below through 6.9 kHz at ear level and 8.3 kHz at the top layer to 12 kHz straight
            above (f = 4 kHz * 3^((elevation + 90) / 180)), 12 dB deep (Q 4) up to 30 deg and
            fading out towards the zenith;
          - the band around 8 kHz that marks a source above (Blauert's directional band), raised
            by 4 dB * sin(elevation);
          - for sources behind, a high shelf down to -8 dB at 4 kHz directly behind;
        the notch and the overhead band are made power-neutral over the audio band (a makeup
        gain), so that they change the spectrum of a path, not its level.
    A short late reverberation (a four-line feedback delay network, 0.35 s) adds externalisation.
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

    static constexpr double kHeadRadius = 0.0875;   ///< metres
    static constexpr double kSpeedOfSound = 343.0;  ///< metres per second
    static constexpr double kSpeakerDistance = 2.0; ///< metres from the listener
    static constexpr double kEarHeight = 1.2;       ///< metres above the floor
    static constexpr double kRoomHeight = 2.7;      ///< floor to ceiling
    static constexpr double kRoomWidth = 5.0;       ///< side walls at +-2.5 m
    static constexpr double kRoomLength = 6.0;      ///< front and back walls at +-3 m
    static constexpr int kPaths = 5;                ///< direct, floor, ceiling, side wall, front/back wall

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
    /** A speaker's elevation in degrees (0 at ear level, 30 for the top layer). */
    [[nodiscard]] static double elevationOf(Speaker speaker) noexcept;
    /** The angle (radians, 0..pi) between a source at ear level at `azimuthDegrees` and an ear. */
    [[nodiscard]] static double incidence(double azimuthDegrees, bool rightEar) noexcept;
    /** The angle (radians, 0..pi) between a source in any direction and an ear. */
    [[nodiscard]] static double incidence(double azimuthDegrees, double elevationDegrees, bool rightEar) noexcept;
    /** The Brown-Duda delay (seconds, >= 0) of an ear at incidence `theta`. */
    [[nodiscard]] static double earDelay(double theta) noexcept;
    /** The head-shadow alpha at incidence `theta` (2 facing the ear, 0.1 at 150 deg). */
    [[nodiscard]] static double shadowAlpha(double theta) noexcept;
    /** The head-shadow filter's gain (linear) at `frequency` for incidence `theta` (analog prototype). */
    [[nodiscard]] static double shadowGain(double theta, double frequency) noexcept;

    /** The pinna's spectral cues for a direction. */
    struct PinnaCue
    {
        double notchFrequency = 0.0; ///< Hz: the first pinna notch
        double notchDepth = 0.0;     ///< dB (how deep)
        double overheadGain = 0.0;   ///< dB around 8 kHz (sources above)
        double rearShelf = 0.0;      ///< dB above 4 kHz (negative behind)
    };
    [[nodiscard]] static PinnaCue pinnaCue(double azimuthDegrees, double elevationDegrees) noexcept;
    /** The pinna filters' gain (dB) at `frequency` for a direction, as built at `sampleRate` (their
        makeup gain included). */
    [[nodiscard]] static double pinnaGain(double azimuthDegrees, double elevationDegrees, double frequency, double sampleRate) noexcept;

    /** One way a speaker's sound reaches the listener: from a direction (degrees), along a path
        (metres), at a level (relative to the direct sound's). */
    struct Arrival
    {
        double azimuth = 0.0;
        double elevation = 0.0;
        double distance = 0.0;
        double gain = 0.0;
    };
    /** The direct sound and the first reflections from the floor, the ceiling, the nearest side
        wall and the front or back wall (in that order) of a speaker in a direction, the
        reflections scaled by `room` (0..1). */
    [[nodiscard]] static std::array<Arrival, kPaths> arrivals(double azimuthDegrees, double elevationDegrees, double room) noexcept;

private:
    static constexpr int kMaxSources = kMaxChannels;
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

    /** A path to one ear: its delay (a third-order Lagrange read) and the head shadow. */
    struct EarPath
    {
        double delay = 0.0;           ///< samples
        int back = 0;                 ///< the newest of the four samples read, this many back
        std::array<double, 4> taps{}; ///< their weights
        double b0 = 1.0, b1 = 0.0, a1 = 0.0;
        double x1 = 0.0, y1 = 0.0;
    };

    /** A path from a source: its level, its pinna filters and its delay line (both ears read it). */
    struct Path
    {
        double gain = 0.0;   ///< 0: not rendered (a reflection, with no room)
        double makeup = 1.0; ///< keeps the notch and the overhead band power-neutral
        double level = 0.0;  ///< gain * makeup
        Biquad notch, overhead, shelf;
        bool hasNotch = false, hasOverhead = false, hasShelf = false;
        std::vector<double> line; ///< the filtered signal (power-of-two size)
        std::array<EarPath, 2> ears{};
    };

    struct Source
    {
        bool active = false;
        bool lfe = false;
        Biquad lfeLow1, lfeLow2;
        std::array<Path, kPaths> paths{};
    };

    void setHeadShadow(EarPath& path, double theta) const noexcept;
    void setDelay(EarPath& path, double delay) const noexcept;
    void setPinna(Path& path, double azimuth, double elevation) const noexcept;
    static void clear(Path& path) noexcept;

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

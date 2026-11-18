#pragma once

#include "core/Types.h"
#include "engine/ChannelLayout.h"

#include <array>
#include <utility>
#include <vector>

namespace dojo::engine
{

/** A direction or position: x right, y front, z up (the ITU-R BS.2076 axes). */
struct Vec3
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/** The unit vector of a direction: azimuth in degrees, positive to the left (0 ahead, +-180
    behind); elevation in degrees, positive up. */
[[nodiscard]] Vec3 directionVector(double azimuthDegrees, double elevationDegrees) noexcept;
/** The azimuth and elevation (degrees) of a vector (not necessarily of unit length). */
[[nodiscard]] std::pair<double, double> directionAngles(const Vec3& v) noexcept;

/**
    3D vector-base amplitude panning (VBAP, after Pulkki) over the speakers of a layout.

    The speakers (all but the LFE) are unit vectors on the listener's sphere. Their convex hull is a
    mesh of non-overlapping triangles joining neighbouring speakers, heights included. A source in
    direction p plays from the three speakers l1, l2, l3 of the triangle around it, with the gains
    that solve p = g1 l1 + g2 l2 + g3 l3 (g = L^-1 p, L's inverse worked out once per triangle),
    normalised to constant power: g1^2 + g2^2 + g3^2 = 1.

    Where the speakers leave the sphere open, or would pan lopsided, the mesh gets virtual speakers,
    whose signal is shared out to real ones at equal power:
      - a flat layout (5.1, 7.1: no heights) gets one straight up and one straight down, shared by
        the ear-level ring;
      - a face of more than three speakers in one plane (a 3D bed's ear-level ring seen from below,
        or a symmetric quad of two ear-level and two top speakers) gets one in its middle, shared by
        that face's speakers, so a symmetric layout pans symmetrically;
      - a gap of more than 160 degrees in the ear-level ring (3.1: nothing behind) gets virtual ring
        speakers, each shared by the two real speakers either side of the gap (cos/sin by where it
        lies in the gap).
    Mono plays everything from its one speaker; stereo pans along the L/R arc.

    Spread (the source's size, and its distance: a closer source surrounds you more) uses
    energy-preserving multiple-direction panning: the powers of the gains of directions sampled over
    a cap around the source (up to the whole sphere) are summed, so the source widens smoothly and
    keeps its loudness.

    Configure on the message thread (it allocates); the gain functions are real-time safe.
*/
class SpatialPanner
{
public:
    SpatialPanner() = default;
    explicit SpatialPanner(const ChannelLayout& layout) { configure(layout); }

    void configure(const ChannelLayout& layout);
    [[nodiscard]] const ChannelLayout& getLayout() const noexcept { return layout_; }

    using Gains = std::array<double, kMaxChannels>;

    /** Power-normalised gains (one per channel of the layout; the LFE gets 0) for a point source. */
    [[nodiscard]] Gains pointGains(double azimuthDegrees, double elevationDegrees) const noexcept;
    /** A source spread over the directions up to `spread` x 180 degrees around its own (0: a point,
        1: the whole sphere), with equal power. */
    [[nodiscard]] Gains spreadGains(double azimuthDegrees, double elevationDegrees, double spread) const noexcept;

    /** The spread a source has at `distance` (0 at the listener .. 1 at the speakers) with its own
        `size` (0..1): a source moving in surrounds you, all the way at the centre. */
    [[nodiscard]] static double effectiveSpread(double size, double distance) noexcept;

    // The mesh (tests, and the 3D panner's speaker view).
    struct Vertex
    {
        Vec3 direction;
        int channel = -1;                          ///< the layout's channel, or -1 (virtual)
        std::vector<std::pair<int, double>> shares; ///< a virtual speaker's channels and gains
    };
    struct Triangle
    {
        std::array<int, 3> vertex{};
        std::array<std::array<double, 3>, 3> inverse{}; ///< L^-1, L's columns the three directions
    };
    [[nodiscard]] const std::vector<Vertex>& getVertices() const noexcept { return vertices_; }
    [[nodiscard]] const std::vector<Triangle>& getTriangles() const noexcept { return triangles_; }

private:
    enum class Mode
    {
        none,
        single,
        pair,
        mesh,
    };

    void addPointPower(const Vec3& direction, double weight, Gains& power) const noexcept;
    void meshGains(const Vec3& p, Gains& gains) const noexcept;

    ChannelLayout layout_;
    Mode mode_ = Mode::none;
    int single_ = -1;          ///< mono: its channel
    int left_ = -1, right_ = -1; ///< stereo: the pair's channels
    std::vector<Vertex> vertices_;
    std::vector<Triangle> triangles_;
};

/**
    Decorrelation filters, one per output channel, for a source's diffuse part (its size): each is a
    cascade of four Schroeder all-pass sections with delays of its own (1 to 12 ms), so every
    channel's copy keeps the signal's spectrum (all-pass: flat magnitude) while the copies are
    mutually uncorrelated. A spread source made of them sounds wide and enveloping instead of a
    phantom image between speakers.

    prepare() allocates (message thread); process() is real-time safe.
*/
class Decorrelator
{
public:
    void prepare(double sampleRate, int numChannels);
    void reset() noexcept;
    [[nodiscard]] int getNumChannels() const noexcept { return numChannels_; }
    /** Filters `numSamples` of `input` into `output` (may be the same buffer) for `channel`. */
    void process(int channel, const Sample* input, Sample* output, int numSamples) noexcept;

private:
    static constexpr int kSections = 4;
    struct Section
    {
        std::vector<Sample> buffer;
        int write = 0;
        double gain = 0.5;
    };
    std::array<std::array<Section, kSections>, static_cast<std::size_t>(kMaxChannels)> sections_{};
    int numChannels_ = 0;
};

} // namespace dojo::engine

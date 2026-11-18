#include "engine/SpatialPanner.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace dojo::engine
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

Vec3 operator-(const Vec3& a, const Vec3& b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator+(const Vec3& a, const Vec3& b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator*(const Vec3& a, double s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
double dot(const Vec3& a, const Vec3& b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) noexcept
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double length(const Vec3& a) noexcept { return std::sqrt(dot(a, a)); }
Vec3 normalised(const Vec3& a) noexcept
{
    const double l = length(a);
    return l > 0.0 ? a * (1.0 / l) : Vec3{0.0, 1.0, 0.0};
}

/** The inverse of the matrix whose columns are a, b, c (zero if it is singular). */
std::array<std::array<double, 3>, 3> inverseOfColumns(const Vec3& a, const Vec3& b, const Vec3& c) noexcept
{
    // Rows of the inverse are the cross products of the other two columns over the determinant.
    const double det = dot(a, cross(b, c));
    std::array<std::array<double, 3>, 3> inverse{};
    if (std::abs(det) < 1.0e-12)
        return inverse;
    const auto r0 = cross(b, c) * (1.0 / det);
    const auto r1 = cross(c, a) * (1.0 / det);
    const auto r2 = cross(a, b) * (1.0 / det);
    inverse[0] = {r0.x, r0.y, r0.z};
    inverse[1] = {r1.x, r1.y, r1.z};
    inverse[2] = {r2.x, r2.y, r2.z};
    return inverse;
}

void normalisePower(SpatialPanner::Gains& gains, int numChannels) noexcept
{
    double power = 0.0;
    for (int c = 0; c < numChannels; ++c)
        power += gains[static_cast<std::size_t>(c)] * gains[static_cast<std::size_t>(c)];
    if (power <= 0.0)
        return;
    const double scale = 1.0 / std::sqrt(power);
    for (int c = 0; c < numChannels; ++c)
        gains[static_cast<std::size_t>(c)] *= scale;
}
} // namespace

Vec3 directionVector(double azimuthDegrees, double elevationDegrees) noexcept
{
    const double a = azimuthDegrees * kPi / 180.0;
    const double e = elevationDegrees * kPi / 180.0;
    return {-std::sin(a) * std::cos(e), std::cos(a) * std::cos(e), std::sin(e)};
}

std::pair<double, double> directionAngles(const Vec3& v) noexcept
{
    const double horizontal = std::hypot(v.x, v.y);
    const double azimuth = horizontal > 1.0e-12 ? std::atan2(-v.x, v.y) * 180.0 / kPi : 0.0;
    const double elevation = std::atan2(v.z, horizontal) * 180.0 / kPi;
    return {azimuth, elevation};
}

// --- The mesh ------------------------------------------------------------------------------------

void SpatialPanner::configure(const ChannelLayout& layout)
{
    layout_ = layout;
    vertices_.clear();
    triangles_.clear();
    mode_ = Mode::none;
    single_ = left_ = right_ = -1;

    std::vector<int> channels;
    for (int c = 0; c < layout.numChannels; ++c)
        if (layout.speakers[static_cast<std::size_t>(c)] != Speaker::lfe)
            channels.push_back(c);
    if (channels.empty())
        return;
    if (channels.size() == 1)
    {
        mode_ = Mode::single;
        single_ = channels.front();
        return;
    }
    if (channels.size() == 2)
    {
        mode_ = Mode::pair;
        const bool firstIsLeft = directionOf(layout, layout.speakers[static_cast<std::size_t>(channels[0])]).azimuth
                                 >= directionOf(layout, layout.speakers[static_cast<std::size_t>(channels[1])]).azimuth;
        left_ = firstIsLeft ? channels[0] : channels[1];
        right_ = firstIsLeft ? channels[1] : channels[0];
        return;
    }

    mode_ = Mode::mesh;
    for (const int c : channels)
    {
        const auto d = directionOf(layout, layout.speakers[static_cast<std::size_t>(c)]);
        vertices_.push_back({directionVector(d.azimuth, d.elevation), c, {}});
    }

    // The ear-level ring, by azimuth; gaps of more than 160 degrees get virtual ring speakers.
    struct RingSpeaker
    {
        double azimuth;
        int channel;
    };
    std::vector<RingSpeaker> ring;
    for (const auto& v : vertices_)
    {
        const auto [azimuth, elevation] = directionAngles(v.direction);
        if (std::abs(elevation) < 1.0e-6)
            ring.push_back({azimuth, v.channel});
    }
    std::sort(ring.begin(), ring.end(), [](const RingSpeaker& a, const RingSpeaker& b) { return a.azimuth < b.azimuth; });
    for (std::size_t i = 0; i < ring.size(); ++i)
    {
        const auto& a = ring[i];
        const auto& b = ring[(i + 1) % ring.size()];
        const double gap = ring.size() == 1 ? 360.0 : (i + 1 == ring.size() ? b.azimuth + 360.0 - a.azimuth : b.azimuth - a.azimuth);
        if (gap <= 160.0)
            continue;
        const int count = static_cast<int>(std::ceil(gap / 90.0)) - 1;
        for (int k = 1; k <= count; ++k)
        {
            const double t = static_cast<double>(k) / (count + 1);
            Vertex filler{directionVector(a.azimuth + gap * t, 0.0), -1, {}};
            filler.shares.emplace_back(a.channel, std::cos(t * kPi * 0.5));
            if (b.channel != a.channel)
                filler.shares.emplace_back(b.channel, std::sin(t * kPi * 0.5));
            vertices_.push_back(std::move(filler));
        }
    }

    // Nothing above (a flat layout: 5.1, 7.1) or below (every 3D bed): a virtual speaker straight
    // up or down, shared by the ear-level ring. (A top layer makes its own: the middle of its face.)
    const bool anyAbove = std::any_of(vertices_.begin(), vertices_.end(), [](const Vertex& v) { return v.direction.z > 1.0e-6; });
    const bool anyBelow = std::any_of(vertices_.begin(), vertices_.end(), [](const Vertex& v) { return v.direction.z < -1.0e-6; });
    if (!anyAbove || !anyBelow)
    {
        std::vector<std::pair<int, double>> shares;
        std::vector<int> sharing;
        for (const auto& r : ring)
            sharing.push_back(r.channel);
        if (sharing.empty())
            sharing = channels;
        for (const int c : sharing)
            shares.emplace_back(c, 1.0 / std::sqrt(static_cast<double>(sharing.size())));
        if (!anyAbove)
            vertices_.push_back({{0.0, 0.0, 1.0}, -1, shares});
        if (!anyBelow)
            vertices_.push_back({{0.0, 0.0, -1.0}, -1, shares});
    }

    // The convex hull: every plane through three speakers with all the others on one side (and
    // the listener inside) is a face. A face may hold more than three speakers (all in its plane).
    struct Face
    {
        Vec3 normal;
        double offset;
        std::vector<int> members;
    };
    std::vector<Face> faces;
    const int n = static_cast<int>(vertices_.size());
    constexpr double kTolerance = 1.0e-7;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
            for (int k = j + 1; k < n; ++k)
            {
                const auto& a = vertices_[static_cast<std::size_t>(i)].direction;
                const auto& b = vertices_[static_cast<std::size_t>(j)].direction;
                const auto& c = vertices_[static_cast<std::size_t>(k)].direction;
                auto normal = cross(b - a, c - a);
                const double l = length(normal);
                if (l < 1.0e-9)
                    continue;
                normal = normal * (1.0 / l);
                double offset = dot(normal, a);
                if (offset < 0.0)
                {
                    normal = normal * -1.0;
                    offset = -offset;
                }
                if (offset < 1.0e-9)
                    continue; // (the plane passes through the listener)
                bool isFace = true;
                for (int m = 0; m < n && isFace; ++m)
                    isFace = dot(normal, vertices_[static_cast<std::size_t>(m)].direction) - offset <= kTolerance;
                if (!isFace)
                    continue;
                const bool known = std::any_of(faces.begin(), faces.end(), [&](const Face& f)
                                               { return length(f.normal - normal) < 1.0e-6 && std::abs(f.offset - offset) < 1.0e-6; });
                if (known)
                    continue;
                Face face{normal, offset, {}};
                for (int m = 0; m < n; ++m)
                    if (std::abs(dot(normal, vertices_[static_cast<std::size_t>(m)].direction) - offset) <= kTolerance)
                        face.members.push_back(m);
                faces.push_back(std::move(face));
            }

    const auto addTriangle = [this](int a, int b, int c)
    {
        Triangle t;
        t.vertex = {a, b, c};
        t.inverse = inverseOfColumns(vertices_[static_cast<std::size_t>(a)].direction, vertices_[static_cast<std::size_t>(b)].direction,
                                     vertices_[static_cast<std::size_t>(c)].direction);
        triangles_.push_back(t);
    };

    for (const auto& face : faces)
    {
        if (face.members.size() == 3)
        {
            addTriangle(face.members[0], face.members[1], face.members[2]);
            continue;
        }
        // A face of more speakers: a virtual one in its middle (shared equally by them), and a fan.
        Vec3 centroid;
        for (const int m : face.members)
            centroid = centroid + vertices_[static_cast<std::size_t>(m)].direction;
        centroid = centroid * (1.0 / static_cast<double>(face.members.size()));
        const auto u = normalised(vertices_[static_cast<std::size_t>(face.members.front())].direction - centroid);
        const auto w = cross(face.normal, u);
        auto ordered = face.members;
        std::sort(ordered.begin(), ordered.end(), [&](int p, int q)
                  {
                      const auto dp = vertices_[static_cast<std::size_t>(p)].direction - centroid;
                      const auto dq = vertices_[static_cast<std::size_t>(q)].direction - centroid;
                      return std::atan2(dot(dp, w), dot(dp, u)) < std::atan2(dot(dq, w), dot(dq, u));
                  });

        Vertex middle{normalised(centroid), -1, {}};
        Gains shares{};
        const double each = 1.0 / std::sqrt(static_cast<double>(ordered.size()));
        for (const int m : ordered)
        {
            const auto& member = vertices_[static_cast<std::size_t>(m)];
            if (member.channel >= 0)
                shares[static_cast<std::size_t>(member.channel)] += each;
            else
                for (const auto& [channel, gain] : member.shares)
                    shares[static_cast<std::size_t>(channel)] += each * gain;
        }
        normalisePower(shares, layout.numChannels);
        for (int c = 0; c < layout.numChannels; ++c)
            if (shares[static_cast<std::size_t>(c)] > 0.0)
                middle.shares.emplace_back(c, shares[static_cast<std::size_t>(c)]);
        vertices_.push_back(std::move(middle));
        const int centre = static_cast<int>(vertices_.size()) - 1;
        for (std::size_t m = 0; m < ordered.size(); ++m)
            addTriangle(centre, ordered[m], ordered[(m + 1) % ordered.size()]);
    }
}

// --- Gains ---------------------------------------------------------------------------------------

void SpatialPanner::meshGains(const Vec3& p, Gains& gains) const noexcept
{
    // The triangle around p: all three gains non-negative (or, at a seam, the nearest to that).
    int best = -1;
    double bestMinimum = -std::numeric_limits<double>::infinity();
    std::array<double, 3> bestGains{};
    for (std::size_t t = 0; t < triangles_.size(); ++t)
    {
        const auto& inv = triangles_[t].inverse;
        std::array<double, 3> g{};
        for (std::size_t r = 0; r < 3; ++r)
            g[r] = inv[r][0] * p.x + inv[r][1] * p.y + inv[r][2] * p.z;
        const double minimum = std::min({g[0], g[1], g[2]});
        if (minimum > bestMinimum)
        {
            bestMinimum = minimum;
            best = static_cast<int>(t);
            bestGains = g;
            if (minimum >= -1.0e-9)
                break;
        }
    }
    if (best < 0)
        return;
    const auto& triangle = triangles_[static_cast<std::size_t>(best)];
    for (std::size_t k = 0; k < 3; ++k)
    {
        const double g = std::max(0.0, bestGains[k]);
        const auto& vertex = vertices_[static_cast<std::size_t>(triangle.vertex[k])];
        if (vertex.channel >= 0)
            gains[static_cast<std::size_t>(vertex.channel)] += g;
        else
            for (const auto& [channel, share] : vertex.shares)
                gains[static_cast<std::size_t>(channel)] += g * share;
    }
}

SpatialPanner::Gains SpatialPanner::pointGains(double azimuthDegrees, double elevationDegrees) const noexcept
{
    Gains gains{};
    const auto p = directionVector(azimuthDegrees, elevationDegrees);
    switch (mode_)
    {
    case Mode::none:
        break;
    case Mode::single:
        gains[static_cast<std::size_t>(single_)] = 1.0;
        break;
    case Mode::pair:
    {
        // Along the arc between the pair (two-speaker VBAP), anything outside it folded in: a source
        // behind plays as its mirror in front, one overhead from both.
        const auto l = directionVector(directionOf(layout_, layout_.speakers[static_cast<std::size_t>(left_)]).azimuth, 0.0);
        const auto r = directionVector(directionOf(layout_, layout_.speakers[static_cast<std::size_t>(right_)]).azimuth, 0.0);
        const double x = p.x;
        const double y = std::abs(p.y);
        if (std::abs(x) < 1.0e-12 && y < 1.0e-12)
        {
            gains[static_cast<std::size_t>(left_)] = gains[static_cast<std::size_t>(right_)] = 1.0;
            break;
        }
        const double det = l.x * r.y - r.x * l.y;
        double gl = (x * r.y - r.x * y) / det;
        double gr = (l.x * y - x * l.y) / det;
        gains[static_cast<std::size_t>(left_)] = std::max(0.0, gl);
        gains[static_cast<std::size_t>(right_)] = std::max(0.0, gr);
        break;
    }
    case Mode::mesh:
        meshGains(p, gains);
        break;
    }
    normalisePower(gains, layout_.numChannels);
    return gains;
}

void SpatialPanner::addPointPower(const Vec3& direction, double weight, Gains& power) const noexcept
{
    const auto [azimuth, elevation] = directionAngles(direction);
    const auto gains = pointGains(azimuth, elevation);
    for (int c = 0; c < layout_.numChannels; ++c)
        power[static_cast<std::size_t>(c)] += weight * gains[static_cast<std::size_t>(c)] * gains[static_cast<std::size_t>(c)];
}

SpatialPanner::Gains SpatialPanner::spreadGains(double azimuthDegrees, double elevationDegrees, double spread) const noexcept
{
    spread = std::clamp(spread, 0.0, 1.0);
    if (spread < 1.0e-3 || mode_ == Mode::single || mode_ == Mode::none)
        return pointGains(azimuthDegrees, elevationDegrees);

    // Directions over the cap around the source (rings of more and more points), each weighted by
    // the area it stands for, their gains' powers summed: energy-preserving spread.
    const auto p = directionVector(azimuthDegrees, elevationDegrees);
    const auto u = normalised(cross(p, std::abs(p.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{0.0, 1.0, 0.0}));
    const auto w = cross(p, u);
    const double cap = spread * kPi;
    constexpr int kRings = 6;
    const double step = cap / kRings;

    Gains power{};
    addPointPower(p, 2.0 * kPi * (1.0 - std::cos(step * 0.5)), power);
    for (int ring = 1; ring <= kRings; ++ring)
    {
        const double phi = std::min(kPi, step * ring);
        const int points = 6 * ring;
        const double band = 2.0 * kPi * (std::cos(std::max(0.0, phi - step * 0.5)) - std::cos(std::min(kPi, phi + step * 0.5)));
        for (int k = 0; k < points; ++k)
        {
            const double theta = 2.0 * kPi * (k + 0.5 * (ring % 2)) / points;
            const auto direction = p * std::cos(phi) + (u * std::cos(theta) + w * std::sin(theta)) * std::sin(phi);
            addPointPower(direction, band / points, power);
        }
    }
    Gains gains{};
    for (int c = 0; c < layout_.numChannels; ++c)
        gains[static_cast<std::size_t>(c)] = std::sqrt(power[static_cast<std::size_t>(c)]);
    normalisePower(gains, layout_.numChannels);
    return gains;
}

double SpatialPanner::effectiveSpread(double size, double distance) noexcept
{
    return 1.0 - (1.0 - std::clamp(size, 0.0, 1.0)) * std::clamp(distance, 0.0, 1.0);
}

// --- Decorrelation ---------------------------------------------------------------------------------

void Decorrelator::prepare(double sampleRate, int numChannels)
{
    numChannels_ = std::clamp(numChannels, 0, kMaxChannels);
    // Delays (samples at 48 kHz): primes from 41 (0.85 ms) up, the shortest 16 for every channel's
    // first section, the next 16 for its second, and so on, so each channel has one short, two middle
    // and one long section, all different.
    std::vector<int> primes;
    for (int v = 41; static_cast<int>(primes.size()) < kSections * kMaxChannels; v += 2)
    {
        bool prime = true;
        for (int d = 3; d * d <= v && prime; d += 2)
            prime = v % d != 0;
        if (prime)
            primes.push_back(v * (primes.size() % 3 == 0 ? 3 : 1)); // (spread them further apart)
    }
    std::sort(primes.begin(), primes.end());
    const double scale = sampleRate / 48'000.0;
    for (int c = 0; c < numChannels_; ++c)
        for (int s = 0; s < kSections; ++s)
        {
            auto& section = sections_[static_cast<std::size_t>(c)][static_cast<std::size_t>(s)];
            const int delay = std::max(1, static_cast<int>(std::lround(primes[static_cast<std::size_t>(s * kMaxChannels + c)] * scale)));
            section.buffer.assign(static_cast<std::size_t>(delay), 0.0);
            section.write = 0;
            section.gain = (s + c) % 2 == 0 ? 0.6 : -0.6;
        }
}

void Decorrelator::reset() noexcept
{
    for (auto& channel : sections_)
        for (auto& section : channel)
        {
            std::fill(section.buffer.begin(), section.buffer.end(), 0.0);
            section.write = 0;
        }
}

void Decorrelator::process(int channel, const Sample* input, Sample* output, int numSamples) noexcept
{
    if (channel < 0 || channel >= numChannels_)
        return;
    auto& sections = sections_[static_cast<std::size_t>(channel)];
    for (int i = 0; i < numSamples; ++i)
    {
        Sample x = input[i];
        for (auto& section : sections)
        {
            // Schroeder all-pass: w = x + g w[n-D]; y = -g w + w[n-D].
            const auto size = static_cast<int>(section.buffer.size());
            const Sample delayed = section.buffer[static_cast<std::size_t>(section.write)];
            const Sample w = x + section.gain * delayed;
            x = -section.gain * w + delayed;
            section.buffer[static_cast<std::size_t>(section.write)] = w;
            section.write = section.write + 1 == size ? 0 : section.write + 1;
        }
        output[i] = x;
    }
}

} // namespace dojo::engine

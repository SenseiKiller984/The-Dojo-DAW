#include "engine/dsp/SurroundVirtualizer.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace dojo::engine::dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kLateSeconds = 0.35; ///< T60 of the late reverberation
constexpr double kLineSeconds = 0.016; ///< the paths' delay lines (the longest path is ~11 ms)

// The pinna model.
constexpr double kNotchQ = 4.0;
constexpr double kNotchDepth = 12.0;       ///< dB, up to 30 degrees of elevation
constexpr double kOverheadFrequency = 8000.0;
constexpr double kOverheadQ = 2.5;
constexpr double kOverheadGain = 4.0;      ///< dB, straight above
constexpr double kRearShelfFrequency = 4000.0;
constexpr double kRearShelfGain = -8.0;    ///< dB, straight behind

// The reflections' levels (relative to the direct sound at the same distance).
constexpr double kFloorReflection = 0.45;
constexpr double kCeilingReflection = 0.4;
constexpr double kSideReflection = 0.55;
constexpr double kEndReflection = 0.45;
constexpr std::array<double, 4> kLateDelaysMs{23.1, 29.3, 37.9, 43.7};

int nextPowerOfTwo(int n)
{
    int p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

double degrees(double radians) { return radians * 180.0 / kPi; }
double radians(double degreesValue) { return degreesValue * kPi / 180.0; }

/** RBJ cookbook biquads. */
template <typename Biquad>
void setLowPass(Biquad& f, double frequency, double q, double sampleRate)
{
    const double w0 = 2.0 * kPi * frequency / sampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    f.b0 = (1.0 - cosw) / 2.0 / a0;
    f.b1 = (1.0 - cosw) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * cosw / a0;
    f.a2 = (1.0 - alpha) / a0;
}

template <typename Biquad>
void setHighPass(Biquad& f, double frequency, double q, double sampleRate)
{
    const double w0 = 2.0 * kPi * frequency / sampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    f.b0 = (1.0 + cosw) / 2.0 / a0;
    f.b1 = -(1.0 + cosw) / a0;
    f.b2 = f.b0;
    f.a1 = -2.0 * cosw / a0;
    f.a2 = (1.0 - alpha) / a0;
}

template <typename Biquad>
void setHighShelf(Biquad& f, double frequency, double gainDb, double sampleRate)
{
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * frequency / sampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0); // shelf slope 1
    const double root = 2.0 * std::sqrt(a) * alpha;
    const double a0 = (a + 1.0) - (a - 1.0) * cosw + root;
    f.b0 = a * ((a + 1.0) + (a - 1.0) * cosw + root) / a0;
    f.b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cosw) / a0;
    f.b2 = a * ((a + 1.0) + (a - 1.0) * cosw - root) / a0;
    f.a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cosw) / a0;
    f.a2 = ((a + 1.0) - (a - 1.0) * cosw - root) / a0;
}

template <typename Biquad>
void setPeak(Biquad& f, double frequency, double gainDb, double q, double sampleRate)
{
    const double a = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * kPi * frequency / sampleRate;
    const double cosw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha / a;
    f.b0 = (1.0 + alpha * a) / a0;
    f.b1 = -2.0 * cosw / a0;
    f.b2 = (1.0 - alpha * a) / a0;
    f.a1 = -2.0 * cosw / a0;
    f.a2 = (1.0 - alpha / a) / a0;
}

/** A biquad's gain (linear) at `frequency`. */
template <typename Biquad>
double gainOf(const Biquad& f, double frequency, double sampleRate)
{
    const std::complex<double> z = std::polar(1.0, -2.0 * kPi * frequency / sampleRate); // (z^-1)
    return std::abs((f.b0 + f.b1 * z + f.b2 * z * z) / (1.0 + f.a1 * z + f.a2 * z * z));
}

/** A direction (degrees; azimuth positive to the right) as a unit vector: x right, y ahead, z up. */
struct Point
{
    double x = 0.0, y = 0.0, z = 0.0;
};
Point pointOf(double azimuth, double elevation, double distance)
{
    const double horizontal = distance * std::cos(radians(elevation));
    return {horizontal * std::sin(radians(azimuth)), horizontal * std::cos(radians(azimuth)), distance * std::sin(radians(elevation))};
}
} // namespace

// --- The head model ----------------------------------------------------------------------------------

double SurroundVirtualizer::azimuthOf(Speaker speaker, bool sevenOne) noexcept
{
    switch (speaker)
    {
    case Speaker::left:          return -30.0;
    case Speaker::right:         return 30.0;
    case Speaker::centre:        return 0.0;
    case Speaker::lfe:           return 0.0;
    case Speaker::leftSurround:  return sevenOne ? -90.0 : -110.0;
    case Speaker::rightSurround: return sevenOne ? 90.0 : 110.0;
    case Speaker::leftRear:      return -150.0;
    case Speaker::rightRear:     return 150.0;
    case Speaker::leftWide:      return -60.0;
    case Speaker::rightWide:     return 60.0;
    case Speaker::topFrontLeft:  return sevenOne ? -45.0 : -30.0;
    case Speaker::topFrontRight: return sevenOne ? 45.0 : 30.0;
    case Speaker::topMiddleLeft: return -90.0;
    case Speaker::topMiddleRight: return 90.0;
    case Speaker::topRearLeft:   return sevenOne ? -135.0 : -110.0;
    case Speaker::topRearRight:  return sevenOne ? 135.0 : 110.0;
    }
    return 0.0;
}

double SurroundVirtualizer::elevationOf(Speaker speaker) noexcept
{
    return isHeight(speaker) ? 30.0 : 0.0;
}

double SurroundVirtualizer::incidence(double azimuthDegrees, bool rightEar) noexcept
{
    return incidence(azimuthDegrees, 0.0, rightEar);
}

double SurroundVirtualizer::incidence(double azimuthDegrees, double elevationDegrees, bool rightEar) noexcept
{
    // The ears are at the ends of the x axis: the cosine of the angle is the direction's x.
    const double towardsRight = std::sin(radians(azimuthDegrees)) * std::cos(radians(elevationDegrees));
    return std::acos(std::clamp(rightEar ? towardsRight : -towardsRight, -1.0, 1.0));
}

double SurroundVirtualizer::earDelay(double theta) noexcept
{
    const double t = kHeadRadius / kSpeedOfSound;
    const double delay = theta < kPi / 2.0 ? -t * std::cos(theta) : t * (theta - kPi / 2.0);
    return delay + t; // (0 for a source straight at the ear)
}

double SurroundVirtualizer::shadowAlpha(double theta) noexcept
{
    constexpr double alphaMin = 0.1;
    const double thetaMin = radians(150.0);
    return (1.0 + alphaMin / 2.0) + (1.0 - alphaMin / 2.0) * std::cos(theta / thetaMin * kPi);
}

double SurroundVirtualizer::shadowGain(double theta, double frequency) noexcept
{
    const double beta = 2.0 * kSpeedOfSound / kHeadRadius;
    const double omega = 2.0 * kPi * frequency;
    const double alpha = shadowAlpha(theta);
    return std::sqrt((alpha * alpha * omega * omega + beta * beta) / (omega * omega + beta * beta));
}

void SurroundVirtualizer::setHeadShadow(EarPath& path, double theta) const noexcept
{
    const double beta = 2.0 * kSpeedOfSound / kHeadRadius;
    const double k = 2.0 * sampleRate_;
    const double alpha = shadowAlpha(theta);
    path.b0 = (alpha * k + beta) / (k + beta);
    path.b1 = (beta - alpha * k) / (k + beta);
    path.a1 = (beta - k) / (k + beta);
}

void SurroundVirtualizer::setDelay(EarPath& path, double delay) const noexcept
{
    // Third-order Lagrange between the samples delay-1 .. delay+2 back.
    path.delay = delay;
    const auto whole = static_cast<int>(delay);
    const double t = delay - whole + 1.0;
    path.back = whole - 1;
    path.taps = {-(t - 1.0) * (t - 2.0) * (t - 3.0) / 6.0, t * (t - 2.0) * (t - 3.0) / 2.0,
                 -t * (t - 1.0) * (t - 3.0) / 2.0, t * (t - 1.0) * (t - 2.0) / 6.0};
}

// --- The pinna and the room ------------------------------------------------------------------------------

SurroundVirtualizer::PinnaCue SurroundVirtualizer::pinnaCue(double azimuthDegrees, double elevationDegrees) noexcept
{
    const double elevation = std::clamp(elevationDegrees, -90.0, 90.0);
    PinnaCue cue;
    cue.notchFrequency = 4000.0 * std::pow(3.0, (elevation + 90.0) / 180.0);
    cue.notchDepth = kNotchDepth * std::clamp((90.0 - elevation) / 60.0, 0.0, 1.0);
    cue.overheadGain = kOverheadGain * std::max(0.0, std::sin(radians(elevation)));
    const double behind = -std::cos(radians(azimuthDegrees)) * std::cos(radians(elevation));
    cue.rearShelf = kRearShelfGain * std::max(0.0, behind);
    return cue;
}

void SurroundVirtualizer::setPinna(Path& path, double azimuth, double elevation) const noexcept
{
    const auto cue = pinnaCue(azimuth, elevation);
    const double nyquistMargin = 0.45 * sampleRate_;
    const auto turnOn = [](bool& on, bool wanted, Biquad& filter)
    {
        if (wanted && !on)
            filter.clear(); // (its state is from another direction, or stale)
        on = wanted;
    };
    turnOn(path.hasNotch, cue.notchDepth > 0.05 && cue.notchFrequency < nyquistMargin, path.notch);
    if (path.hasNotch)
        setPeak(path.notch, cue.notchFrequency, -cue.notchDepth, kNotchQ, sampleRate_);
    turnOn(path.hasOverhead, cue.overheadGain > 0.05 && kOverheadFrequency < nyquistMargin, path.overhead);
    if (path.hasOverhead)
        setPeak(path.overhead, kOverheadFrequency, cue.overheadGain, kOverheadQ, sampleRate_);
    turnOn(path.hasShelf, cue.rearShelf < -0.05, path.shelf);
    if (path.hasShelf)
        setHighShelf(path.shelf, kRearShelfFrequency, cue.rearShelf, sampleRate_);

    // The notch and the overhead band's power over the audio band (white), made up.
    path.makeup = 1.0;
    if (path.hasNotch || path.hasOverhead)
    {
        constexpr int kSteps = 128;
        const double top = std::min(20000.0, nyquistMargin);
        double power = 0.0;
        for (int i = 0; i < kSteps; ++i)
        {
            const double frequency = 20.0 + (top - 20.0) * (i + 0.5) / kSteps;
            const double g = (path.hasNotch ? gainOf(path.notch, frequency, sampleRate_) : 1.0)
                             * (path.hasOverhead ? gainOf(path.overhead, frequency, sampleRate_) : 1.0);
            power += g * g;
        }
        path.makeup = 1.0 / std::sqrt(power / kSteps);
    }
}

double SurroundVirtualizer::pinnaGain(double azimuthDegrees, double elevationDegrees, double frequency, double sampleRate) noexcept
{
    SurroundVirtualizer v;
    v.sampleRate_ = sampleRate;
    Path path;
    v.setPinna(path, azimuthDegrees, elevationDegrees);
    double gain = path.makeup;
    if (path.hasNotch)
        gain *= gainOf(path.notch, frequency, sampleRate);
    if (path.hasOverhead)
        gain *= gainOf(path.overhead, frequency, sampleRate);
    if (path.hasShelf)
        gain *= gainOf(path.shelf, frequency, sampleRate);
    return 20.0 * std::log10(gain);
}

std::array<SurroundVirtualizer::Arrival, SurroundVirtualizer::kPaths> SurroundVirtualizer::arrivals(double azimuthDegrees, double elevationDegrees,
                                                                                                    double room) noexcept
{
    // Image sources: the speaker mirrored in each surface (the listener's head at the origin).
    const auto p = pointOf(azimuthDegrees, elevationDegrees, kSpeakerDistance);
    const double floorZ = -kEarHeight;
    const double ceilingZ = kRoomHeight - kEarHeight;
    const double sideX = p.x >= 0.0 ? kRoomWidth / 2.0 : -kRoomWidth / 2.0;
    const double endY = p.y >= 0.0 ? kRoomLength / 2.0 : -kRoomLength / 2.0;
    const std::array<std::pair<Point, double>, kPaths> images{{
        {p, 1.0},
        {{p.x, p.y, 2.0 * floorZ - p.z}, kFloorReflection},
        {{p.x, p.y, 2.0 * ceilingZ - p.z}, kCeilingReflection},
        {{2.0 * sideX - p.x, p.y, p.z}, kSideReflection},
        {{p.x, 2.0 * endY - p.y, p.z}, kEndReflection},
    }};
    const double scale = std::clamp(room, 0.0, 1.0);
    std::array<Arrival, kPaths> result{};
    for (std::size_t i = 0; i < images.size(); ++i)
    {
        const auto& [q, reflection] = images[i];
        auto& a = result[i];
        a.distance = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z);
        a.azimuth = degrees(std::atan2(q.x, q.y));
        a.elevation = degrees(std::atan2(q.z, std::hypot(q.x, q.y)));
        // (spherical spreading, relative to the direct sound)
        a.gain = i == 0 ? 1.0 : scale * reflection * kSpeakerDistance / a.distance;
    }
    return result;
}

void SurroundVirtualizer::clear(Path& path) noexcept
{
    std::fill(path.line.begin(), path.line.end(), 0.0);
    path.notch.clear();
    path.overhead.clear();
    path.shelf.clear();
    for (auto& ear : path.ears)
        ear.x1 = ear.y1 = 0.0;
}

// --- Set-up ------------------------------------------------------------------------------------------

void SurroundVirtualizer::prepare(double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int size = nextPowerOfTwo(static_cast<int>(kLineSeconds * sampleRate_) + 8);
    mask_ = size - 1;
    for (auto& s : sources_)
        for (auto& path : s.paths)
            path.line.assign(static_cast<std::size_t>(size), 0.0);
    for (std::size_t i = 0; i < late_.size(); ++i)
    {
        lateLength_[i] = std::max(8, static_cast<int>(kLateDelaysMs[i] * 0.001 * sampleRate_));
        late_[i].assign(static_cast<std::size_t>(lateLength_[i]), 0.0);
    }
    const int xtcSize = nextPowerOfTwo(static_cast<int>(0.002 * sampleRate_) + 8);
    xtcMask_ = xtcSize - 1;
    xtcLeft_.assign(static_cast<std::size_t>(xtcSize), 0.0);
    xtcRight_.assign(static_cast<std::size_t>(xtcSize), 0.0);
    configure(layout_, output_, room_);
    reset();
}

void SurroundVirtualizer::reset() noexcept
{
    for (auto& s : sources_)
    {
        s.lfeLow1.clear();
        s.lfeLow2.clear();
        for (auto& path : s.paths)
            clear(path);
    }
    for (auto& line : late_)
        std::fill(line.begin(), line.end(), 0.0);
    lateWrite_.fill(0);
    lateDamp_.fill(0.0);
    std::fill(xtcLeft_.begin(), xtcLeft_.end(), 0.0);
    std::fill(xtcRight_.begin(), xtcRight_.end(), 0.0);
    for (auto* f : {&xtcHighL_, &xtcLowL_, &xtcHighR_, &xtcLowR_})
        f->clear();
    write_ = 0;
    xtcWrite_ = 0;
}

void SurroundVirtualizer::configure(const ChannelLayout& layout, Output output, double room) noexcept
{
    const bool changed = layout != layout_;
    layout_ = layout;
    output_ = output;
    room_ = std::clamp(room, 0.0, 1.0);
    const bool sevenOne = layout.has(Speaker::leftRear);

    double binauralPower = 0.0;
    for (int c = 0; c < kMaxSources; ++c)
    {
        auto& s = sources_[static_cast<std::size_t>(c)];
        s.active = c < layout.numChannels;
        const auto speaker = s.active ? layout.speakers[static_cast<std::size_t>(c)] : Speaker::lfe;
        s.lfe = s.active && speaker == Speaker::lfe;
        if (!s.active || s.lfe)
        {
            for (auto& path : s.paths)
                path.gain = 0.0;
            if (s.lfe)
            {
                setLowPass(s.lfeLow1, 120.0, 0.7071, sampleRate_);
                setLowPass(s.lfeLow2, 120.0, 0.7071, sampleRate_);
            }
            continue;
        }

        // The direct sound and four first reflections, each from its own direction, to both ears.
        const auto ways = arrivals(azimuthOf(speaker, sevenOne), elevationOf(speaker), room_);
        for (std::size_t p = 0; p < ways.size(); ++p)
        {
            auto& path = s.paths[p];
            const bool wasRendered = path.gain > 0.0;
            path.gain = ways[p].gain;
            if (!(path.gain > 0.0))
                continue;
            if (!wasRendered && !changed)
                clear(path); // (its line stopped being written; a change resets everything below)
            setPinna(path, ways[p].azimuth, ways[p].elevation);
            path.level = path.gain * path.makeup;
            const double extra = (ways[p].distance - kSpeakerDistance) / kSpeedOfSound;
            for (int ear = 0; ear < 2; ++ear)
            {
                auto& earPath = path.ears[static_cast<std::size_t>(ear)];
                const double theta = incidence(ways[p].azimuth, ways[p].elevation, ear == 1);
                double delay = 2.0 + (extra + earDelay(theta)) * sampleRate_;
                if (mask_ > 8)
                    delay = std::min(delay, static_cast<double>(mask_ - 4)); // (cannot happen in this room)
                setDelay(earPath, delay);
                setHeadShadow(earPath, theta);
            }
            binauralPower += path.gain * path.gain;
        }
    }

    // As loud as the ITU fold-down for uncorrelated channels of equal level.
    const auto fold = conversionMatrix(layout, ChannelLayout::stereo());
    double foldPower = 0.0;
    for (int c = 0; c < std::min(fold.numInputs, layout.numChannels); ++c)
        if (layout.speakers[static_cast<std::size_t>(c)] != Speaker::lfe)
            foldPower += fold.gain[0][static_cast<std::size_t>(c)] * fold.gain[0][static_cast<std::size_t>(c)];
    normalise_ = binauralPower > 0.0 ? std::sqrt(foldPower / binauralPower) : 1.0;

    // Late reverberation.
    for (std::size_t i = 0; i < late_.size(); ++i)
        lateGain_[i] = std::pow(10.0, -3.0 * (lateLength_[i] / sampleRate_) / kLateSeconds);
    lateMix_ = 0.12 * room_;
    dampCoefficient_ = std::exp(-2.0 * kPi * 5000.0 / sampleRate_);

    // Crosstalk cancellation for speakers at +-30 degrees.
    const double span = radians(30.0);
    xtcDelay_ = (kHeadRadius / kSpeedOfSound) * (span + std::sin(span)) * sampleRate_;
    setHighPass(xtcHighL_, 250.0, 0.7071, sampleRate_);
    setHighPass(xtcHighR_, 250.0, 0.7071, sampleRate_);
    setLowPass(xtcLowL_, 6000.0, 0.7071, sampleRate_);
    setLowPass(xtcLowR_, 6000.0, 0.7071, sampleRate_);

    if (changed)
        reset();
}

// --- Processing ----------------------------------------------------------------------------------------

void SurroundVirtualizer::process(const Sample* const* in, int numChannels, Sample* left, Sample* right, int numSamples) noexcept
{
    const int channels = std::min({numChannels, layout_.numChannels, kMaxSources});
    if (sources_[0].paths[0].line.empty())
        return; // (not prepared)

    for (int n = 0; n < numSamples; ++n)
    {
        double ears[2] = {0.0, 0.0};
        double lfe = 0.0;
        double lateIn = 0.0;

        for (int c = 0; c < channels; ++c)
        {
            auto& s = sources_[static_cast<std::size_t>(c)];
            const double x = in[c] != nullptr ? in[c][n] : 0.0;
            if (s.lfe)
            {
                lfe += s.lfeLow2.process(s.lfeLow1.process(x));
                continue;
            }
            lateIn += x;
            for (auto& path : s.paths)
            {
                if (!(path.gain > 0.0))
                    continue; // (a reflection, with no room)
                double shaped = x;
                if (path.hasShelf)
                    shaped = path.shelf.process(shaped);
                if (path.hasNotch)
                    shaped = path.notch.process(shaped);
                if (path.hasOverhead)
                    shaped = path.overhead.process(shaped);
                auto* line = path.line.data();
                line[write_] = shaped;
                for (int ear = 0; ear < 2; ++ear)
                {
                    auto& e = path.ears[static_cast<std::size_t>(ear)];
                    const int from = write_ - e.back;
                    const double delayed = e.taps[0] * line[from & mask_] + e.taps[1] * line[(from - 1) & mask_]
                                           + e.taps[2] * line[(from - 2) & mask_] + e.taps[3] * line[(from - 3) & mask_];
                    const double y = e.b0 * delayed + e.b1 * e.x1 - e.a1 * e.y1;
                    e.x1 = delayed;
                    e.y1 = y;
                    ears[ear] += path.level * y;
                }
            }
        }
        write_ = (write_ + 1) & mask_;

        double l = ears[0] * normalise_ + 0.5 * lfe;
        double r = ears[1] * normalise_ + 0.5 * lfe;

        // Late reverberation: four lines, a Hadamard feedback matrix, damped.
        if (lateMix_ > 0.0)
        {
            std::array<double, kLateLines> out{};
            for (std::size_t i = 0; i < late_.size(); ++i)
            {
                out[i] = late_[i][static_cast<std::size_t>(lateWrite_[i])];
                lateDamp_[i] = (1.0 - dampCoefficient_) * out[i] + dampCoefficient_ * lateDamp_[i];
            }
            const auto& d = lateDamp_;
            const std::array<double, kLateLines> mixed{0.5 * (d[0] + d[1] + d[2] + d[3]), 0.5 * (d[0] - d[1] + d[2] - d[3]),
                                                       0.5 * (d[0] + d[1] - d[2] - d[3]), 0.5 * (d[0] - d[1] - d[2] + d[3])};
            const double input = lateIn * normalise_ * 0.25;
            for (std::size_t i = 0; i < late_.size(); ++i)
            {
                late_[i][static_cast<std::size_t>(lateWrite_[i])] = lateGain_[i] * mixed[i] + (i % 2 == 0 ? input : -input);
                lateWrite_[i] = lateWrite_[i] + 1 >= lateLength_[i] ? 0 : lateWrite_[i] + 1;
            }
            l += lateMix_ * (out[0] + out[2]);
            r += lateMix_ * (out[1] + out[3]);
        }

        // Speakers: cancel what each speaker sends to the far ear (recursive, band-limited).
        if (output_ == Output::speakers)
        {
            const auto delayed = [&](const std::vector<double>& line)
            {
                // (the newest value in the line is one sample old already)
                const double back = std::max(0.0, xtcDelay_ - 1.0);
                const auto whole = static_cast<int>(back);
                const double frac = back - whole;
                const double a = line[static_cast<std::size_t>((xtcWrite_ - whole) & xtcMask_)];
                const double b = line[static_cast<std::size_t>((xtcWrite_ - whole - 1) & xtcMask_)];
                return a + (b - a) * frac;
            };
            const double fromRight = xtcLowR_.process(xtcHighR_.process(delayed(xtcRight_)));
            const double fromLeft = xtcLowL_.process(xtcHighL_.process(delayed(xtcLeft_)));
            const double yl = l - xtcGain_ * fromRight;
            const double yr = r - xtcGain_ * fromLeft;
            xtcWrite_ = (xtcWrite_ + 1) & xtcMask_;
            xtcLeft_[static_cast<std::size_t>(xtcWrite_)] = yl;
            xtcRight_[static_cast<std::size_t>(xtcWrite_)] = yr;
            l = yl;
            r = yr;
        }

        left[n] = l;
        right[n] = r;
    }
}

// --- Devices ---------------------------------------------------------------------------------------------

ChannelLayout deviceLayout(int numOutputs, bool alsaOrder) noexcept
{
    using S = Speaker;
    if (numOutputs >= 8)
        return alsaOrder ? ChannelLayout::of({S::left, S::right, S::leftRear, S::rightRear, S::centre, S::lfe, S::leftSurround, S::rightSurround})
                         : ChannelLayout::of({S::left, S::right, S::centre, S::lfe, S::leftRear, S::rightRear, S::leftSurround, S::rightSurround});
    if (numOutputs >= 6)
        return alsaOrder ? ChannelLayout::of({S::left, S::right, S::leftSurround, S::rightSurround, S::centre, S::lfe})
                         : ChannelLayout::of({S::left, S::right, S::centre, S::lfe, S::leftSurround, S::rightSurround});
    if (numOutputs >= 4)
        return ChannelLayout::of({S::left, S::right, S::leftSurround, S::rightSurround});
    return ChannelLayout::stereo();
}

} // namespace dojo::engine::dsp

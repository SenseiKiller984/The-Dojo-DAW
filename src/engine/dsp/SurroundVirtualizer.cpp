#include "engine/dsp/SurroundVirtualizer.h"

#include <algorithm>
#include <cmath>

namespace dojo::engine::dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSpeakerDistance = 2.0; ///< metres from the listener
constexpr double kEarHeight = 1.2;       ///< above the floor
constexpr double kRoomWidth = 5.0;       ///< side walls at +-2.5 m
constexpr double kRoomLength = 6.0;      ///< front and back walls at +-3 m
constexpr double kLateSeconds = 0.35;    ///< T60 of the late reverberation
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
    }
    return 0.0;
}

double SurroundVirtualizer::incidence(double azimuthDegrees, bool rightEar) noexcept
{
    double difference = azimuthDegrees - (rightEar ? 90.0 : -90.0);
    difference = std::fmod(difference + 540.0, 360.0) - 180.0; // -180 .. 180
    return radians(std::abs(difference));
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

// --- Set-up ------------------------------------------------------------------------------------------

void SurroundVirtualizer::prepare(double sampleRate)
{
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    const int size = nextPowerOfTwo(static_cast<int>(0.025 * sampleRate_) + 8); // (the longest path is ~13 ms)
    mask_ = size - 1;
    for (auto& s : sources_)
        s.line.assign(static_cast<std::size_t>(size), 0.0);
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
        std::fill(s.line.begin(), s.line.end(), 0.0);
        s.pinna.clear();
        s.lfeLow1.clear();
        s.lfeLow2.clear();
        for (auto& ears : s.paths)
            for (auto& p : ears)
                p.x1 = p.y1 = 0.0;
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
        if (!s.active)
            continue;
        const auto speaker = layout.speakers[static_cast<std::size_t>(c)];
        s.lfe = speaker == Speaker::lfe;
        if (s.lfe)
        {
            setLowPass(s.lfeLow1, 120.0, 0.7071, sampleRate_);
            setLowPass(s.lfeLow2, 120.0, 0.7071, sampleRate_);
            continue;
        }

        const double azimuth = azimuthOf(speaker, sevenOne);
        setHighShelf(s.pinna, 4000.0, -8.0 * std::max(0.0, -std::cos(radians(azimuth))), sampleRate_);

        // The direct sound and three first reflections (image sources), each to both ears.
        const double x = kSpeakerDistance * std::sin(radians(azimuth));
        const double y = kSpeakerDistance * std::cos(radians(azimuth));
        struct Image
        {
            double azimuth, distance, gain;
        };
        const double floorDistance = std::hypot(kSpeakerDistance, 2.0 * kEarHeight);
        const double sideX = x >= 0.0 ? kRoomWidth - x : -kRoomWidth - x;
        const double endY = y >= 0.0 ? kRoomLength - y : -kRoomLength - y;
        const std::array<Image, kPaths> images{{
            {azimuth, kSpeakerDistance, 1.0},
            {azimuth, floorDistance, room_ * 0.45 * kSpeakerDistance / floorDistance},
            {degrees(std::atan2(sideX, y)), std::hypot(sideX, y), room_ * 0.55 * kSpeakerDistance / std::hypot(sideX, y)},
            {degrees(std::atan2(x, endY)), std::hypot(x, endY), room_ * 0.45 * kSpeakerDistance / std::hypot(x, endY)},
        }};
        for (std::size_t p = 0; p < images.size(); ++p)
        {
            const double extra = (images[p].distance - kSpeakerDistance) / kSpeedOfSound;
            for (int ear = 0; ear < 2; ++ear)
            {
                auto& path = s.paths[p][static_cast<std::size_t>(ear)];
                const double theta = incidence(images[p].azimuth, ear == 1);
                path.delay = 2.0 + (extra + earDelay(theta)) * sampleRate_;
                path.gain = images[p].gain;
                setHeadShadow(path, theta);
            }
            binauralPower += images[p].gain * images[p].gain;
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

double SurroundVirtualizer::readDelayed(const std::vector<double>& line, int write, double delay) const noexcept
{
    // Third-order Lagrange between the samples delay-1 .. delay+2 back.
    const auto whole = static_cast<int>(delay);
    const double t = delay - whole + 1.0;
    const auto sample = [&](int back) { return line[static_cast<std::size_t>((write - back) & mask_)]; };
    const double h0 = -(t - 1.0) * (t - 2.0) * (t - 3.0) / 6.0;
    const double h1 = t * (t - 2.0) * (t - 3.0) / 2.0;
    const double h2 = -t * (t - 1.0) * (t - 3.0) / 2.0;
    const double h3 = t * (t - 1.0) * (t - 2.0) / 6.0;
    return h0 * sample(whole - 1) + h1 * sample(whole) + h2 * sample(whole + 1) + h3 * sample(whole + 2);
}

void SurroundVirtualizer::process(const Sample* const* in, int numChannels, Sample* left, Sample* right, int numSamples) noexcept
{
    const int channels = std::min({numChannels, layout_.numChannels, kMaxSources});
    if (sources_[0].line.empty())
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
            const double shaped = s.pinna.process(x);
            s.line[static_cast<std::size_t>(write_)] = shaped;
            lateIn += shaped;
            for (auto& pathPair : s.paths)
                for (int ear = 0; ear < 2; ++ear)
                {
                    auto& p = pathPair[static_cast<std::size_t>(ear)];
                    if (!(p.gain > 0.0))
                        continue; // (a reflection, with no room)
                    const double delayed = readDelayed(s.line, write_, p.delay);
                    const double y = p.b0 * delayed + p.b1 * p.x1 - p.a1 * p.y1;
                    p.x1 = delayed;
                    p.y1 = y;
                    ears[ear] += p.gain * y;
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

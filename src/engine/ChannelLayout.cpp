#include "engine/ChannelLayout.h"

#include <algorithm>
#include <cmath>

namespace dojo::engine
{
namespace
{
constexpr double kMinus3dB = 0.70710678118654752;
constexpr double kHalfPi = 1.57079632679489662;

/** Where a speaker's signal goes when the target layout lacks it (one fold-down step). */
struct Fold
{
    int count = 0;
    std::array<std::pair<Speaker, double>, 2> targets{};
};

Fold foldOf(Speaker speaker) noexcept
{
    switch (speaker)
    {
    case Speaker::centre:        return {2, {{{Speaker::left, kMinus3dB}, {Speaker::right, kMinus3dB}}}};
    case Speaker::left:          return {1, {{{Speaker::centre, kMinus3dB}, {}}}}; // (only into mono)
    case Speaker::right:         return {1, {{{Speaker::centre, kMinus3dB}, {}}}};
    case Speaker::leftSurround:  return {1, {{{Speaker::left, kMinus3dB}, {}}}};
    case Speaker::rightSurround: return {1, {{{Speaker::right, kMinus3dB}, {}}}};
    case Speaker::leftRear:      return {1, {{{Speaker::leftSurround, kMinus3dB}, {}}}};
    case Speaker::rightRear:     return {1, {{{Speaker::rightSurround, kMinus3dB}, {}}}};
    case Speaker::topFrontLeft:  return {1, {{{Speaker::left, kMinus3dB}, {}}}};
    case Speaker::topFrontRight: return {1, {{{Speaker::right, kMinus3dB}, {}}}};
    case Speaker::topMiddleLeft:  return {1, {{{Speaker::leftSurround, kMinus3dB}, {}}}};
    case Speaker::topMiddleRight: return {1, {{{Speaker::rightSurround, kMinus3dB}, {}}}};
    case Speaker::leftWide:
    case Speaker::rightWide:
    case Speaker::topRearLeft:
    case Speaker::topRearRight:  // (route() picks their targets by what the layout has)
    case Speaker::lfe:           break;
    }
    return {};
}

/** Adds `gain` of input `input` (speaker `speaker`) into `matrix`, folding it down as needed. */
void route(ChannelMatrix& matrix, const ChannelLayout& to, int input, Speaker speaker, double gain, int depth) noexcept
{
    if (const int output = to.indexOf(speaker); output >= 0)
    {
        matrix.gain[static_cast<std::size_t>(output)][static_cast<std::size_t>(input)] += gain;
        return;
    }
    if (depth > 4)
        return;
    switch (speaker)
    {
    case Speaker::leftWide:
    case Speaker::rightWide:
    {
        // Between the front and the surround: into both (-3 dB each) where both exist.
        const bool isLeft = speaker == Speaker::leftWide;
        const auto front = isLeft ? Speaker::left : Speaker::right;
        const auto surround = isLeft ? Speaker::leftSurround : Speaker::rightSurround;
        if (to.has(front) && to.has(surround))
        {
            route(matrix, to, input, front, gain * kMinus3dB, depth + 1);
            route(matrix, to, input, surround, gain * kMinus3dB, depth + 1);
        }
        else
        {
            route(matrix, to, input, front, gain * kMinus3dB, depth + 1);
        }
        return;
    }
    case Speaker::topRearLeft:
    case Speaker::topRearRight:
    {
        // Into the rear below it, or the surround where there are no rears (5.1.4 -> 5.1).
        const bool isLeft = speaker == Speaker::topRearLeft;
        const auto rear = isLeft ? Speaker::leftRear : Speaker::rightRear;
        const auto surround = isLeft ? Speaker::leftSurround : Speaker::rightSurround;
        route(matrix, to, input, to.has(rear) ? rear : surround, gain * kMinus3dB, depth + 1);
        return;
    }
    case Speaker::left:
    case Speaker::right:
    case Speaker::centre:
    case Speaker::lfe:
    case Speaker::leftSurround:
    case Speaker::rightSurround:
    case Speaker::leftRear:
    case Speaker::rightRear:
    case Speaker::topFrontLeft:
    case Speaker::topFrontRight:
    case Speaker::topMiddleLeft:
    case Speaker::topMiddleRight:
        break; // (one fold step, below)
    }
    const auto fold = foldOf(speaker);
    for (int k = 0; k < fold.count; ++k)
        route(matrix, to, input, fold.targets[static_cast<std::size_t>(k)].first, gain * fold.targets[static_cast<std::size_t>(k)].second,
              depth + 1);
}

std::pair<double, double> balance(double pan) noexcept
{
    pan = std::clamp(pan, -1.0, 1.0);
    return {pan <= 0.0 ? 1.0 : 1.0 - pan, pan >= 0.0 ? 1.0 : 1.0 + pan};
}

void set(ChannelMatrix& m, const ChannelLayout& to, Speaker speaker, int input, double gain) noexcept
{
    if (const int output = to.indexOf(speaker); output >= 0)
        m.gain[static_cast<std::size_t>(output)][static_cast<std::size_t>(input)] += gain;
}
} // namespace

const char* shortName(Speaker speaker) noexcept
{
    switch (speaker)
    {
    case Speaker::left:          return "L";
    case Speaker::right:         return "R";
    case Speaker::centre:        return "C";
    case Speaker::lfe:           return "LFE";
    case Speaker::leftSurround:  return "Ls";
    case Speaker::rightSurround: return "Rs";
    case Speaker::leftRear:      return "Lrs";
    case Speaker::rightRear:     return "Rrs";
    case Speaker::leftWide:      return "Lw";
    case Speaker::rightWide:     return "Rw";
    case Speaker::topFrontLeft:  return "Ltf";
    case Speaker::topFrontRight: return "Rtf";
    case Speaker::topMiddleLeft: return "Ltm";
    case Speaker::topMiddleRight: return "Rtm";
    case Speaker::topRearLeft:   return "Ltr";
    case Speaker::topRearRight:  return "Rtr";
    }
    return "?";
}

int sideOf(Speaker speaker) noexcept
{
    switch (speaker)
    {
    case Speaker::left:
    case Speaker::leftSurround:
    case Speaker::leftRear:
    case Speaker::leftWide:
    case Speaker::topFrontLeft:
    case Speaker::topMiddleLeft:
    case Speaker::topRearLeft:   return -1;
    case Speaker::right:
    case Speaker::rightSurround:
    case Speaker::rightRear:
    case Speaker::rightWide:
    case Speaker::topFrontRight:
    case Speaker::topMiddleRight:
    case Speaker::topRearRight:  return 1;
    case Speaker::centre:
    case Speaker::lfe:           break;
    }
    return 0;
}

bool isHeight(Speaker speaker) noexcept
{
    return speaker == Speaker::topFrontLeft || speaker == Speaker::topFrontRight || speaker == Speaker::topMiddleLeft
           || speaker == Speaker::topMiddleRight || speaker == Speaker::topRearLeft || speaker == Speaker::topRearRight;
}

ChannelLayout ChannelLayout::of(std::initializer_list<Speaker> list) noexcept
{
    ChannelLayout layout;
    layout.numChannels = 0;
    for (const auto speaker : list)
        if (layout.numChannels < kMaxChannels)
            layout.speakers[static_cast<std::size_t>(layout.numChannels++)] = speaker;
    return layout;
}

ChannelLayout ChannelLayout::lcrLfe() noexcept
{
    return of({Speaker::left, Speaker::right, Speaker::centre, Speaker::lfe});
}

ChannelLayout ChannelLayout::surround51() noexcept
{
    return of({Speaker::left, Speaker::right, Speaker::centre, Speaker::lfe, Speaker::leftSurround, Speaker::rightSurround});
}

ChannelLayout ChannelLayout::surround71() noexcept
{
    return of({Speaker::left, Speaker::right, Speaker::centre, Speaker::lfe, Speaker::leftSurround, Speaker::rightSurround,
               Speaker::leftRear, Speaker::rightRear});
}

ChannelLayout ChannelLayout::dolbyDigital() noexcept
{
    return of({Speaker::left, Speaker::centre, Speaker::right, Speaker::leftSurround, Speaker::rightSurround, Speaker::lfe});
}

ChannelLayout ChannelLayout::dts() noexcept
{
    return of({Speaker::left, Speaker::right, Speaker::leftSurround, Speaker::rightSurround, Speaker::centre, Speaker::lfe});
}

ChannelLayout ChannelLayout::surround514() noexcept
{
    return of({Speaker::left, Speaker::right, Speaker::centre, Speaker::lfe, Speaker::leftSurround, Speaker::rightSurround,
               Speaker::topFrontLeft, Speaker::topFrontRight, Speaker::topRearLeft, Speaker::topRearRight});
}

ChannelLayout ChannelLayout::surround714() noexcept
{
    return of({Speaker::left, Speaker::right, Speaker::centre, Speaker::lfe, Speaker::leftSurround, Speaker::rightSurround,
               Speaker::leftRear, Speaker::rightRear, Speaker::topFrontLeft, Speaker::topFrontRight, Speaker::topRearLeft,
               Speaker::topRearRight});
}

ChannelLayout ChannelLayout::surround916() noexcept
{
    return of({Speaker::left, Speaker::right, Speaker::centre, Speaker::lfe, Speaker::leftSurround, Speaker::rightSurround,
               Speaker::leftRear, Speaker::rightRear, Speaker::leftWide, Speaker::rightWide, Speaker::topFrontLeft,
               Speaker::topFrontRight, Speaker::topMiddleLeft, Speaker::topMiddleRight, Speaker::topRearLeft,
               Speaker::topRearRight});
}

bool ChannelLayout::hasHeights() const noexcept
{
    for (int i = 0; i < numChannels; ++i)
        if (isHeight(speakers[static_cast<std::size_t>(i)]))
            return true;
    return false;
}

SpeakerDirection directionOf(const ChannelLayout& layout, Speaker speaker) noexcept
{
    // The 7.x family (with rears) has its surrounds at the sides and its tops at +-45 / +-135.
    const bool sevenFamily = layout.has(Speaker::leftRear) || layout.has(Speaker::rightRear);
    const auto mirrored = [](SpeakerDirection d) { return SpeakerDirection{-d.azimuth, d.elevation}; };
    switch (speaker)
    {
    case Speaker::left:           return {30.0, 0.0};
    case Speaker::right:          return {-30.0, 0.0};
    case Speaker::centre:         return {0.0, 0.0};
    case Speaker::lfe:            return {0.0, -30.0};
    case Speaker::leftSurround:   return {sevenFamily ? 90.0 : 110.0, 0.0};
    case Speaker::rightSurround:  return mirrored(directionOf(layout, Speaker::leftSurround));
    case Speaker::leftRear:       return {135.0, 0.0};
    case Speaker::rightRear:      return {-135.0, 0.0};
    case Speaker::leftWide:       return {60.0, 0.0};
    case Speaker::rightWide:      return {-60.0, 0.0};
    case Speaker::topFrontLeft:   return {sevenFamily ? 45.0 : 30.0, 30.0};
    case Speaker::topFrontRight:  return mirrored(directionOf(layout, Speaker::topFrontLeft));
    case Speaker::topMiddleLeft:  return {90.0, 30.0};
    case Speaker::topMiddleRight: return {-90.0, 30.0};
    case Speaker::topRearLeft:    return {sevenFamily ? 135.0 : 110.0, 30.0};
    case Speaker::topRearRight:   return mirrored(directionOf(layout, Speaker::topRearLeft));
    }
    return {};
}

int ChannelLayout::indexOf(Speaker speaker) const noexcept
{
    for (int i = 0; i < numChannels; ++i)
        if (speakers[static_cast<std::size_t>(i)] == speaker)
            return i;
    return -1;
}

bool ChannelLayout::operator==(const ChannelLayout& other) const noexcept
{
    if (numChannels != other.numChannels)
        return false;
    for (int i = 0; i < numChannels; ++i)
        if (speakers[static_cast<std::size_t>(i)] != other.speakers[static_cast<std::size_t>(i)])
            return false;
    return true;
}

bool ChannelMatrix::isIdentity() const noexcept
{
    if (numInputs != numOutputs)
        return false;
    for (int o = 0; o < numOutputs; ++o)
        for (int i = 0; i < numInputs; ++i)
        {
            const double g = gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)];
            if (std::abs(g - (o == i ? 1.0 : 0.0)) > 1.0e-12)
                return false;
        }
    return true;
}

ChannelMatrix conversionMatrix(const ChannelLayout& from, const ChannelLayout& to) noexcept
{
    ChannelMatrix matrix;
    matrix.numInputs = from.numChannels;
    matrix.numOutputs = to.numChannels;
    for (int i = 0; i < from.numChannels; ++i)
        route(matrix, to, i, from.speakers[static_cast<std::size_t>(i)], 1.0, 0);
    return matrix;
}

ChannelMatrix surroundPanMatrix(const ChannelLayout& to, double pan, double frontRear, double centre) noexcept
{
    ChannelMatrix m;
    m.numInputs = 2;
    m.numOutputs = to.numChannels;
    const auto [gainLeft, gainRight] = balance(pan);

    if (!to.isMultichannel())
    {
        set(m, to, Speaker::left, 0, gainLeft);
        set(m, to, Speaker::right, 1, gainRight);
        if (!to.has(Speaker::left)) // into mono
        {
            set(m, to, Speaker::centre, 0, gainLeft * kMinus3dB);
            set(m, to, Speaker::centre, 1, gainRight * kMinus3dB);
        }
        return m;
    }

    // Front / side / rear weights (equal power).
    const bool surrounds = to.has(Speaker::leftSurround) && to.has(Speaker::rightSurround);
    const bool rears = to.has(Speaker::leftRear) && to.has(Speaker::rightRear);
    const double y = std::clamp(frontRear, -1.0, 1.0);
    double front = 1.0;
    double side = 0.0;
    double rear = 0.0;
    if (surrounds && rears)
    {
        if (y <= 0.0)
        {
            front = std::cos(kHalfPi * (y + 1.0));
            side = std::sin(kHalfPi * (y + 1.0));
        }
        else
        {
            front = 0.0;
            side = std::cos(kHalfPi * y);
            rear = std::sin(kHalfPi * y);
        }
    }
    else if (surrounds || rears)
    {
        front = std::cos(kHalfPi * (y + 1.0) * 0.5);
        (surrounds ? side : rear) = std::sin(kHalfPi * (y + 1.0) * 0.5);
    }

    // The centred part of the front signal moves into the centre speaker (equal power).
    const double centred = to.has(Speaker::centre) ? std::clamp(centre, 0.0, 1.0) * (1.0 - std::min(1.0, std::abs(pan))) : 0.0;
    const double phantom = std::sqrt(1.0 - centred);
    const double hard = std::sqrt(centred) * kMinus3dB;

    set(m, to, Speaker::left, 0, gainLeft * front * phantom);
    set(m, to, Speaker::right, 1, gainRight * front * phantom);
    set(m, to, Speaker::centre, 0, gainLeft * front * hard);
    set(m, to, Speaker::centre, 1, gainRight * front * hard);
    set(m, to, Speaker::leftSurround, 0, gainLeft * side);
    set(m, to, Speaker::rightSurround, 1, gainRight * side);
    set(m, to, Speaker::leftRear, 0, gainLeft * rear);
    set(m, to, Speaker::rightRear, 1, gainRight * rear);
    return m;
}

ChannelMatrix outputMatrix(const ChannelLayout& from, const ChannelLayout& to, double pan, double frontRear, double centre) noexcept
{
    if (from.numChannels == 2 && from.has(Speaker::left) && from.has(Speaker::right))
        return surroundPanMatrix(to, pan, frontRear, centre);

    if (from.numChannels == 1)
    {
        // A one-channel signal: the panner with the same signal on both sides.
        const auto stereo = surroundPanMatrix(to, pan, frontRear, centre);
        ChannelMatrix m;
        m.numInputs = 1;
        m.numOutputs = stereo.numOutputs;
        for (int o = 0; o < m.numOutputs; ++o)
            m.gain[static_cast<std::size_t>(o)][0] = stereo.gain[static_cast<std::size_t>(o)][0] + stereo.gain[static_cast<std::size_t>(o)][1];
        return m;
    }

    auto m = conversionMatrix(from, to);
    const auto [gainLeft, gainRight] = balance(pan);
    for (int i = 0; i < from.numChannels; ++i)
    {
        const int side = sideOf(from.speakers[static_cast<std::size_t>(i)]);
        const double g = side < 0 ? gainLeft : (side > 0 ? gainRight : 1.0);
        for (int o = 0; o < m.numOutputs; ++o)
            m.gain[static_cast<std::size_t>(o)][static_cast<std::size_t>(i)] *= g;
    }
    return m;
}

ChannelGroups channelGroupsOf(const ChannelLayout& layout) noexcept
{
    ChannelGroups result;
    if (layout.numChannels <= 2)
    {
        result.count = 1;
        result.groups[0] = {0, layout.numChannels == 2 ? 1 : -1};
        return result;
    }

    result.count = 0;
    std::array<bool, kMaxChannels> used{};
    const auto addPair = [&](Speaker a, Speaker b)
    {
        const int ia = layout.indexOf(a);
        const int ib = layout.indexOf(b);
        if (ia >= 0 && ib >= 0 && result.count < kMaxChannelGroups)
        {
            result.groups[static_cast<std::size_t>(result.count++)] = {ia, ib};
            used[static_cast<std::size_t>(ia)] = used[static_cast<std::size_t>(ib)] = true;
        }
    };
    addPair(Speaker::left, Speaker::right);
    addPair(Speaker::leftSurround, Speaker::rightSurround);
    addPair(Speaker::leftRear, Speaker::rightRear);
    addPair(Speaker::leftWide, Speaker::rightWide);
    addPair(Speaker::topFrontLeft, Speaker::topFrontRight);
    addPair(Speaker::topMiddleLeft, Speaker::topMiddleRight);
    addPair(Speaker::topRearLeft, Speaker::topRearRight);
    for (int i = 0; i < layout.numChannels; ++i)
        if (!used[static_cast<std::size_t>(i)] && result.count < kMaxChannelGroups)
            result.groups[static_cast<std::size_t>(result.count++)] = {i, -1};
    return result;
}

} // namespace dojo::engine

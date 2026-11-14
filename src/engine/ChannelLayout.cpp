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
    }
    return "?";
}

int sideOf(Speaker speaker) noexcept
{
    switch (speaker)
    {
    case Speaker::left:
    case Speaker::leftSurround:
    case Speaker::leftRear:      return -1;
    case Speaker::right:
    case Speaker::rightSurround:
    case Speaker::rightRear:     return 1;
    case Speaker::centre:
    case Speaker::lfe:           break;
    }
    return 0;
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
    for (int i = 0; i < layout.numChannels; ++i)
        if (!used[static_cast<std::size_t>(i)] && result.count < kMaxChannelGroups)
            result.groups[static_cast<std::size_t>(result.count++)] = {i, -1};
    return result;
}

} // namespace dojo::engine

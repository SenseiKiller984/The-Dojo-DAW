#pragma once

#include <array>
#include <cstdint>
#include <initializer_list>

namespace dojo::engine
{

/** The most channels a track, bus or the master carries (7.1). */
inline constexpr int kMaxChannels = 8;

/** A loudspeaker position. Surrounds are the side pair of 7.1 and the rear pair of 5.1. */
enum class Speaker : std::uint8_t
{
    left,
    right,
    centre,
    lfe,
    leftSurround,
    rightSurround,
    leftRear,
    rightRear,
};

/** Short label of a speaker ("L", "R", "C", "LFE", "Ls", "Rs", "Lrs", "Rrs"). */
[[nodiscard]] const char* shortName(Speaker speaker) noexcept;

/** Which side a speaker is on: -1 left, +1 right, 0 centre or LFE (pan balance acts on the sides). */
[[nodiscard]] int sideOf(Speaker speaker) noexcept;

/**
    The ordered speakers of a signal: channel i of the buffer feeds `speakers[i]`. Formats with the
    same speakers in another order (5.1 SMPTE, Dolby Digital, DTS) are different layouts; signals are
    converted between them by speaker, never by channel number.
*/
struct ChannelLayout
{
    int numChannels = 2;
    std::array<Speaker, kMaxChannels> speakers{Speaker::left, Speaker::right};

    [[nodiscard]] static ChannelLayout of(std::initializer_list<Speaker> list) noexcept;
    [[nodiscard]] static ChannelLayout mono() noexcept { return of({Speaker::centre}); }
    [[nodiscard]] static ChannelLayout stereo() noexcept { return of({Speaker::left, Speaker::right}); }
    [[nodiscard]] static ChannelLayout lcrLfe() noexcept;       ///< 3.1: L R C LFE
    [[nodiscard]] static ChannelLayout surround51() noexcept;   ///< 5.1 (SMPTE/ITU): L R C LFE Ls Rs
    [[nodiscard]] static ChannelLayout surround71() noexcept;   ///< 7.1 (SMPTE): L R C LFE Ls Rs Lrs Rrs
    [[nodiscard]] static ChannelLayout dolbyDigital() noexcept; ///< 5.1 in AC-3 order: L C R Ls Rs LFE
    [[nodiscard]] static ChannelLayout dts() noexcept;          ///< 5.1 in DTS order: L R Ls Rs C LFE

    /** The channel that feeds `speaker`, or -1. */
    [[nodiscard]] int indexOf(Speaker speaker) const noexcept;
    [[nodiscard]] bool has(Speaker speaker) const noexcept { return indexOf(speaker) >= 0; }
    [[nodiscard]] bool isMultichannel() const noexcept { return numChannels > 2; }

    [[nodiscard]] bool operator==(const ChannelLayout& other) const noexcept;
    [[nodiscard]] bool operator!=(const ChannelLayout& other) const noexcept { return !(*this == other); }
};

/** Gains from input channels to output channels: out[o] = sum over i of gain[o][i] * in[i]. */
struct ChannelMatrix
{
    int numInputs = 0;
    int numOutputs = 0;
    std::array<std::array<double, kMaxChannels>, kMaxChannels> gain{};

    [[nodiscard]] bool isIdentity() const noexcept;
};

/**
    Converts a signal between layouts by speaker. Speakers both layouts have pass straight through.
    The others are folded down (ITU-R BS.775 style): the centre into left and right at -3 dB,
    surrounds into the fronts at -3 dB, rears into the surrounds at -3 dB (or the fronts), and the
    LFE is dropped. Into a mono layout everything but the LFE is summed at -3 dB (or 0 dB for the
    centre).
*/
[[nodiscard]] ChannelMatrix conversionMatrix(const ChannelLayout& from, const ChannelLayout& to) noexcept;

/**
    The surround panner: places a stereo signal (a mono track's is dual mono) in a multichannel
    layout. `pan` (-1 left .. +1 right) is the balance of the two channels, as on a stereo bus.
    `frontRear` moves it from the front (-1) to the rear (+1); in 7.1, 0 is the side surrounds.
    `centre` (0..1) moves the centred part of the front signal from the phantom centre (left and
    right) into the centre speaker: at 1, a source panned to the middle plays only from the centre.
    The LFE gets nothing. Into a stereo layout this is the plain balance.
*/
[[nodiscard]] ChannelMatrix surroundPanMatrix(const ChannelLayout& to, double pan, double frontRear, double centre) noexcept;

/**
    The matrix a track's fader applies on the way to its destination: the surround panner for a
    stereo signal, otherwise the conversion by speaker followed by the pan as a balance (left
    speakers by the left gain, right speakers by the right gain).
*/
[[nodiscard]] ChannelMatrix outputMatrix(const ChannelLayout& from, const ChannelLayout& to, double pan, double frontRear,
                                         double centre) noexcept;

/**
    How effects run on a multichannel signal: every effect is a stereo processor, so a multichannel
    track runs one linked instance per channel group - the speaker pairs (L/R, Ls/Rs, Lrs/Rrs) as
    stereo pairs, the centre and the LFE each on their own (as dual mono). Group 0 is always the
    front pair (or the first channels). second == -1 marks a mono group.
*/
struct ChannelGroup
{
    int first = 0;
    int second = 1;
};
inline constexpr int kMaxChannelGroups = 5;
struct ChannelGroups
{
    int count = 1;
    std::array<ChannelGroup, kMaxChannelGroups> groups{};
};
[[nodiscard]] ChannelGroups channelGroupsOf(const ChannelLayout& layout) noexcept;

} // namespace dojo::engine

#pragma once

#include "engine/ChannelLayout.h"
#include "engine/io/Bw64Writer.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace dojo::engine::adm
{

/** A point in the ADM's Cartesian room: x from the left wall (-1) to the right (+1), y from the
    back (-1) to the front (+1), z from the floor (-1) through ear level (0) to the ceiling (+1). */
struct Cartesian
{
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

/**
    A polar position (ADM conventions: azimuth in degrees, positive to the left; elevation in degrees;
    distance 0..1) as a Cartesian one, by the mapping of ITU-R BS.2127 section 10: the loudspeaker
    directions of the standard layouts land on the room's corners and edges (M+030 at the front left
    corner, M+110 at the back left corner, the top layer at 30 degrees on the ceiling), azimuths in
    between are spread along the walls by a tangent law, and elevations above 30 degrees come in
    across the ceiling.
*/
[[nodiscard]] Cartesian polarToCartesian(double azimuth, double elevation, double distance) noexcept;

/** The ITU-R BS.2094 common definition (its audioChannelFormat number, e.g. 0x0001 for
    AC_00010001, FrontLeft) of a speaker of a layout, or 0 if there is none. */
[[nodiscard]] int commonChannelFormat(const ChannelLayout& layout, Speaker speaker) noexcept;
/** The common audioPackFormat (e.g. 0x0003 for AP_00010003, 5.1) holding exactly a layout's
    speakers (in any order), or 0 if there is none (3.1, 9.1.6: the file gets a pack of its own). */
[[nodiscard]] int commonPackFormat(const ChannelLayout& layout) noexcept;

/** An object over a stretch of the programme: an audioBlockFormat. The renderer moves it from the
    previous block's position to this one's over the block (the first block starts there). */
struct ObjectBlock
{
    std::int64_t start = 0;  ///< samples from the programme's start
    std::int64_t length = 0; ///< samples
    Cartesian position;
    double size = 0.0;       ///< its width, height and depth (0..1)
    double gain = 1.0;       ///< linear
};

/** One mono object: a channel of the file and its blocks. */
struct Object
{
    juce::String name;
    int fileTrack = 1; ///< its channel in the file, counted from 1
    std::vector<ObjectBlock> blocks;
};

/** The objects of one track (one audioContent): one for a mono track, a left and a right one for a
    stereo track. */
struct ObjectGroup
{
    juce::String name;
    std::vector<Object> objects;
};

/** What an ADM BWF file holds: a bed (DirectSpeakers) and objects. */
struct Programme
{
    juce::String name;
    double sampleRate = 48000.0;
    int bitDepth = 24;
    std::int64_t length = 0; ///< samples
    ChannelLayout bed;       ///< no channels: no bed
    std::vector<int> bedTracks; ///< the file channel (from 1) of each of the bed's speakers
    std::vector<ObjectGroup> groups;
};

/** Which ADM track each file channel carries (the chna chunk): the bed's from the common
    definitions, the objects' from the programme's own formats. UIDs follow the file's channels. */
[[nodiscard]] std::vector<TrackEntry> trackEntries(const Programme& programme);
/** The ADM document (ITU-R BS.2076-2, EBUCore wrapper) for the axml chunk: the programme, a
    content per bed and object track, an audioObject per bed and mono object, the bed's pack (a
    common one, or one of the file's own referring to common channels), and each object's pack,
    channel format with its blocks (Cartesian), stream and track formats, and every track UID. */
[[nodiscard]] juce::String documentFor(const Programme& programme);

/** Fewer blocks for the same movement: consecutive blocks that stay put become one, and blocks the
    renderer's linear movement between their neighbours passes within `tolerance` (room units, for
    the position and the size) are left out. The blocks stay contiguous and cover the same time. */
[[nodiscard]] std::vector<ObjectBlock> simplify(const std::vector<ObjectBlock>& blocks, double tolerance);

/** An ADM time ("hh:mm:ss.zzzzz", to 10 microseconds) for a number of 10-microsecond ticks. */
[[nodiscard]] juce::String formatTime(std::int64_t ticks);
/** A sample position in 10-microsecond ticks (rounded: blocks stay contiguous). */
[[nodiscard]] std::int64_t ticksOf(std::int64_t samples, double sampleRate) noexcept;

} // namespace dojo::engine::adm

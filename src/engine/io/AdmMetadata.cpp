#include "engine/io/AdmMetadata.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace dojo::engine::adm
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
double rad(double degrees) { return degrees * kPi / 180.0; }

// --- BS.2127 section 10 -------------------------------------------------------------------------------

/** `y` moved by whole turns to the first angle at or after `x` (angles grow anticlockwise). */
double relativeAngle(double x, double y)
{
    while (y - 360.0 >= x)
        y -= 360.0;
    while (y < x)
        y += 360.0;
    return y;
}

/** Whether `x` lies between `start` and `end` going anticlockwise from start. */
bool insideAngleRange(double x, double start, double end)
{
    while (end - 360.0 > start)
        end -= 360.0;
    while (end < start)
        end += 360.0;
    while (x - 360.0 >= start)
        x -= 360.0;
    while (x < start)
        x += 360.0;
    return x <= end;
}

/** The directions that map onto the room's corners and the middle of its front wall. */
struct Corner
{
    double azimuth;
    double x, y;
};
constexpr std::array<Corner, 5> kCorners{{{0.0, 0.0, 1.0}, {-30.0, 1.0, 1.0}, {-110.0, 1.0, -1.0}, {110.0, -1.0, -1.0}, {30.0, -1.0, 1.0}}};
constexpr double kTopElevation = 30.0;       ///< the top layer's elevation...
constexpr double kTopElevationMapped = 45.0; ///< ...reaches the ceiling's edge

// --- Common definitions (ITU-R BS.2094) -------------------------------------------------------------------

struct CommonChannel
{
    double azimuth, elevation;
    int id;
};
constexpr std::array<CommonChannel, 21> kCommonChannels{{
    {30.0, 0.0, 0x01},   {-30.0, 0.0, 0x02},  {0.0, 0.0, 0x03},     {110.0, 0.0, 0x05},  {-110.0, 0.0, 0x06},
    {90.0, 0.0, 0x0a},   {-90.0, 0.0, 0x0b},  {135.0, 0.0, 0x1c},   {-135.0, 0.0, 0x1d}, {60.0, 0.0, 0x18},
    {-60.0, 0.0, 0x19},  {30.0, 30.0, 0x0d},  {-30.0, 30.0, 0x0f},  {110.0, 30.0, 0x10}, {-110.0, 30.0, 0x12},
    {45.0, 30.0, 0x22},  {-45.0, 30.0, 0x23}, {90.0, 30.0, 0x13},   {-90.0, 30.0, 0x14}, {135.0, 30.0, 0x1e},
    {-135.0, 30.0, 0x1f},
}};
constexpr int kLfe = 0x04;

struct CommonPack
{
    int id;
    std::vector<int> channels; // (sorted)
};
const std::vector<CommonPack>& commonPacks()
{
    static const std::vector<CommonPack> packs{
        {0x01, {0x03}},                                                             // mono (0+1+0)
        {0x02, {0x01, 0x02}},                                                       // stereo (0+2+0)
        {0x0a, {0x01, 0x02, 0x03}},                                                 // 3.0 (0+3+0)
        {0x03, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06}},                               // 5.1 (0+5+0)
        {0x0f, {0x01, 0x02, 0x03, 0x04, 0x0a, 0x0b, 0x1c, 0x1d}},                   // 7.1 back (0+7+0)
        {0x05, {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x0d, 0x0f, 0x10, 0x12}},       // 5.1.4 (4+5+0)
        {0x17, {0x01, 0x02, 0x03, 0x04, 0x0a, 0x0b, 0x1c, 0x1d, 0x1e, 0x1f, 0x22, 0x23}}, // 7.1.4 (4+7+0)
    };
    return packs;
}

constexpr int kFirstCustom = 0x1001; ///< (IDs below are reserved for the common definitions)
const char* const kCustomBedPack = "AP_00011001";

juce::String hex(int value, int digits) { return juce::String::toHexString(value).paddedLeft('0', digits); }
juce::String uidOf(int fileTrack) { return "ATU_" + hex(fileTrack, 8); }
juce::String objectNumber(int index) { return hex(kFirstCustom + index, 4); } ///< the n-th object's "xxxx"

juce::String escaped(const juce::String& text)
{
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace("\"", "&quot;").replace("'", "&apos;");
}

juce::String number(double value)
{
    if (std::abs(value) < 5.0e-7)
        value = 0.0; // (no "-0.000000")
    return juce::String(value, 6);
}

juce::String bedPackId(const ChannelLayout& bed)
{
    const int common = commonPackFormat(bed);
    return common != 0 ? "AP_0001" + hex(common, 4) : juce::String(kCustomBedPack);
}

bool samePlace(const ObjectBlock& a, const ObjectBlock& b, double tolerance)
{
    return std::abs(a.position.x - b.position.x) <= tolerance && std::abs(a.position.y - b.position.y) <= tolerance
           && std::abs(a.position.z - b.position.z) <= tolerance && std::abs(a.size - b.size) <= tolerance
           && std::abs(a.gain - b.gain) <= tolerance;
}

/** Whether the movement from block a's position (reached at its end) to block b's passes every
    block in between within the tolerance. */
bool onLine(const std::vector<ObjectBlock>& blocks, std::size_t a, std::size_t b, double tolerance)
{
    const auto endOf = [&](std::size_t k) { return static_cast<double>(blocks[k].start + blocks[k].length); };
    const double from = endOf(a);
    const double span = endOf(b) - from;
    if (!(span > 0.0))
        return false;
    const auto& p = blocks[a];
    const auto& q = blocks[b];
    for (std::size_t k = a + 1; k < b; ++k)
    {
        const double t = (endOf(k) - from) / span;
        const auto at = [t](double u, double v) { return u + (v - u) * t; };
        ObjectBlock expected;
        expected.position = {at(p.position.x, q.position.x), at(p.position.y, q.position.y), at(p.position.z, q.position.z)};
        expected.size = at(p.size, q.size);
        expected.gain = at(p.gain, q.gain);
        if (!samePlace(expected, blocks[k], tolerance))
            return false;
    }
    return true;
}
} // namespace

Cartesian polarToCartesian(double azimuth, double elevation, double distance) noexcept
{
    double z = 0.0;
    double radius = distance;
    if (std::abs(elevation) > kTopElevation)
    {
        const double mapped = kTopElevationMapped
                              + (90.0 - kTopElevationMapped) * (std::abs(elevation) - kTopElevation) / (90.0 - kTopElevation);
        z = elevation > 0.0 ? distance : -distance;
        radius = distance * std::tan(rad(90.0 - mapped));
    }
    else
    {
        z = std::tan(rad(kTopElevationMapped * elevation / kTopElevation)) * distance;
    }

    for (std::size_t i = 0; i < kCorners.size(); ++i)
    {
        const auto& left = kCorners[i];
        const auto& right = kCorners[(i + 1) % kCorners.size()];
        if (!insideAngleRange(azimuth, right.azimuth, left.azimuth))
            continue;
        // Within the sector, a tangent law spreads the direction along the wall between the corners.
        const double rightAzimuth = right.azimuth;
        const double leftAzimuth = relativeAngle(rightAzimuth, left.azimuth);
        const double middle = (leftAzimuth + rightAzimuth) / 2.0;
        const double half = rightAzimuth - middle;
        const double towardsRight = 0.5 + 0.5 * std::tan(rad(relativeAngle(rightAzimuth, azimuth) - middle)) / std::tan(rad(half));
        const double p = std::atan2(towardsRight, 1.0 - towardsRight) * (2.0 / kPi);
        return {radius * (left.x + (right.x - left.x) * p), radius * (left.y + (right.y - left.y) * p), z};
    }
    return {0.0, radius, z};
}

int commonChannelFormat(const ChannelLayout& layout, Speaker speaker) noexcept
{
    if (speaker == Speaker::lfe)
        return kLfe;
    const auto direction = directionOf(layout, speaker);
    for (const auto& c : kCommonChannels)
        if (std::abs(c.azimuth - direction.azimuth) < 0.5 && std::abs(c.elevation - direction.elevation) < 0.5)
            return c.id;
    return 0;
}

int commonPackFormat(const ChannelLayout& layout) noexcept
{
    std::vector<int> channels;
    for (int c = 0; c < layout.numChannels; ++c)
    {
        const int id = commonChannelFormat(layout, layout.speakers[static_cast<std::size_t>(c)]);
        if (id == 0)
            return 0;
        channels.push_back(id);
    }
    std::sort(channels.begin(), channels.end());
    for (const auto& pack : commonPacks())
        if (pack.channels == channels)
            return pack.id;
    return 0;
}

std::int64_t ticksOf(std::int64_t samples, double sampleRate) noexcept
{
    return std::llround(static_cast<double>(samples) * 100000.0 / sampleRate);
}

juce::String formatTime(std::int64_t ticks)
{
    ticks = std::max<std::int64_t>(0, ticks);
    const auto fraction = static_cast<int>(ticks % 100000);
    const auto seconds = static_cast<int>((ticks / 100000) % 60);
    const auto minutes = static_cast<int>((ticks / 6000000) % 60);
    const auto hours = static_cast<int>(ticks / 360000000);
    return juce::String(hours).paddedLeft('0', 2) + ":" + juce::String(minutes).paddedLeft('0', 2) + ":"
           + juce::String(seconds).paddedLeft('0', 2) + "." + juce::String(fraction).paddedLeft('0', 5);
}

std::vector<ObjectBlock> simplify(const std::vector<ObjectBlock>& blocks, double tolerance)
{
    if (blocks.size() < 2)
        return blocks;

    // Blocks that stay put join the one before them, if that one stays put too (one that moves keeps
    // its own time: the movement must not stretch).
    std::vector<ObjectBlock> held;
    for (const auto& block : blocks)
    {
        const bool stillBefore = held.size() == 1 || (held.size() > 1 && samePlace(held[held.size() - 2], held.back(), 1.0e-12));
        if (!held.empty() && stillBefore && samePlace(held.back(), block, 1.0e-12))
        {
            held.back().length = block.start + block.length - held.back().start;
            continue;
        }
        held.push_back(block);
    }

    // Blocks the movement between their neighbours passes near are left out (at most a few hundred
    // at a time: the check is quadratic).
    constexpr std::size_t kWindow = 512;
    std::vector<ObjectBlock> result{held.front()};
    std::size_t anchor = 0;
    while (anchor + 1 < held.size())
    {
        std::size_t reach = anchor + 1;
        for (std::size_t candidate = anchor + 2; candidate < held.size() && candidate - anchor <= kWindow; ++candidate)
        {
            if (!onLine(held, anchor, candidate, tolerance))
                break;
            reach = candidate;
        }
        ObjectBlock kept = held[reach];
        const auto end = kept.start + kept.length;
        kept.start = result.back().start + result.back().length;
        kept.length = end - kept.start;
        result.push_back(kept);
        anchor = reach;
    }
    return result;
}

std::vector<TrackEntry> trackEntries(const Programme& programme)
{
    std::vector<TrackEntry> entries;
    const auto& bed = programme.bed;
    if (bed.numChannels > 0)
    {
        const auto pack = bedPackId(bed);
        for (int c = 0; c < bed.numChannels && c < static_cast<int>(programme.bedTracks.size()); ++c)
        {
            const int channel = commonChannelFormat(bed, bed.speakers[static_cast<std::size_t>(c)]);
            const int track = programme.bedTracks[static_cast<std::size_t>(c)];
            entries.push_back({track, uidOf(track), "AT_0001" + hex(channel, 4) + "_01", pack});
        }
    }
    int index = 0;
    for (const auto& group : programme.groups)
        for (const auto& object : group.objects)
        {
            const auto n = objectNumber(index++);
            entries.push_back({object.fileTrack, uidOf(object.fileTrack), "AT_0003" + n + "_01", "AP_0003" + n});
        }
    std::sort(entries.begin(), entries.end(), [](const TrackEntry& a, const TrackEntry& b) { return a.trackIndex < b.trackIndex; });
    return entries;
}

juce::String documentFor(const Programme& programme)
{
    juce::MemoryOutputStream x;
    const auto line = [&x](int depth, const juce::String& text) { x << juce::String::repeatedString("  ", depth) << text << "\n"; };
    const auto element = [&](int depth, const juce::String& tag, const juce::String& value)
    { line(depth, "<" + tag + ">" + value + "</" + tag.upToFirstOccurrenceOf(" ", false, false) + ">"); };

    const auto end = formatTime(ticksOf(programme.length, programme.sampleRate));
    const auto zero = formatTime(0);
    const auto& bed = programme.bed;
    const bool hasBed = bed.numChannels > 0;
    const auto rate = juce::String(static_cast<int>(std::lround(programme.sampleRate)));
    const auto bits = juce::String(programme.bitDepth);

    line(0, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>");
    line(0, "<ebuCoreMain xmlns:dc=\"http://purl.org/dc/elements/1.1/\" xmlns=\"urn:ebu:metadata-schema:ebuCore_2016\" "
            "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
            "xsi:schemaLocation=\"urn:ebu:metadata-schema:ebuCore_2016 ebucore.xsd\" xml:lang=\"en\">");
    line(1, "<coreMetadata>");
    line(2, "<format>");
    line(3, "<audioFormatExtended version=\"ITU-R_BS.2076-2\">");

    // The programme and its contents: the bed's, then one per object track.
    line(4, "<audioProgramme audioProgrammeID=\"APR_1001\" audioProgrammeName=\"" + escaped(programme.name) + "\" start=\"" + zero
                + "\" end=\"" + end + "\">");
    int content = 0;
    if (hasBed)
        element(5, "audioContentIDRef", "ACO_" + hex(kFirstCustom + content++, 4));
    for (std::size_t g = 0; g < programme.groups.size(); ++g)
        element(5, "audioContentIDRef", "ACO_" + hex(kFirstCustom + content++, 4));
    line(4, "</audioProgramme>");

    content = 0;
    int object = 0;
    if (hasBed)
    {
        line(4, "<audioContent audioContentID=\"ACO_" + hex(kFirstCustom + content++, 4) + "\" audioContentName=\"Bed\">");
        element(5, "audioObjectIDRef", "AO_" + hex(kFirstCustom + object++, 4));
        line(4, "</audioContent>");
    }
    for (const auto& group : programme.groups)
    {
        line(4, "<audioContent audioContentID=\"ACO_" + hex(kFirstCustom + content++, 4) + "\" audioContentName=\"" + escaped(group.name) + "\">");
        for (std::size_t o = 0; o < group.objects.size(); ++o)
            element(5, "audioObjectIDRef", "AO_" + hex(kFirstCustom + object++, 4));
        line(4, "</audioContent>");
    }

    // The objects: the bed's (its speakers' tracks), then each mono object.
    object = 0;
    if (hasBed)
    {
        line(4, "<audioObject audioObjectID=\"AO_" + hex(kFirstCustom + object++, 4) + "\" audioObjectName=\"Bed\" start=\"" + zero
                    + "\" duration=\"" + end + "\">");
        element(5, "audioPackFormatIDRef", bedPackId(bed));
        for (const int track : programme.bedTracks)
            element(5, "audioTrackUIDRef", uidOf(track));
        line(4, "</audioObject>");
    }
    int index = 0;
    for (const auto& group : programme.groups)
        for (const auto& o : group.objects)
        {
            line(4, "<audioObject audioObjectID=\"AO_" + hex(kFirstCustom + object++, 4) + "\" audioObjectName=\"" + escaped(o.name)
                        + "\" start=\"" + zero + "\" duration=\"" + end + "\">");
            element(5, "audioPackFormatIDRef", "AP_0003" + objectNumber(index++));
            element(5, "audioTrackUIDRef", uidOf(o.fileTrack));
            line(4, "</audioObject>");
        }

    // Packs: the bed's own, if no common one fits, then the objects'.
    if (hasBed && commonPackFormat(bed) == 0)
    {
        line(4, "<audioPackFormat audioPackFormatID=\"" + juce::String(kCustomBedPack) + "\" audioPackFormatName=\"Bed "
                    + juce::String(bed.numChannels) + " channels\" typeLabel=\"0001\" typeDefinition=\"DirectSpeakers\">");
        for (int c = 0; c < bed.numChannels; ++c)
            element(5, "audioChannelFormatIDRef", "AC_0001" + hex(commonChannelFormat(bed, bed.speakers[static_cast<std::size_t>(c)]), 4));
        line(4, "</audioPackFormat>");
    }
    index = 0;
    for (const auto& group : programme.groups)
        for (const auto& o : group.objects)
        {
            const auto n = objectNumber(index++);
            line(4, "<audioPackFormat audioPackFormatID=\"AP_0003" + n + "\" audioPackFormatName=\"" + escaped(o.name)
                        + "\" typeLabel=\"0003\" typeDefinition=\"Objects\">");
            element(5, "audioChannelFormatIDRef", "AC_0003" + n);
            line(4, "</audioPackFormat>");
        }

    // The objects' channels with their blocks (Cartesian: the room's coordinates).
    index = 0;
    for (const auto& group : programme.groups)
        for (const auto& o : group.objects)
        {
            const auto n = objectNumber(index++);
            line(4, "<audioChannelFormat audioChannelFormatID=\"AC_0003" + n + "\" audioChannelFormatName=\"" + escaped(o.name)
                        + "\" typeLabel=\"0003\" typeDefinition=\"Objects\">");
            int block = 1;
            for (const auto& b : o.blocks)
            {
                const auto from = ticksOf(b.start, programme.sampleRate);
                const auto to = ticksOf(b.start + b.length, programme.sampleRate);
                line(5, "<audioBlockFormat audioBlockFormatID=\"AB_0003" + n + "_" + hex(block++, 8) + "\" rtime=\"" + formatTime(from)
                            + "\" duration=\"" + formatTime(to - from) + "\">");
                element(6, "cartesian", "1");
                element(6, "position coordinate=\"X\"", number(b.position.x));
                element(6, "position coordinate=\"Y\"", number(b.position.y));
                element(6, "position coordinate=\"Z\"", number(b.position.z));
                element(6, "width", number(b.size));
                element(6, "height", number(b.size));
                element(6, "depth", number(b.size));
                element(6, "gain", number(b.gain));
                line(5, "</audioBlockFormat>");
            }
            line(4, "</audioChannelFormat>");
        }

    // Their stream and track formats (PCM).
    index = 0;
    for (const auto& group : programme.groups)
        for (const auto& o : group.objects)
        {
            const auto n = objectNumber(index++);
            line(4, "<audioStreamFormat audioStreamFormatID=\"AS_0003" + n + "\" audioStreamFormatName=\"PCM_" + escaped(o.name)
                        + "\" formatLabel=\"0001\" formatDefinition=\"PCM\">");
            element(5, "audioChannelFormatIDRef", "AC_0003" + n);
            element(5, "audioTrackFormatIDRef", "AT_0003" + n + "_01");
            line(4, "</audioStreamFormat>");
        }
    index = 0;
    for (const auto& group : programme.groups)
        for (const auto& o : group.objects)
        {
            const auto n = objectNumber(index++);
            line(4, "<audioTrackFormat audioTrackFormatID=\"AT_0003" + n + "_01\" audioTrackFormatName=\"PCM_" + escaped(o.name)
                        + "\" formatLabel=\"0001\" formatDefinition=\"PCM\">");
            element(5, "audioStreamFormatIDRef", "AS_0003" + n);
            line(4, "</audioTrackFormat>");
        }

    // Every track of the file.
    for (const auto& entry : trackEntries(programme))
    {
        line(4, "<audioTrackUID UID=\"" + entry.uid + "\" sampleRate=\"" + rate + "\" bitDepth=\"" + bits + "\">");
        element(5, "audioTrackFormatIDRef", entry.trackRef);
        element(5, "audioPackFormatIDRef", entry.packRef);
        line(4, "</audioTrackUID>");
    }

    line(3, "</audioFormatExtended>");
    line(2, "</format>");
    line(1, "</coreMetadata>");
    line(0, "</ebuCoreMain>");
    return x.toUTF8();
}

} // namespace dojo::engine::adm

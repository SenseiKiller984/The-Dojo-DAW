#include "engine/io/AdmMetadata.h"
#include "engine/io/Bw64Writer.h"
#include "Bw64Reader.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>
#include <set>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using engine::ChannelLayout;
using engine::Speaker;
namespace adm = engine::adm;
using namespace dojo::test::bw64;

namespace
{
std::vector<std::vector<float>> ramps(int channels, int frames)
{
    std::vector<std::vector<float>> audio(static_cast<std::size_t>(channels), std::vector<float>(static_cast<std::size_t>(frames)));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
            audio[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)] = 0.9f * std::sin(0.01f * static_cast<float>(i * (c + 1)));
    return audio;
}

const juce::XmlElement* child(const juce::XmlElement& parent, const char* tag)
{
    const auto* found = parent.getChildByName(tag);
    REQUIRE(found != nullptr);
    return found;
}
} // namespace

TEST_CASE("ADM coordinates: polar to Cartesian as ITU-R BS.2127 maps them", "[adm]")
{
    // Reference values from the BS.2127 section 10 equations (azimuth positive to the left).
    struct Case
    {
        double az, el, d, x, y, z;
    };
    const Case cases[] = {
        {0.0, 0.0, 1.00, 0.0, 1.0, 0.0},
        {30.0, 0.0, 1.00, -1.0, 1.0, 0.0},
        {-30.0, 0.0, 1.00, 1.0, 1.0, 0.0},
        {110.0, 0.0, 1.00, -1.0, -1.0, 0.0},
        {-110.0, 0.0, 1.00, 1.0, -1.0, 0.0},
        {180.0, 0.0, 1.00, 0.0, -1.0, 0.0},
        {-180.0, 0.0, 1.00, 0.0, -1.0, 0.0},
        {90.0, 0.0, 1.00, -1.0, -0.521097949783, 0.0},
        {-90.0, 0.0, 1.00, 1.0, -0.521097949783, 0.0},
        {45.0, 30.0, 1.00, -1.0, 0.645821576268, 1.0},
        {-135.0, 30.0, 1.00, 0.444444444444, -1.0, 1.0},
        {30.0, 30.0, 1.00, -1.0, 1.0, 1.0},
        {0.0, 90.0, 1.00, 0.0, 0.0, 1.0},
        {60.0, 60.0, 0.50, -0.207106781187, 0.054618057319, 0.5},
        {15.0, -20.0, 0.80, -0.4, 0.8, -0.461880215352},
        {-45.0, 10.0, 0.30, 0.3, 0.193746472880, 0.080384757729},
        {135.0, 0.0, 1.00, -0.444444444444, -1.0, 0.0},
        {170.0, 45.0, 1.00, -0.054524557644, -0.668178637919, 1.0},
    };
    for (const auto& c : cases)
    {
        const auto p = adm::polarToCartesian(c.az, c.el, c.d);
        INFO("az " << c.az << " el " << c.el << " d " << c.d);
        CHECK_THAT(p.x, WithinAbs(c.x, 1e-9));
        CHECK_THAT(p.y, WithinAbs(c.y, 1e-9));
        CHECK_THAT(p.z, WithinAbs(c.z, 1e-9));
    }
    // Continuous round the room, at every height.
    for (const double el : {-60.0, 0.0, 20.0, 45.0})
        for (double az = -180.0; az < 180.0; az += 1.0)
        {
            const auto a = adm::polarToCartesian(az, el, 1.0);
            const auto b = adm::polarToCartesian(az + 1.0, el, 1.0);
            CHECK(std::hypot(a.x - b.x, a.y - b.y) < 0.06);
        }
}

TEST_CASE("ADM common definitions: every bed speaker, and the packs", "[adm]")
{
    // Each speaker of every bed has its common audioChannelFormat, at the speaker's direction.
    const std::map<int, std::pair<double, double>> directions{
        {0x01, {30, 0}},   {0x02, {-30, 0}},   {0x03, {0, 0}},     {0x05, {110, 0}},   {0x06, {-110, 0}},   {0x0a, {90, 0}},
        {0x0b, {-90, 0}},  {0x1c, {135, 0}},   {0x1d, {-135, 0}},  {0x18, {60, 0}},    {0x19, {-60, 0}},    {0x0d, {30, 30}},
        {0x0f, {-30, 30}}, {0x10, {110, 30}},  {0x12, {-110, 30}}, {0x22, {45, 30}},   {0x23, {-45, 30}},   {0x13, {90, 30}},
        {0x14, {-90, 30}}, {0x1e, {135, 30}},  {0x1f, {-135, 30}},
    };
    for (const auto& layout : {ChannelLayout::stereo(), ChannelLayout::lcrLfe(), ChannelLayout::surround51(), ChannelLayout::surround71(),
                               ChannelLayout::dolbyDigital(), ChannelLayout::dts(), ChannelLayout::surround514(),
                               ChannelLayout::surround714(), ChannelLayout::surround916()})
        for (int c = 0; c < layout.numChannels; ++c)
        {
            const auto speaker = layout.speakers[static_cast<std::size_t>(c)];
            const int id = adm::commonChannelFormat(layout, speaker);
            INFO(layout.numChannels << " channels, speaker " << engine::shortName(speaker));
            if (speaker == Speaker::lfe)
            {
                CHECK(id == 0x04);
                continue;
            }
            REQUIRE(directions.count(id) == 1);
            const auto d = engine::directionOf(layout, speaker);
            CHECK(directions.at(id).first == d.azimuth);
            CHECK(directions.at(id).second == d.elevation);
        }
    CHECK(adm::commonPackFormat(ChannelLayout::stereo()) == 0x02);
    CHECK(adm::commonPackFormat(ChannelLayout::surround51()) == 0x03);
    CHECK(adm::commonPackFormat(ChannelLayout::dolbyDigital()) == 0x03); // (any order: chna says which is which)
    CHECK(adm::commonPackFormat(ChannelLayout::dts()) == 0x03);
    CHECK(adm::commonPackFormat(ChannelLayout::surround71()) == 0x0f);
    CHECK(adm::commonPackFormat(ChannelLayout::surround514()) == 0x05);
    CHECK(adm::commonPackFormat(ChannelLayout::surround714()) == 0x17);
    CHECK(adm::commonPackFormat(ChannelLayout::surround916()) == 0); // (no common 9.1.6: a pack of the file's own)
    CHECK(adm::commonPackFormat(ChannelLayout::lcrLfe()) == 0);
}

TEST_CASE("BW64 writer: JUNK, fmt, chna, data, axml in that order, sizes patched", "[adm][bw64]")
{
    test::TempFolder temp;
    const auto file = temp.dir.getChildFile("three.wav");
    const std::vector<adm::TrackEntry> tracks{{1, "ATU_00000001", "AT_00010001_01", "AP_00010002"},
                                              {2, "ATU_00000002", "AT_00010002_01", "AP_00010002"},
                                              {3, "ATU_00000003", "AT_00031001_01", "AP_00031001"}};
    const auto audio = ramps(3, 1000);
    {
        adm::Bw64Writer writer(file, 3, 48000.0, adm::Bw64Writer::Format::pcm24, tracks);
        REQUIRE(writer.isOpen());
        std::array<const float*, 3> channels{audio[0].data(), audio[1].data(), audio[2].data()};
        REQUIRE(writer.write(channels.data(), 600));
        std::array<const float*, 3> rest{audio[0].data() + 600, audio[1].data() + 600, audio[2].data() + 600};
        REQUIRE(writer.write(rest.data(), 400));
        CHECK(writer.getFramesWritten() == 1000);
        REQUIRE(writer.finish("<adm>three</adm>"));
    }

    const auto parsed = parse(file);
    CHECK(parsed.header == "RIFF");
    CHECK(parsed.headerSize == file.getSize() - 8);
    REQUIRE(parsed.chunks.size() == 5);
    CHECK(parsed.chunks[0].id == "JUNK");
    CHECK(parsed.chunks[0].size == 28);
    CHECK(parsed.chunks[1].id == "fmt ");
    CHECK(parsed.chunks[2].id == "chna");
    CHECK(parsed.chunks[3].id == "data");
    CHECK(parsed.chunks[4].id == "axml");

    const auto& fmt = parsed.chunks[1].data;
    CHECK(u16(fmt, 0) == 1);          // PCM
    CHECK(u16(fmt, 2) == 3);          // channels
    CHECK(u32(fmt, 4) == 48000);
    CHECK(u32(fmt, 8) == 48000 * 9);  // bytes per second
    CHECK(u16(fmt, 12) == 9);         // block align
    CHECK(u16(fmt, 14) == 24);

    const auto entries = chnaEntries(parsed.chunks[2]);
    CHECK(u16(parsed.chunks[2].data, 0) == 3); // numTracks
    REQUIRE(entries.size() == 3);
    for (std::size_t i = 0; i < entries.size(); ++i)
    {
        CHECK(entries[i].trackIndex == tracks[i].trackIndex);
        CHECK(entries[i].uid == tracks[i].uid);
        CHECK(entries[i].trackRef == tracks[i].trackRef);
        CHECK(entries[i].packRef == tracks[i].packRef);
    }
    CHECK(parsed.chunks[3].size == 1000 * 9);
    CHECK(text(parsed.chunks[4].data, 0, static_cast<std::size_t>(parsed.chunks[4].size)) == "<adm>three</adm>");

    // An ordinary WAV reader plays it (the ADM chunks are skipped).
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatReader> reader(wav.createReaderFor(new juce::FileInputStream(file), true));
    REQUIRE(reader != nullptr);
    CHECK(reader->numChannels == 3);
    CHECK(reader->lengthInSamples == 1000);
    juce::AudioBuffer<float> back(3, 1000);
    REQUIRE(reader->read(&back, 0, 1000, 0, true, true));
    for (int c = 0; c < 3; ++c)
        for (int i = 0; i < 1000; i += 37)
            CHECK_THAT(back.getSample(c, i), WithinAbs(audio[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)], 2.0 / 8388607.0));
}

TEST_CASE("BW64 writer: beyond 4 GB the header says BW64 and ds64 holds the sizes", "[adm][bw64]")
{
    test::TempFolder temp;
    const auto file = temp.dir.getChildFile("big.wav");
    const auto audio = ramps(2, 500);
    {
        adm::Bw64Writer writer(file, 2, 48000.0, adm::Bw64Writer::Format::pcm24,
                               {{1, "ATU_00000001", "AT_00010001_01", "AP_00010002"}, {2, "ATU_00000002", "AT_00010002_01", "AP_00010002"}});
        writer.setAlwaysBw64(true); // (as a file over 4 GB would)
        std::array<const float*, 2> channels{audio[0].data(), audio[1].data()};
        REQUIRE(writer.write(channels.data(), 500));
        REQUIRE(writer.finish("<adm/>"));
    }
    const auto parsed = parse(file);
    CHECK(parsed.header == "BW64");
    CHECK(parsed.headerSize == 0xFFFFFFFFu);
    REQUIRE(parsed.chunks.front().id == "ds64");
    const auto& ds64 = parsed.chunks.front().data;
    CHECK(u64(ds64, 0) == static_cast<std::uint64_t>(file.getSize()) - 8); // the RIFF size
    CHECK(u64(ds64, 8) == 500 * 6);                                        // the data size
    CHECK(u32(ds64, 24) == 0);                                             // no table
    const auto* data = parsed.find("data");
    REQUIRE(data != nullptr);
    CHECK(data->size == 500 * 6); // (its own size field says 0xFFFFFFFF: read from ds64)
    CHECK(parsed.find("axml") != nullptr);
}

TEST_CASE("BW64 writer: 16-bit, float and odd sizes (padded)", "[adm][bw64]")
{
    test::TempFolder temp;
    const auto audio = ramps(1, 3);
    for (const auto format : {adm::Bw64Writer::Format::pcm16, adm::Bw64Writer::Format::pcm24, adm::Bw64Writer::Format::float32})
    {
        const auto file = temp.dir.getChildFile("odd.wav");
        {
            adm::Bw64Writer writer(file, 1, 44100.0, format, {{1, "ATU_00000001", "AT_00031001_01", "AP_00031001"}});
            const float* channel = audio[0].data();
            REQUIRE(writer.write(&channel, 3));
            REQUIRE(writer.finish("<a>odd</a>!")); // (11 bytes: padded too)
        }
        const auto parsed = parse(file); // (fails if a chunk is not on an even byte)
        const auto* fmt = parsed.find("fmt ");
        REQUIRE(fmt != nullptr);
        const int bytes = format == adm::Bw64Writer::Format::pcm16 ? 2 : format == adm::Bw64Writer::Format::pcm24 ? 3 : 4;
        CHECK(u16(fmt->data, 0) == (format == adm::Bw64Writer::Format::float32 ? 3 : 1));
        CHECK(u16(fmt->data, 14) == bytes * 8);
        CHECK(fmt->size == (format == adm::Bw64Writer::Format::float32 ? 18u : 16u));
        CHECK(parsed.find("data")->size == static_cast<std::uint64_t>(3 * bytes));
        CHECK(text(parsed.find("axml")->data, 0, 11) == "<a>odd</a>!");
        if (format == adm::Bw64Writer::Format::float32)
        {
            float v = 0.0f;
            std::memcpy(&v, parsed.find("data")->data.getData(), 4);
            CHECK(v == audio[0][0]);
        }
    }
}

TEST_CASE("ADM document: a 7.1.4 bed and mono and stereo objects", "[adm]")
{
    adm::Programme programme;
    programme.name = "Song & <Mix>";
    programme.sampleRate = 48000.0;
    programme.length = 48000 * 10;
    programme.bed = ChannelLayout::surround714();
    for (int c = 1; c <= 12; ++c)
        programme.bedTracks.push_back(c);
    adm::ObjectGroup vocal{"Vocal", {{"Vocal", 13, {{0, 48000, {0.0, 1.0, 0.0}, 0.0, 1.0}, {48000, 48000, {-1.0, 1.0, 1.0}, 0.25, 1.0},
                                                    {96000, 48000 * 8, {1.0, -1.0, 0.5}, 0.5, 1.0}}}}};
    adm::ObjectGroup pad{"Pad", {{"Pad L", 14, {{0, 48000 * 10, adm::polarToCartesian(30.0, 0.0, 1.0), 0.0, 1.0}}},
                                 {"Pad R", 15, {{0, 48000 * 10, adm::polarToCartesian(-30.0, 0.0, 1.0), 0.0, 1.0}}}}};
    programme.groups = {vocal, pad};

    // chna: the bed's speakers from the common definitions, the objects' own formats.
    const auto entries = adm::trackEntries(programme);
    REQUIRE(entries.size() == 15);
    const char* const bedTracks[] = {"AT_00010001_01", "AT_00010002_01", "AT_00010003_01", "AT_00010004_01", "AT_0001000a_01",
                                     "AT_0001000b_01", "AT_0001001c_01", "AT_0001001d_01", "AT_00010022_01", "AT_00010023_01",
                                     "AT_0001001e_01", "AT_0001001f_01"};
    for (int i = 0; i < 12; ++i)
    {
        CHECK(entries[static_cast<std::size_t>(i)].trackIndex == i + 1);
        CHECK(entries[static_cast<std::size_t>(i)].uid == "ATU_" + juce::String::toHexString(i + 1).paddedLeft('0', 8));
        CHECK(entries[static_cast<std::size_t>(i)].trackRef == bedTracks[i]);
        CHECK(entries[static_cast<std::size_t>(i)].packRef == "AP_00010017");
    }
    CHECK(entries[12].trackRef == "AT_00031001_01");
    CHECK(entries[12].packRef == "AP_00031001");
    CHECK(entries[13].trackRef == "AT_00031002_01");
    CHECK(entries[14].trackRef == "AT_00031003_01");
    CHECK(entries[14].uid == "ATU_0000000f");

    const auto document = adm::documentFor(programme);
    const auto xml = juce::XmlDocument::parse(document);
    REQUIRE(xml != nullptr);
    CHECK(xml->getTagName() == "ebuCoreMain");
    const auto* afe = child(*child(*child(*xml, "coreMetadata"), "format"), "audioFormatExtended");
    CHECK(afe->getStringAttribute("version") == "ITU-R_BS.2076-2");

    // Every reference resolves, except to the common definitions (IDs 0001xxxx).
    std::set<juce::String> defined;
    std::vector<juce::String> referenced;
    for (const auto* e : afe->getChildIterator())
    {
        for (int a = 0; a < e->getNumAttributes(); ++a)
            if (e->getAttributeName(a).endsWith("ID") || e->getAttributeName(a) == "UID")
                defined.insert(e->getAttributeValue(a));
        for (const auto* r : e->getChildIterator())
            if (r->getTagName().endsWith("IDRef") || r->getTagName() == "audioTrackUIDRef")
                referenced.push_back(r->getAllSubText());
    }
    for (const auto& r : referenced)
    {
        INFO(r);
        CHECK((defined.count(r) == 1 || r.startsWith("AP_0001") || r.startsWith("AT_0001") || r.startsWith("AC_0001")));
    }

    const auto* programmeElement = child(*afe, "audioProgramme");
    CHECK(programmeElement->getStringAttribute("audioProgrammeName") == "Song & <Mix>");
    CHECK(programmeElement->getStringAttribute("end") == "00:00:10.00000");
    CHECK(programmeElement->getNumChildElements() == 3); // bed, Vocal, Pad

    // The bed object: its common pack and twelve tracks.
    const juce::XmlElement* bedObject = nullptr;
    std::vector<const juce::XmlElement*> channelFormats;
    for (const auto* e : afe->getChildIterator())
    {
        if (e->getTagName() == "audioObject" && e->getStringAttribute("audioObjectName") == "Bed")
            bedObject = e;
        if (e->getTagName() == "audioChannelFormat")
            channelFormats.push_back(e);
    }
    REQUIRE(bedObject != nullptr);
    CHECK(child(*bedObject, "audioPackFormatIDRef")->getAllSubText() == "AP_00010017");
    int uids = 0;
    for (const auto* r : bedObject->getChildWithTagNameIterator("audioTrackUIDRef"))
        uids += r != nullptr ? 1 : 0;
    CHECK(uids == 12);

    // The objects' channels: Cartesian blocks that tile the programme.
    REQUIRE(channelFormats.size() == 3);
    const auto* vocalChannel = channelFormats[0];
    CHECK(vocalChannel->getStringAttribute("audioChannelFormatID") == "AC_00031001");
    CHECK(vocalChannel->getStringAttribute("typeDefinition") == "Objects");
    std::vector<const juce::XmlElement*> blocks;
    for (const auto* b : vocalChannel->getChildWithTagNameIterator("audioBlockFormat"))
        blocks.push_back(b);
    REQUIRE(blocks.size() == 3);
    CHECK(blocks[0]->getStringAttribute("audioBlockFormatID") == "AB_00031001_00000001");
    CHECK(blocks[1]->getStringAttribute("rtime") == "00:00:01.00000");
    CHECK(blocks[1]->getStringAttribute("duration") == "00:00:01.00000");
    CHECK(blocks[2]->getStringAttribute("rtime") == "00:00:02.00000");
    CHECK(blocks[2]->getStringAttribute("duration") == "00:00:08.00000");
    CHECK(child(*blocks[1], "cartesian")->getAllSubText() == "1");
    std::map<juce::String, double> position;
    for (const auto* p : blocks[1]->getChildWithTagNameIterator("position"))
        position[p->getStringAttribute("coordinate")] = p->getAllSubText().getDoubleValue();
    CHECK_THAT(position["X"], WithinAbs(-1.0, 1e-9));
    CHECK_THAT(position["Y"], WithinAbs(1.0, 1e-9));
    CHECK_THAT(position["Z"], WithinAbs(1.0, 1e-9));
    CHECK_THAT(child(*blocks[1], "width")->getAllSubText().getDoubleValue(), WithinAbs(0.25, 1e-9));
    CHECK_THAT(child(*blocks[1], "depth")->getAllSubText().getDoubleValue(), WithinAbs(0.25, 1e-9));
    CHECK_THAT(child(*blocks[1], "gain")->getAllSubText().getDoubleValue(), WithinAbs(1.0, 1e-9));

    // Every file track has its audioTrackUID, as chna says.
    int trackUids = 0;
    for (const auto* u : afe->getChildWithTagNameIterator("audioTrackUID"))
    {
        const auto& entry = entries[static_cast<std::size_t>(trackUids++)];
        CHECK(u->getStringAttribute("UID") == entry.uid);
        CHECK(child(*u, "audioTrackFormatIDRef")->getAllSubText() == entry.trackRef);
        CHECK(child(*u, "audioPackFormatIDRef")->getAllSubText() == entry.packRef);
        CHECK(u->getStringAttribute("sampleRate") == "48000");
    }
    CHECK(trackUids == 15);
}

TEST_CASE("ADM document: a 9.1.6 bed gets a pack of its own, of common channels", "[adm]")
{
    adm::Programme programme;
    programme.name = "Wide";
    programme.length = 4800;
    programme.bed = ChannelLayout::surround916();
    for (int c = 1; c <= 16; ++c)
        programme.bedTracks.push_back(c);
    const auto entries = adm::trackEntries(programme);
    REQUIRE(entries.size() == 16);
    for (const auto& e : entries)
        CHECK(e.packRef == "AP_00011001");
    const auto xml = juce::XmlDocument::parse(adm::documentFor(programme));
    REQUIRE(xml != nullptr);
    const auto* afe = child(*child(*child(*xml, "coreMetadata"), "format"), "audioFormatExtended");
    const auto* pack = child(*afe, "audioPackFormat");
    CHECK(pack->getStringAttribute("audioPackFormatID") == "AP_00011001");
    CHECK(pack->getStringAttribute("typeDefinition") == "DirectSpeakers");
    std::vector<juce::String> channels;
    for (const auto* r : pack->getChildWithTagNameIterator("audioChannelFormatIDRef"))
        channels.push_back(r->getAllSubText());
    REQUIRE(channels.size() == 16);
    CHECK(channels[8] == "AC_00010018");  // Lw (M+060)
    CHECK(channels[12] == "AC_00010013"); // Ltm (U+090)
}

TEST_CASE("ADM times and fewer blocks for the same movement", "[adm]")
{
    CHECK(adm::formatTime(0) == "00:00:00.00000");
    CHECK(adm::formatTime(123456789) == "00:20:34.56789");
    CHECK(adm::formatTime(360000000LL * 2 + 1) == "02:00:00.00001");
    CHECK(adm::ticksOf(48000, 48000.0) == 100000);
    CHECK(adm::ticksOf(1, 48000.0) == 2); // (20.8 us)

    const auto block = [](std::int64_t start, double x, double size = 0.0)
    { return adm::ObjectBlock{start * 512, 512, {x, 0.5, 0.0}, size, 1.0}; };
    const auto covers = [](const std::vector<adm::ObjectBlock>& blocks, std::int64_t end)
    {
        std::int64_t at = 0;
        for (const auto& b : blocks)
        {
            if (b.start != at || b.length <= 0)
                return false;
            at += b.length;
        }
        return at == end;
    };

    // Standing still: one block.
    std::vector<adm::ObjectBlock> still;
    for (int k = 0; k < 100; ++k)
        still.push_back(block(k, 0.25));
    auto simple = adm::simplify(still, 1e-4);
    CHECK(simple.size() == 1);
    CHECK(covers(simple, 100 * 512));

    // A quick move between two stays: the move keeps its own (short) block.
    std::vector<adm::ObjectBlock> jump;
    for (int k = 0; k < 10; ++k)
        jump.push_back(block(k, 0.0));
    for (int k = 10; k < 30; ++k)
        jump.push_back(block(k, 1.0));
    simple = adm::simplify(jump, 1e-4);
    REQUIRE(simple.size() == 3);
    CHECK(simple[1].start == 10 * 512);
    CHECK(simple[1].length == 512); // (the renderer moves it in 512 samples, as it was)
    CHECK(simple[2].length == 19 * 512);
    CHECK(covers(simple, 30 * 512));

    // A steady glide: its two ends. A glide that turns: the turn kept.
    std::vector<adm::ObjectBlock> glide, turn;
    for (int k = 0; k < 50; ++k)
    {
        glide.push_back(block(k, -1.0 + 0.04 * k, 0.01 * k));
        turn.push_back(block(k, k < 25 ? 0.04 * k : 1.0 - 0.04 * (k - 25)));
    }
    simple = adm::simplify(glide, 1e-4);
    CHECK(simple.size() == 2);
    CHECK(covers(simple, 50 * 512));
    CHECK_THAT(simple.back().position.x, WithinAbs(-1.0 + 0.04 * 49, 1e-12));
    simple = adm::simplify(turn, 1e-4);
    CHECK(simple.size() == 3);
    CHECK(covers(simple, 50 * 512));
    CHECK_THAT(simple[1].position.x, WithinAbs(1.0, 1e-12)); // (block 25, the top of the turn)
    CHECK(simple[1].start + simple[1].length == 26 * 512);
}

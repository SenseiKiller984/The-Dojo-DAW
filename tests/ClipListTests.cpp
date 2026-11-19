#include "devices/DeviceRegistry.h"
#include "devices/Instruments.h"
#include "engine/AudioEngine.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/MediaInfo.h"
#include "session/Project.h"
#include "TestAudio.h"
#include "ui/ClipList.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/ClipEditor.h"
#include "ui/components/ClipListPanel.h"
#include "ui/components/TrackDialogs.h"
#include "ui/components/VideoWindow.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <optional>
#include <unordered_map>

using namespace dojo;
using session::Session;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
namespace clip = session::clip;
namespace track = session::track;
namespace cl = ui::cliplist;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    auto processor = devices::createBuiltinDevice(session::device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

std::set<std::uint32_t> idsOf(std::initializer_list<juce::ValueTree> clips)
{
    std::set<std::uint32_t> ids;
    for (const auto& c : clips)
        ids.insert(clip::getId(c));
    return ids;
}

/** A WAV with a BWF time reference (`timeReference` seconds) and, optionally, an iXML chunk. */
juce::File writeBwf(const juce::File& file, int numSamples, int numChannels, std::optional<double> timeReference,
                    const juce::String& ixml = {})
{
    juce::AudioBuffer<float> buffer(numChannels, numSamples);
    for (int ch = 0; ch < numChannels; ++ch)
        for (int i = 0; i < numSamples; ++i)
            buffer.setSample(ch, i, 0.25f * static_cast<float>(std::sin(0.05 * i)));
    {
        std::unordered_map<juce::String, juce::String> metadata;
        if (timeReference.has_value())
        {
            metadata[juce::WavAudioFormat::bwavTimeReference] = juce::String(static_cast<juce::int64>(*timeReference * kRate));
            metadata[juce::WavAudioFormat::bwavDescription] = "test";
        }
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                      .withSampleRate(kRate)
                                                      .withNumChannels(numChannels)
                                                      .withBitsPerSample(24)
                                                      .withMetadataValues(metadata));
        REQUIRE(writer != nullptr);
        writer->writeFromAudioSampleBuffer(buffer, 0, numSamples);
    }
    if (ixml.isNotEmpty())
    {
        // Append an iXML chunk and fix the RIFF size.
        juce::MemoryBlock data;
        file.loadFileAsData(data);
        juce::MemoryOutputStream out(data, true);
        out.write("iXML", 4);
        const auto text = ixml.toStdString();
        out.writeInt(static_cast<int>(text.size()));
        out.write(text.data(), text.size());
        if (text.size() % 2 != 0)
            out.writeByte(0);
        out.flush();
        const auto riffSize = static_cast<std::uint32_t>(data.getSize() - 8);
        data.copyFrom(&riffSize, 4, 4);
        REQUIRE(file.replaceWithData(data.getData(), data.getSize()));
    }
    return file;
}

/** Waits for `done` while letting the message thread deliver (async imports). */
bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 10'000)
{
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (!done())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;
        juce::MessageManager::getInstance()->runDispatchLoopUntil(10);
    }
    return true;
}

cl::Item itemOf(const juce::ValueTree& c)
{
    cl::Item item;
    item.clip = c;
    item.entry = c.getParent().hasType(session::ids::clipList);
    item.kind = clip::isMidi(c) ? cl::Kind::midi : clip::isVideo(c) ? cl::Kind::video : cl::Kind::audio;
    return item;
}

std::vector<juce::String> namesOf(const Session& session, const std::vector<cl::Item>& items)
{
    std::vector<juce::String> names;
    for (const auto& item : items)
        names.push_back(cl::getName(session, item));
    return names;
}
} // namespace

// --- Media information ---------------------------------------------------------------------------------

TEST_CASE("Media info: BWF time stamps, iXML scene and take, and names", "[cliplist][media]")
{
    juce::String scene, take;
    session::parseSceneTakeFromName("Sc12 Tk3", scene, take);
    CHECK(scene == "12");
    CHECK(take == "3");
    session::parseSceneTakeFromName("Interview_S04_T02_boom", scene, take);
    CHECK(scene == "04");
    CHECK(take == "02");
    session::parseSceneTakeFromName("Scene 7A Take 1", scene, take);
    CHECK(scene == "7A");
    CHECK(take == "1");
    session::parseSceneTakeFromName("Bass Loop", scene, take);
    CHECK(scene.isEmpty());
    CHECK(take.isEmpty());

    session::parseIxmlSceneTake("<BWFXML><IXML_VERSION>1.5</IXML_VERSION><SCENE>21B</SCENE><TAKE>4</TAKE></BWFXML>", scene, take);
    CHECK(scene == "21B");
    CHECK(take == "4");

    test::TempFolder temp;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const auto file = writeBwf(temp.dir.getChildFile("boom.wav"), 4800, 2, 36000.5,
                               "<BWFXML><SCENE>9</SCENE><TAKE>2</TAKE></BWFXML>");
    const auto info = session::readMediaFileInfo(formats, file);
    CHECK(info.exists);
    CHECK(info.readable);
    CHECK(info.numChannels == 2);
    CHECK_THAT(info.lengthSeconds, WithinAbs(0.1, 1e-9));
    REQUIRE(info.timeReference.has_value());
    CHECK_THAT(*info.timeReference, WithinAbs(36000.5, 1e-6));
    CHECK(info.scene == "9");
    CHECK(info.take == "2");
    CHECK(info.diskName.isNotEmpty());

    const auto missing = session::readMediaFileInfo(formats, temp.dir.getChildFile("nope.wav"));
    CHECK_FALSE(missing.exists);
    CHECK_FALSE(missing.readable);

    // Timecodes typed back.
    CHECK_THAT(*ui::parseTimecode("10:00:00:15", 30.0), WithinAbs(36000.5, 1e-9));
    CHECK_THAT(*ui::parseTimecode("01:02", 30.0), WithinAbs(1.0 + 2.0 / 30.0, 1e-9));
    CHECK_FALSE(ui::parseTimecode("00:00:00:30", 30.0).has_value()); // no frame 30 at 30 fps
    CHECK_FALSE(ui::parseTimecode("1:2:3:4:5", 30.0).has_value());
    CHECK_FALSE(ui::parseTimecode("ab:cd", 30.0).has_value());
    CHECK(ui::formatTimecode(*ui::parseTimecode("01:23:45:12", 30.0), 30.0) == "01:23:45:12");
}

// --- Whole-file clips and time stamps ---------------------------------------------------------------

TEST_CASE("Imported files get a whole-file clip and their BWF time stamp", "[cliplist][session]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    const auto stamped = writeBwf(temp.dir.getChildFile("dialog.wav"), 48000, 1, 3600.0);
    const auto plain = test::writeWav(temp.dir.getChildFile("music.wav"), test::sine(220.0, kRate, 24000), kRate);

    session::Project project([](std::unique_ptr<engine::RenderGraph>) {}, createDevice);
    auto& session = project.getSession();
    project.importFiles({stamped.getFullPathName(), plain.getFullPathName()}, {}, 2.0);
    REQUIRE(pumpUntil([&session] { return session.getAllTracks().size() == 2 && session.getClipListEntries().size() == 2; }));

    const auto dialog = session.findWholeFileClip(stamped);
    REQUIRE(dialog.isValid());
    CHECK(dialog.getProperty(session::ids::wholeFile, false));
    CHECK_THAT(clip::getLength(dialog), WithinAbs(1.0, 1e-6));
    CHECK_THAT(session.getOriginalTimeStamp(dialog), WithinAbs(3600.0, 1e-6));
    CHECK_THAT(session.getOriginalTimeStamp(session.findWholeFileClip(plain)), WithinAbs(2.0, 1e-9)); // where it was placed

    // The clip on the track is that whole-file clip (one row), stamped like its file.
    auto onTrack = session.getAllTracks()[0].getChild(0);
    CHECK_THAT(session.getOriginalTimeStamp(onTrack), WithinAbs(3600.0, 1e-6));
    auto items = cl::collectItems(session, {});
    REQUIRE(items.size() == 2);
    CHECK(items[0].wholeFile);
    CHECK(items[0].used);

    // A split makes two clips playing parts of it: auto-created, the whole file unused now.
    auto right = session.splitClip(onTrack, 2.4);
    REQUIRE(right.isValid());
    CHECK(right.getProperty(session::ids::autoCreated, false));
    CHECK_THAT(session.getOriginalTimeStamp(right), WithinAbs(3600.4, 1e-6)); // its start in the file
    items = cl::collectItems(session, {});
    CHECK(items.size() == 2); // (auto-created hidden)
    CHECK_FALSE(items[0].used);
    cl::Show all;
    all.autoCreated = true;
    CHECK(cl::collectItems(session, all).size() == 4);

    // User time stamps: the original stays, the user one moves; splits keep it consistent.
    session.setUserTimeStamp(onTrack, 7200.0);
    CHECK_THAT(session.getUserTimeStamp(onTrack), WithinAbs(7200.0, 1e-6));
    CHECK_THAT(session.getOriginalTimeStamp(onTrack), WithinAbs(3600.0, 1e-6));
    auto again = session.splitClip(onTrack, 2.2);
    REQUIRE(again.isValid());
    CHECK_THAT(session.getUserTimeStamp(again), WithinAbs(7200.2, 1e-6));
}

TEST_CASE("Sessions from before get whole-file clips when opened, without becoming dirty", "[cliplist][session]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("take.wav"), test::sine(220.0, kRate, 48000), kRate);
    {
        Session old;
        auto t = old.addTrack();
        old.addClip(t, file, 1.0, 0.5, 0.25);
        REQUIRE(old.save(temp.dir.getChildFile("Old.dojo")).wasOk());
    }
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {}, createDevice);
    REQUIRE(project.open(temp.dir.getChildFile("Old.dojo")).wasOk());
    const auto whole = project.getSession().findWholeFileClip(file);
    REQUIRE(whole.isValid());
    CHECK_THAT(clip::getLength(whole), WithinAbs(1.0, 1e-6));
    CHECK_FALSE(project.hasUnsavedChanges());
    CHECK_FALSE(project.getSession().getUndoManager().canUndo());
}

TEST_CASE("Time Stamp sets user time stamps; others move with the first", "[cliplist][timestamp]")
{
    Session session;
    auto t = session.addTrack();
    auto a = session.addClip(t, juce::File("/a.wav"), 10.0, 1.0, 0.5);
    auto b = session.addClip(t, juce::File("/b.wav"), 20.0, 1.0);
    CHECK_THAT(session.getOriginalTimeStamp(a), WithinAbs(10.0, 1e-9)); // where it was first placed
    CHECK_THAT(session.getUserTimeStamp(a), WithinAbs(10.0, 1e-9));

    // Moving a clip does not change its time stamps.
    clip::setStart(a, 30.0, nullptr);
    CHECK_THAT(session.getOriginalTimeStamp(a), WithinAbs(10.0, 1e-9));

    REQUIRE(cl::setUserTimeStamps(session, {a, b}, 100.0));
    CHECK_THAT(session.getUserTimeStamp(a), WithinAbs(100.0, 1e-9));
    CHECK_THAT(session.getUserTimeStamp(b), WithinAbs(110.0, 1e-9));
    CHECK_THAT(session.getOriginalTimeStamp(b), WithinAbs(20.0, 1e-9));
    session.getUndoManager().undo();
    CHECK_THAT(session.getUserTimeStamp(a), WithinAbs(10.0, 1e-9));

    // The dialog.
    juce::MessageManager::getInstance();
    double got = -1.0;
    ui::TimeStampPanel panel("a", 2, 10.0, 10.0, 42.0);
    panel.onSet = [&got](double s) { got = s; };
    panel.user.setText("nonsense", juce::dontSendNotification);
    panel.confirm();
    CHECK(got < 0.0);
    panel.currentButton.triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(panel.user.getText() == "00:00:42:00");
    panel.user.setText("01:00:00:00", juce::dontSendNotification);
    panel.confirm();
    CHECK_THAT(got, WithinAbs(3600.0, 1e-9));
}

// --- Auto Rename ----------------------------------------------------------------------------------------

TEST_CASE("Auto Rename numbers clips; clips made from them by editing continue the numbering", "[cliplist][autorename]")
{
    Session session;
    ui::EditorState editor;
    auto t = session.addTrack();
    auto a = session.addClip(t, juce::File("/a.wav"), 0.0, 4.0);
    auto b = session.addClip(t, juce::File("/b.wav"), 5.0, 4.0);

    CHECK_FALSE(cl::autoRename(session, {a, b}, "  ", 1, 2, ""));
    REQUIRE(cl::autoRename(session, {a, b}, "Verse_", 7, 3, ".v"));
    CHECK(clip::getName(a) == "Verse_007.v");
    CHECK(clip::getName(b) == "Verse_008.v");
    CHECK(clip::isUserNamed(a));
    CHECK(session.getNextAutoNumber("Verse_") == 9);

    // Split, duplicate and paste go on from there (and are auto-created clips).
    auto right = session.splitClip(a, 2.0);
    REQUIRE(right.isValid());
    CHECK(clip::getName(right) == "Verse_009.v");
    CHECK_FALSE(clip::isUserNamed(right));
    editor.setSelectedClips(idsOf({b}));
    ui::actions::duplicateSelectedClips(session, editor);
    REQUIRE(editor.getSelectedClips().size() == 1);
    CHECK(clip::getName(session.findClip(*editor.getSelectedClips().begin())) == "Verse_010.v");
    // Clips without auto-naming keep their names when split.
    auto plain = session.addClip(t, juce::File("/c.wav"), 20.0, 4.0);
    CHECK(clip::getName(session.splitClip(plain, 22.0)) == "c");

    // Undo the rename: names and counter back.
    for (int i = 0; i < 4; ++i)
        session.getUndoManager().undo();
    CHECK(clip::getName(b) == "b");

    // The dialog.
    juce::MessageManager::getInstance();
    ui::AutoRenamePanel panel("Take_", 1, 3);
    panel.zeros.setText("2", juce::sendNotificationSync);
    panel.suffix.setText("_x", juce::sendNotificationSync);
    CHECK(panel.getExample() == "Take_01_x  ...  Take_03_x");
    juce::String name;
    int number = 0, zeros = 0;
    juce::String suffix;
    panel.onRename = [&](const juce::String& n, int num, int z, const juce::String& s)
    {
        name = n;
        number = num;
        zeros = z;
        suffix = s;
    };
    panel.confirm();
    CHECK(name == "Take_");
    CHECK(number == 1);
    CHECK(zeros == 2);
    CHECK(suffix == "_x");
}

// --- Show, Sort, Select --------------------------------------------------------------------------------

TEST_CASE("Clip List Show options filter kinds of clips", "[cliplist][show]")
{
    Session session;
    auto audio = session.addTrack(session::TrackKind::audio, "Audio");
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto video = session.addTrack(session::TrackKind::video, "Video");
    session.addWholeFileClip(juce::File("/media/take.wav"), 10.0, false, 0.0);
    auto a = session.addClip(audio, juce::File("/media/take.wav"), 0.0, 10.0); // the whole file
    auto m = session.addMidiClip(keys, 0.0, 2.0, "Chords");
    session.addWholeFileClip(juce::File("/media/movie.avi"), 5.0, true, 0.0);
    session.addVideoClip(video, juce::File("/media/movie.avi"), 0.0, 5.0);
    auto part = session.addClip(audio, juce::File("/media/take.wav"), 12.0, 2.0, 3.0); // part of it: auto-created
    session.groupClips({a, m}, "Song");

    cl::Show show;
    auto items = cl::collectItems(session, show);
    CHECK(namesOf(session, items) == std::vector<juce::String>{"take", "movie", "Chords", "Song"});
    CHECK(items[0].offline); // (no such file)
    CHECK(items[1].kind == cl::Kind::video);
    CHECK(items[3].kind == cl::Kind::group);

    show.audio = false;
    show.groups = false;
    CHECK(namesOf(session, cl::collectItems(session, show)) == std::vector<juce::String>{"movie", "Chords"});
    show.midi = false;
    show.video = false;
    CHECK(cl::collectItems(session, show).empty());

    show = {};
    show.autoCreated = true;
    items = cl::collectItems(session, show);
    REQUIRE(items.size() == 5);
    const auto found = std::find_if(items.begin(), items.end(), [&](const cl::Item& i) { return i.clip == part; });
    REQUIRE(found != items.end());
    CHECK(found->autoCreated);

    // Column information.
    CHECK(cl::getFileTypeName(items[0]) == "Audio");
    CHECK(cl::isTickBased(*std::find_if(items.begin(), items.end(), [](const cl::Item& i) { return i.kind == cl::Kind::midi; })));
    CHECK_FALSE(cl::isProcessed(items[0]));
    clip::setStretch(part, 1.5, nullptr);
    CHECK(cl::isProcessed(*found));
    CHECK_THAT(cl::getLength(session, items.back()), WithinAbs(10.0, 1e-9)); // the group's extent
    CHECK(cl::getColour(session, *found, juce::Colours::black) == session::track::getColour(audio));

    // Options are remembered as text.
    cl::Options options;
    options.show.diskName = true;
    options.show.midi = false;
    options.sort = cl::SortKey::sceneAndTake;
    options.ascending = false;
    const auto back = cl::Options::fromString(options.toString());
    CHECK(back.show.diskName);
    CHECK_FALSE(back.show.midi);
    CHECK(back.sort == cl::SortKey::sceneAndTake);
    CHECK_FALSE(back.ascending);
    CHECK(cl::Options::fromString("garbage").show.audio); // defaults
}

TEST_CASE("Clip List Sort By orders rows, ascending or descending", "[cliplist][sort]")
{
    test::TempFolder temp;
    const auto s2 = writeBwf(temp.dir.getChildFile("Sc2_Tk1.wav"), 9600, 1, 100.0);
    const auto s10 = writeBwf(temp.dir.getChildFile("Sc10_Tk3.wav"), 4800, 2, 50.0);
    const auto s2b = writeBwf(temp.dir.getChildFile("Sc2_Tk2.wav"), 14400, 1, 75.0);

    Session session;
    auto t = session.addTrack();
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    session.addWholeFileClip(s2, 0.2, false, 100.0);
    session.addWholeFileClip(s10, 0.1, false, 50.0);
    session.addWholeFileClip(s2b, 0.3, false, 75.0);
    session.addMidiClip(keys, 0.0, 0.25, "Arp");
    juce::ignoreUnused(t);
    cl::FileInfoCache files;

    const auto sorted = [&](cl::SortKey key, bool ascending)
    {
        auto items = cl::collectItems(session, {});
        cl::sortItems(items, session, key, ascending, files);
        return namesOf(session, items);
    };
    using V = std::vector<juce::String>;
    CHECK(sorted(cl::SortKey::none, true) == V{"Sc2_Tk1", "Sc10_Tk3", "Sc2_Tk2", "Arp"});
    CHECK(sorted(cl::SortKey::name, true) == V{"Arp", "Sc2_Tk1", "Sc2_Tk2", "Sc10_Tk3"}); // natural order
    CHECK(sorted(cl::SortKey::name, false) == V{"Sc10_Tk3", "Sc2_Tk2", "Sc2_Tk1", "Arp"});
    CHECK(sorted(cl::SortKey::length, true) == V{"Sc10_Tk3", "Sc2_Tk1", "Arp", "Sc2_Tk2"});
    CHECK(sorted(cl::SortKey::originalTimeStamp, true) == V{"Arp", "Sc10_Tk3", "Sc2_Tk2", "Sc2_Tk1"});
    CHECK(sorted(cl::SortKey::clipType, false).front() == "Arp");
    CHECK(sorted(cl::SortKey::timebase, true).back() == "Arp");
    // File keys: MIDI clips have none and go last either way.
    CHECK(sorted(cl::SortKey::fileLength, true) == V{"Sc10_Tk3", "Sc2_Tk1", "Sc2_Tk2", "Arp"});
    CHECK(sorted(cl::SortKey::fileLength, false) == V{"Sc2_Tk2", "Sc2_Tk1", "Sc10_Tk3", "Arp"});
    CHECK(sorted(cl::SortKey::trackFormat, false).front() == "Sc10_Tk3"); // the stereo one
    CHECK(sorted(cl::SortKey::sceneAndTake, true) == V{"Sc2_Tk1", "Sc2_Tk2", "Sc10_Tk3", "Arp"});
    CHECK(sorted(cl::SortKey::fileName, true) == V{"Sc2_Tk1", "Sc2_Tk2", "Sc10_Tk3", "Arp"});
    CHECK(sorted(cl::SortKey::diskName, true).back() == "Arp");
    CHECK(cl::appliesToMidi(cl::SortKey::startInParent));
    CHECK_FALSE(cl::appliesToMidi(cl::SortKey::diskName));
    for (int k = 0; k < cl::kNumSortKeys; ++k)
        CHECK(cl::getSortKeyName(static_cast<cl::SortKey>(k)).isNotEmpty());

    // Channel names.
    auto items = cl::collectItems(session, {});
    CHECK(cl::getChannelName(items[0], files) == "Mono");
    CHECK(cl::getChannelName(items[1], files) == "Stereo");
    CHECK(cl::getChannelCount(items[1], files) == 2);
}

TEST_CASE("Clip List Select: all, unused, unused except whole files, offline", "[cliplist][select]")
{
    test::TempFolder temp;
    const auto present = test::writeWav(temp.dir.getChildFile("here.wav"), test::sine(220.0, kRate, 48000), kRate);
    Session session;
    auto t = session.addTrack();
    session.addWholeFileClip(present, 1.0, false, 0.0);               // 0: whole file, unused
    session.addWholeFileClip(juce::File("/gone/away.wav"), 1.0, false, 0.0); // 1: whole file, offline, unused
    auto used = session.addClip(t, present, 0.0, 0.5);
    juce::String error;
    session.captureClip(t, 0.1, 0.3, "Bit", error);                   // 2: captured, unused
    ui::actions::renameClip(session, used, "Kept");                    // 3: user, used

    const auto items = cl::collectItems(session, {});
    REQUIRE(items.size() == 4);
    CHECK(cl::select(items, cl::Selection::all) == std::vector<int>{0, 1, 2, 3});
    CHECK(cl::select(items, cl::Selection::unused) == std::vector<int>{0, 1, 2});
    CHECK(cl::select(items, cl::Selection::unusedExceptWholeFiles) == std::vector<int>{2});
    CHECK(cl::select(items, cl::Selection::offline) == std::vector<int>{1});

    // Placing the capture makes it used.
    ui::EditorState editor;
    cl::place(session, editor, {items[2]}, t, 5.0);
    CHECK(cl::select(cl::collectItems(session, {}), cl::Selection::unusedExceptWholeFiles).empty());

    // Remove from Clip List: entries only, one undo step.
    CHECK(cl::removeEntries(session, {items[0], items[1], items[3]}) == 2);
    CHECK(session.getClipListEntries().size() == 1);
    session.getUndoManager().undo();
    CHECK(session.getClipListEntries().size() == 3);
}

// --- Drag, place, export ---------------------------------------------------------------------------------

TEST_CASE("Several Clip List rows and clip groups drop onto tracks", "[cliplist][place]")
{
    Session session;
    ui::EditorState editor;
    auto a = session.addTrack(session::TrackKind::audio, "A");
    auto b = session.addTrack(session::TrackKind::audio, "B");
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto e1 = session.addWholeFileClip(juce::File("/x.wav"), 2.0, false, 0.0);
    auto e2 = session.addWholeFileClip(juce::File("/y.wav"), 3.0, false, 0.0);
    auto g1 = session.addClip(a, juce::File("/g1.wav"), 10.0, 1.0);
    auto g2 = session.addClip(b, juce::File("/g2.wav"), 11.0, 1.0);
    const auto group = session.groupClips({g1, g2}, "Hits");

    cl::Item groupItem;
    groupItem.kind = cl::Kind::group;
    groupItem.groupId = group;
    const std::vector<cl::Item> rows{itemOf(e1), itemOf(e2), groupItem};
    const auto description = cl::describeDrag(rows);
    const auto dragged = cl::findDragged(session, description);
    REQUIRE(dragged.size() == 3);
    CHECK(dragged[0].clip == e1);
    CHECK(dragged[0].entry);
    CHECK(dragged[2].groupId == group);

    // One after the other on track A; the group keeps its two tracks.
    const int before = a.getNumChildren();
    const auto placed = cl::place(session, editor, dragged, a, 20.0);
    REQUIRE(placed.size() == 4);
    CHECK(clip::getStart(placed[0]) == 20.0);
    CHECK(clip::getStart(placed[1]) == 22.0);
    CHECK(clip::getStart(placed[2]) == 25.0);
    CHECK(placed[2].getParent() == a);
    CHECK(clip::getStart(placed[3]) == 26.0);
    CHECK(placed[3].getParent() == b);
    CHECK(clip::getOuterGroup(placed[2]) != group);
    CHECK(clip::getOuterGroup(placed[2]) == clip::getOuterGroup(placed[3]));
    CHECK(static_cast<int>(placed[0].getProperty(session::ids::listEntry, 0)) == static_cast<int>(clip::getId(e1)));
    CHECK(editor.getSelectedClips().size() == 4);
    CHECK(a.getNumChildren() == before + 3);

    // The group dropped on the last track: its second clip needs a track below, so one is made.
    const int tracks = session.getNumTracks();
    cl::place(session, editor, {groupItem}, keys, 40.0);
    CHECK(session.getNumTracks() == tracks + 2); // (Keys cannot hold audio either)
    session.getUndoManager().undo();
    CHECK(session.getNumTracks() == tracks);
}

TEST_CASE("Export Clips as Files writes audio as WAV and MIDI as Standard MIDI Files", "[cliplist][export]")
{
    test::TempFolder temp;
    std::vector<float> ramp(48000);
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = static_cast<float>(i) / 48000.0f;
    const auto file = test::writeWav(temp.dir.getChildFile("ramp.wav"), ramp, kRate);

    Session session;
    auto t = session.addTrack();
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto video = session.addTrack(session::TrackKind::video, "V");
    auto c = session.addClip(t, file, 0.0, 0.25, 0.5); // the file's 0.5 .. 0.75 s
    clip::setName(c, "Middle", nullptr);
    auto m = session.addMidiClip(keys, 0.0, 1.0, "Riff"); // 2 beats at 120 bpm
    session.addNote(m, 0.0, 0.5, 60);
    session.addNote(m, 1.0, 1.0, 64);
    session.addNote(m, 3.0, 1.0, 67); // past the clip's end
    auto v = session.addVideoClip(video, juce::File("/movie.avi"), 0.0, 1.0);

    juce::String error;
    const auto out = temp.dir.getChildFile("Exported");
    CHECK(cl::exportClips(session, {itemOf(c), itemOf(m), itemOf(v)}, out, error) == 2);
    CHECK(error.contains("movie")); // video is not exported

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(out.getChildFile("Middle.wav")));
    REQUIRE(reader != nullptr);
    CHECK(reader->lengthInSamples == 12000);
    juce::AudioBuffer<float> audio(1, 12000);
    reader->read(&audio, 0, 12000, 0, true, false);
    CHECK_THAT(audio.getSample(0, 0), WithinAbs(0.5, 1e-5));
    CHECK_THAT(audio.getSample(0, 11999), WithinAbs(0.75, 1e-4));

    juce::MidiFile midi;
    juce::FileInputStream in(out.getChildFile("Riff.mid"));
    REQUIRE(in.openedOk());
    REQUIRE(midi.readFrom(in));
    CHECK(midi.getTimeFormat() == 960);
    int notes = 0;
    for (const auto* e : *midi.getTrack(0))
        if (e->message.isNoteOn())
        {
            ++notes;
            if (e->message.getNoteNumber() == 64)
                CHECK(e->message.getTimeStamp() == 960.0);
        }
    CHECK(notes == 2);
}

// --- The panel --------------------------------------------------------------------------------------------

TEST_CASE("The Clip List panel applies its options, selects, and remembers its options", "[cliplist][ui]")
{
    juce::MessageManager::getInstance();
    Session session;
    ui::EditorState editor;
    auto t = session.addTrack();
    session.addWholeFileClip(juce::File("/a.wav"), 1.0, false, 0.0);
    session.addWholeFileClip(juce::File("/b.wav"), 2.0, false, 0.0);
    auto onTrack = session.addClip(t, juce::File("/b.wav"), 0.0, 2.0);

    ui::ClipListPanel panel(session, editor);
    panel.setSize(900, 300);
    REQUIRE(panel.getItems().size() == 2);

    juce::PropertySet settings;
    panel.setSettings(&settings);
    auto options = panel.getOptions();
    options.sort = cl::SortKey::length;
    options.ascending = false;
    options.show.timebase = true;
    panel.setOptions(options);
    CHECK(cl::getName(session, panel.getItems()[0]) == "b");
    CHECK(settings.getValue("clipListOptions") == options.toString());

    ui::ClipListPanel later(session, editor);
    later.setSettings(&settings);
    CHECK(later.getOptions().sort == cl::SortKey::length);

    panel.selectRows(cl::Selection::unused);
    const auto selected = panel.getSelectedItems();
    REQUIRE(selected.size() == 1);
    CHECK(cl::getName(session, selected[0]) == "a");

    // The menu has the Show, Select and Sort By submenus and the commands.
    auto menu = panel.buildMenu();
    juce::StringArray names;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        names.add(it.getItem().text);
    for (const auto* name : {"Show", "Select", "Sort By", "Rename...", "Auto Rename...", "Time Stamp...", "Export Clips as Files...",
                             "Remove from Clip List"})
        CHECK(names.contains(name));

    // Removing the selected entry.
    panel.removeSelected();
    panel.refresh();
    CHECK(panel.getItems().size() == 1);
    juce::ignoreUnused(onTrack);
}

// --- Piano roll ------------------------------------------------------------------------------------------

namespace
{
juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> at, juce::Point<float> downAt)
{
    const auto now = juce::Time::getCurrentTime();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at,
                            juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier), juce::MouseInputSource::defaultPressure,
                            juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                            juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &c, &c, now, downAt, now, 1,
                            at != downAt);
}
} // namespace

TEST_CASE("The piano roll scrolls left and right with its scroll bar and plays its keys while held", "[ui][pianoroll]")
{
    juce::MessageManager::getInstance();
    Session session;
    ui::EditorState editor;
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto m = session.addMidiClip(keys, 0.0, 8.0); // 16 beats
    session.addNote(m, 0.0, 1.0, 60);

    ui::PianoRoll roll(session, editor);
    roll.setSize(800, 400);
    roll.setClip(m);

    // The horizontal scroll bar covers the clip and moves the view.
    auto& bar = roll.getHorizontalScrollBar();
    CHECK(bar.isVisible());
    CHECK(bar.getMaximumRangeLimit() >= 16.0);
    const double furthest = bar.getMaximumRangeLimit() - bar.getCurrentRangeSize(); // (a little past the clip's end)
    REQUIRE(furthest > 0.5);
    bar.setCurrentRangeStart(furthest, juce::sendNotificationSync);
    CHECK_THAT(roll.getScrollBeats(), WithinAbs(furthest, 1e-9));
    auto& vertical = roll.getVerticalScrollBar();
    vertical.setCurrentRangeStart(127.0 - 90.0, juce::sendNotificationSync);
    CHECK(roll.getScrollPitch() == 90);
    // A sideways wheel moves the view, and the bar with it.
    roll.mouseWheelMove(mouseAt(roll, {300.0f, 100.0f}, {300.0f, 100.0f}), juce::MouseWheelDetails{-0.5f, 0.0f, false, false, false});
    CHECK(roll.getScrollBeats() > furthest);
    CHECK_THAT(bar.getCurrentRangeStart(), WithinAbs(roll.getScrollBeats(), 1e-9));

    // The keys: note-on while held (louder further right), another key as the mouse slides,
    // note-off on release.
    std::vector<juce::MidiMessage> sent;
    roll.onPreview = [&sent](const juce::MidiMessage& message) { sent.push_back(message); };
    const auto keyArea = roll.getKeyboardArea();
    const float y72 = roll.getKeyY(72) + 3.0f;
    const float y74 = roll.getKeyY(74) + 3.0f;
    const juce::Point<float> down(static_cast<float>(keyArea.getRight()) - 4.0f, y72);
    const int actionsBefore = session.getUndoManager().getNumActionsInCurrentTransaction();
    roll.mouseDown(mouseAt(roll, down, down));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].isNoteOn());
    CHECK(sent[0].getNoteNumber() == 72);
    CHECK(sent[0].getFloatVelocity() > 0.9f);
    CHECK(roll.getPreviewNote() == 72);
    roll.mouseDrag(mouseAt(roll, {down.x, y74}, down));
    REQUIRE(sent.size() == 3);
    CHECK(sent[1].isNoteOff());
    CHECK(sent[1].getNoteNumber() == 72);
    CHECK(sent[2].getNoteNumber() == 74);
    roll.mouseUp(mouseAt(roll, {down.x, y74}, down));
    REQUIRE(sent.size() == 4);
    CHECK(sent[3].isNoteOff());
    CHECK(sent[3].getNoteNumber() == 74);
    CHECK(roll.getPreviewNote() == -1);
    CHECK(session.getUndoManager().getNumActionsInCurrentTransaction() == actionsBefore); // (no note was drawn)
    CHECK(m.getNumChildren() == 1);

    // Keys play on an edit-locked clip too; a soft press is quieter.
    clip::setEditLocked(m, true, nullptr);
    const juce::Point<float> soft(2.0f, y72);
    roll.mouseDown(mouseAt(roll, soft, soft));
    REQUIRE(sent.size() == 5);
    CHECK(sent[4].getFloatVelocity() < 0.5f);
    roll.mouseUp(mouseAt(roll, soft, soft));
}

TEST_CASE("The step sequencer's row names play their notes while held", "[ui][pianoroll]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {}, createDevice);
    auto& session = project.getSession();
    ui::EditorState editor;
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto m = session.addMidiClip(keys, 0.0, 2.0);
    ui::StepSequencer steps(project, editor);
    steps.setSize(800, 300);
    steps.setBaseNote(48);
    steps.setClip(m);
    std::vector<juce::MidiMessage> sent;
    steps.onPreview = [&sent](const juce::MidiMessage& message) { sent.push_back(message); };

    const auto label = steps.getRowLabel(15).getCentre().toFloat(); // the lowest row: the base note
    steps.mouseDown(mouseAt(steps, label, label));
    REQUIRE(sent.size() == 1);
    CHECK(sent[0].isNoteOn());
    CHECK(sent[0].getNoteNumber() == 48);
    steps.mouseUp(mouseAt(steps, label, label));
    REQUIRE(sent.size() == 2);
    CHECK(sent[1].isNoteOff());
    CHECK(m.getNumChildren() == 0); // (no step was set)
}

TEST_CASE("Previewed notes play on their track only, even while stopped and unarmed", "[engine][pianoroll]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("a440.wav"), test::sine(440.0, kRate, 96000, 0.5f), kRate);
    engine::AudioEngine engine(1);
    Session session;
    session::AudioAssetCache assets;
    session::DeviceManager devices(session, createDevice);
    session::GraphBuilder builder;
    engine.prepare(kRate, kBlock, 2);
    devices.setPlayHead(engine.getPlayHead());
    devices.prepareAll(kRate, kBlock);
    builder.setRenderFormat(kRate, kBlock);

    const auto addSampler = [&](const juce::String& name)
    {
        auto t = session.addTrack(session::TrackKind::instrument, name);
        auto d = session.addDevice(t, "builtin:sampler", "Sampler");
        auto node = devices.getNode(d);
        REQUIRE(node != nullptr);
        auto& sampler = dynamic_cast<devices::Sampler&>(node->getProcessor());
        juce::String error;
        REQUIRE(sampler.loadSample(file, error));
        sampler.root_->setValueNotifyingHost(sampler.root_->convertTo0to1(69.0f));
        return t;
    };
    auto first = addSampler("One");
    auto second = addSampler("Two");
    track::setVolumeDb(second, session::kMinusInfinityDb, nullptr); // (to tell them apart)
    juce::ignoreUnused(first);

    auto graph = builder.build(session, assets, &devices);
    graph->serial = 1;
    engine::command::SwapGraph swap;
    swap.graph = graph.release();
    REQUIRE(engine.post(swap));

    std::vector<float> left;
    const auto render = [&](double seconds)
    {
        left.clear();
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(seconds * kRate); done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            left.insert(left.end(), l.begin(), l.end());
        }
        return test::peak(left);
    };

    // Transport stopped, track not armed: the note sounds while held...
    REQUIRE(engine.getLiveMidiQueue().push(juce::MidiMessage::noteOn(1, 69, 0.9f), session::track::getId(first)));
    CHECK(render(0.3) > 0.2);
    CHECK_THAT(test::estimateFrequency(left, 2000, 12000, kRate), WithinRel(440.0, 0.01));
    // ... and stops when let go.
    REQUIRE(engine.getLiveMidiQueue().push(juce::MidiMessage::noteOff(1, 69), session::track::getId(first)));
    render(0.5);
    CHECK(render(0.2) < 1e-4);

    // To the other (silent) track only: nothing is heard from the first.
    REQUIRE(engine.getLiveMidiQueue().push(juce::MidiMessage::noteOn(1, 69, 0.9f), session::track::getId(second)));
    CHECK(render(0.3) < 1e-6);
    REQUIRE(engine.getLiveMidiQueue().push(juce::MidiMessage::noteOff(1, 69), session::track::getId(second)));
    render(0.3);

    // A MIDI track's preview plays through the instrument track it drives.
    auto midi = session.addTrack(session::TrackKind::midi, "Melody");
    track::setMidiTarget(midi, session::track::getId(first), nullptr);
    auto rebuilt = builder.build(session, assets, &devices);
    rebuilt->serial = 2;
    engine::command::SwapGraph swapAgain;
    swapAgain.graph = rebuilt.release();
    REQUIRE(engine.post(swapAgain));
    render(0.05);
    REQUIRE(engine.getLiveMidiQueue().push(juce::MidiMessage::noteOn(1, 69, 0.9f), session::track::getId(midi)));
    CHECK(render(0.3) > 0.2);
    REQUIRE(engine.getLiveMidiQueue().push(juce::MidiMessage::noteOff(1, 69), session::track::getId(midi)));
    render(0.5);
    CHECK(render(0.2) < 1e-4);
    engine.releaseRetiredGraphs();
}

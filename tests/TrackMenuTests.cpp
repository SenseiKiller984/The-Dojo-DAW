#include "devices/DeviceRegistry.h"
#include "devices/Equaliser.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Exporter.h"
#include "session/Project.h"
#include "session/Recorder.h"
#include "ui/AppServices.h"
#include "ui/Commands.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/MainComponent.h"
#include "ui/Preferences.h"
#include "ui/components/ArrangerView.h"
#include "ui/components/MixerPanel.h"
#include "ui/components/TrackDialogs.h"
#include "ui/theme/DojoTheme.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;

namespace
{
struct ProjectRig
{
    ProjectRig()
        : project([](std::unique_ptr<engine::RenderGraph>) {},
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); })
    {
        juce::MessageManager::getInstance();
    }

    Session& session() { return project.getSession(); }

    std::vector<juce::String> names()
    {
        std::vector<juce::String> result;
        for (const auto& t : project.getSession().getAllTracks())
            result.push_back(track::getName(t));
        return result;
    }

    void select(std::initializer_list<juce::ValueTree> tracks)
    {
        std::set<std::uint32_t> ids;
        for (const auto& t : tracks)
            ids.insert(track::getId(t));
        editor.setSelectedTracks(ids, track::getId(*tracks.begin()));
    }

    Project project;
    ui::EditorState editor;
};

std::vector<juce::ValueTree> childrenOf(const juce::ValueTree& t, const juce::Identifier& type)
{
    std::vector<juce::ValueTree> result;
    for (const auto& c : t)
        if (c.hasType(type))
            result.push_back(c);
    return result;
}
} // namespace

TEST_CASE("Duplicating a track copies what was asked for, with fresh ids and relinked automation", "[session][tracks][duplicate]")
{
    Session session;
    auto bus = session.addTrack(TrackKind::bus, "Reverb");
    auto vox = session.addTrack(TrackKind::audio, "Vox");
    session.addClip(vox, juce::File::getCurrentWorkingDirectory().getChildFile("vox.wav"), 1.0, 2.0);
    auto eq = session.addDevice(vox, "builtin:eq", "EQ");
    auto send = session.addSend(vox, bus, -6.0, false);
    session.getAutomation(vox, automation::volumeTarget(), true, 0.5);
    session.getAutomation(vox, automation::parameterTarget(device::getId(eq), 3), true, 0.5);
    session.getAutomation(vox, automation::sendTarget(send::getId(send)), true, 0.5);
    track::setArmed(vox, true, nullptr);
    auto after = session.addTrack(TrackKind::audio, "After");

    // Everything.
    auto copy = session.duplicateTrack(vox, {}, vox);
    REQUIRE(copy.isValid());
    CHECK(track::getName(copy) == "Vox.dup1");
    CHECK(session.indexOf(copy) == session.indexOf(vox) + 1); // right after its source
    CHECK(session.indexOf(after) == session.indexOf(copy) + 1);
    CHECK_FALSE(track::isArmed(copy));
    CHECK(track::getId(copy) != track::getId(vox));

    const auto copiedClips = childrenOf(copy, ids::clip);
    REQUIRE(copiedClips.size() == 1);
    CHECK(clip::getId(copiedClips[0]) != clip::getId(childrenOf(vox, ids::clip)[0]));
    CHECK_THAT(clip::getStart(copiedClips[0]), WithinAbs(1.0, 1e-9));

    const auto copiedDevices = Session::getDevices(copy);
    const auto copiedSends = Session::getSends(copy);
    REQUIRE(copiedDevices.size() == 1);
    REQUIRE(copiedSends.size() == 1);
    CHECK(device::getId(copiedDevices[0]) != device::getId(eq));
    CHECK(send::getTarget(copiedSends[0]) == track::getId(bus)); // still to the same (outside) bus

    std::set<juce::String> targets;
    for (const auto& lane : Session::getAutomationLanes(copy))
        targets.insert(automation::getTarget(lane));
    CHECK(targets == std::set<juce::String>{automation::volumeTarget(), automation::parameterTarget(device::getId(copiedDevices[0]), 3),
                                             automation::sendTarget(send::getId(copiedSends[0]))});

    // Names keep counting; without clips and devices, the device's automation stays behind too.
    Session::DuplicateOptions bare;
    bare.clips = false;
    bare.devices = false;
    auto second = session.duplicateTrack(vox, bare, copy);
    CHECK(track::getName(second) == "Vox.dup2");
    CHECK(childrenOf(second, ids::clip).empty());
    CHECK(Session::getDevices(second).empty());
    CHECK(Session::getSends(second).size() == 1);
    targets.clear();
    for (const auto& lane : Session::getAutomationLanes(second))
        targets.insert(automation::getTarget(lane));
    CHECK(targets == std::set<juce::String>{automation::volumeTarget(), automation::sendTarget(send::getId(Session::getSends(second)[0]))});

    // A folder comes with its contents; routing inside it follows the copy.
    auto drums = session.addTrack(TrackKind::audio, "Kick");
    auto group = session.addTrack(TrackKind::bus, "Drum Bus");
    track::setOutput(drums, track::getId(group), nullptr);
    auto folder = session.groupIntoFolder({drums, group}, "Drums");
    auto folderCopy = session.duplicateTrack(folder, {}, folder);
    REQUIRE(folderCopy.getNumChildren() >= 2);
    const auto kickCopy = folderCopy.getChildWithProperty(ids::name, "Kick");
    const auto busCopy = folderCopy.getChildWithProperty(ids::name, "Drum Bus");
    REQUIRE(kickCopy.isValid());
    REQUIRE(busCopy.isValid());
    CHECK(track::getOutput(kickCopy) == track::getId(busCopy));
    CHECK(track::getOutput(drums) == track::getId(group)); // the original is untouched
}

TEST_CASE("Track > Duplicate: several copies, after each source or after the last selected track", "[ui][tracks][duplicate]")
{
    ProjectRig rig;
    auto& session = rig.session();
    auto a = session.addTrack(TrackKind::audio, "A");
    auto b = session.addTrack(TrackKind::audio, "B");
    session.addTrack(TrackKind::audio, "C");
    auto eq = session.addDevice(a, "builtin:eq", "EQ");

    // The copy starts from the device's current settings (not the last saved ones).
    auto node = rig.project.getDevices().getNode(eq);
    REQUIRE(node != nullptr);
    auto& equaliser = dynamic_cast<devices::Equaliser&>(node->getProcessor());
    auto* gain = equaliser.getBand(1).gain;
    gain->setValueNotifyingHost(gain->convertTo0to1(7.5f));

    rig.select({a, b});
    ui::actions::DuplicateSettings settings;
    settings.copies = 2;
    auto copies = ui::actions::duplicateSelectedTracks(rig.project, rig.editor, settings);
    CHECK(copies.size() == 4);
    CHECK(rig.names() == std::vector<juce::String>{"A", "A.dup1", "A.dup2", "B", "B.dup1", "B.dup2", "C"});
    CHECK(rig.editor.getSelectedTracks().size() == 4); // the copies are selected

    const auto copiedEq = Session::getDevices(session.getTrack(1));
    REQUIRE(copiedEq.size() == 1);
    auto copiedNode = rig.project.getDevices().getNode(copiedEq[0]);
    REQUIRE(copiedNode != nullptr);
    CHECK_THAT(dynamic_cast<devices::Equaliser&>(copiedNode->getProcessor()).getBand(1).gain->get(), WithinAbs(7.5, 0.01));

    session.getUndoManager().undo(); // one step
    CHECK(rig.names() == std::vector<juce::String>{"A", "B", "C"});

    // All after the last selected track, in sets.
    rig.select({a, b});
    settings.afterLastSelected = true;
    ui::actions::duplicateSelectedTracks(rig.project, rig.editor, settings);
    CHECK(rig.names() == std::vector<juce::String>{"A", "B", "A.dup1", "B.dup1", "A.dup2", "B.dup2", "C"});
}

TEST_CASE("Track > New adds rows of tracks after the selected one, with formats and names", "[ui][tracks][new]")
{
    ProjectRig rig;
    auto& session = rig.session();
    auto first = session.addTrack(TrackKind::audio, "First");
    auto inside = session.addTrack(TrackKind::audio, "Inside");
    auto last = session.addTrack(TrackKind::audio, "Last");
    auto folder = session.groupIntoFolder({inside}, "Folder");
    juce::ignoreUnused(first, last);
    rig.select({inside});

    using ui::actions::NewTrackType;
    std::vector<ui::actions::NewTracks> rows(3);
    rows[0].count = 2;
    rows[0].type = NewTrackType::audio;
    rows[0].format = TrackFormat::mono;
    rows[0].name = "Vox";
    rows[1].type = NewTrackType::auxInput;
    rows[1].format = TrackFormat::stereo;
    rows[1].name = "Verb";
    rows[2].type = NewTrackType::instrument;

    const auto created = ui::actions::addNewTracks(session, rig.editor, rows);
    REQUIRE(created.size() == 4);
    CHECK(track::getName(created[0]) == "Vox 1");
    CHECK(track::getName(created[1]) == "Vox 2");
    CHECK(track::getName(created[2]) == "Verb");
    CHECK(track::getFormat(created[0]) == TrackFormat::mono);
    CHECK_FALSE(track::isInputStereo(created[0])); // mono tracks record one input channel
    CHECK(track::getKind(created[2]) == TrackKind::bus);
    CHECK(track::getFormat(created[2]) == TrackFormat::stereo);
    CHECK(track::getKind(created[3]) == TrackKind::instrument);
    for (const auto& t : created)
        CHECK(Session::getParentFolder(t) == folder); // after "Inside", in its folder
    CHECK(rig.names() == std::vector<juce::String>{"First", "Folder", "Inside", "Vox 1", "Vox 2", "Verb", track::getName(created[3]), "Last"});
    CHECK(rig.editor.getSelectedTracks().size() == 4);

    session.getUndoManager().undo(); // one step
    CHECK(session.getNumTracks() == 4);

    // At the end instead, and a single named track keeps its name (made unique).
    rows.resize(1);
    rows[0].count = 1;
    rows[0].name = "First";
    const auto atEnd = ui::actions::addNewTracks(session, rig.editor, rows, false);
    REQUIRE(atEnd.size() == 1);
    CHECK(track::getName(atEnd[0]) == "First 2");
    CHECK(session.indexOf(atEnd[0]) == session.getNumTracks() - 1);
}

TEST_CASE("Track > Split into Mono makes a mono track per channel; Make Inactive / Active; track numbers", "[ui][tracks][mono]")
{
    ProjectRig rig;
    auto& session = rig.session();
    auto crowd = session.addTrack(TrackKind::audio, "Audience Far");
    session.addClip(crowd, juce::File::getCurrentWorkingDirectory().getChildFile("crowd.wav"), 0.0, 4.0);
    track::setInput(crowd, 2, true, nullptr);
    session.addDevice(crowd, "builtin:eq", "EQ");
    auto bus = session.addTrack(TrackKind::bus, "Bus");
    auto tail = session.addTrack(TrackKind::audio, "Tail");

    rig.select({crowd, bus}); // the bus cannot be split
    CHECK(ui::actions::getSplittableTracks(session, rig.editor).size() == 1);
    const auto mono = ui::actions::splitSelectedIntoMono(rig.project, rig.editor);
    REQUIRE(mono.size() == 2);
    CHECK(rig.names() == std::vector<juce::String>{"Audience Far", "Audience Far.L", "Audience Far.R", "Bus", "Tail"});
    for (int ch = 0; ch < 2; ++ch)
    {
        const auto& t = mono[static_cast<std::size_t>(ch)];
        CHECK(track::getFormat(t) == TrackFormat::mono);
        CHECK_THAT(track::getPan(t), WithinAbs(ch == 0 ? -1.0 : 1.0, 1e-9));
        CHECK(track::getInput(t) == 2 + ch);
        CHECK_FALSE(track::isInputStereo(t));
        CHECK(Session::getDevices(t).size() == 1);
        const auto clips = childrenOf(t, ids::clip);
        REQUIRE(clips.size() == 1);
        CHECK(clip::getSourceChannel(clips[0]) == ch);
    }
    CHECK(track::getFormat(crowd) == TrackFormat::stereo); // the source stays
    CHECK(rig.editor.getSelectedTracks().size() == 2);
    session.getUndoManager().undo();
    CHECK(session.getNumTracks() == 3);

    // Make Inactive / Make Active (the command reads Make Active once nothing selected is active).
    rig.select({crowd, tail});
    CHECK(ui::actions::anySelectedTrackActive(session, rig.editor));
    CHECK(ui::actions::setSelectedTracksActive(session, rig.editor, false));
    CHECK_FALSE(track::isActive(crowd));
    CHECK_FALSE(track::isActive(tail));
    CHECK(track::isActive(bus));
    CHECK_FALSE(ui::actions::anySelectedTrackActive(session, rig.editor));
    session.getUndoManager().undo();
    CHECK(track::isActive(crowd));
    CHECK(track::isActive(tail));

    // Track numbers count from 1 in arranger order.
    CHECK(ui::actions::getTrackByNumber(session, 1) == crowd);
    CHECK(ui::actions::getTrackByNumber(session, 3) == tail);
    CHECK_FALSE(ui::actions::getTrackByNumber(session, 4).isValid());
    CHECK_FALSE(ui::actions::getTrackByNumber(session, 0).isValid());

    // Track format and activity survive saving and loading.
    test::TempFolder temp;
    track::setActive(tail, false, nullptr);
    track::setFormat(tail, TrackFormat::mono, nullptr);
    const auto file = temp.dir.getChildFile("formats.dojo");
    REQUIRE(rig.project.saveAs(file).wasOk());
    ProjectRig other;
    REQUIRE(other.project.open(file).wasOk());
    const auto loaded = other.project.getSession().getTrack(2);
    CHECK(track::getName(loaded) == "Tail");
    CHECK_FALSE(track::isActive(loaded));
    CHECK(track::getFormat(loaded) == TrackFormat::mono);
}

TEST_CASE("The New Tracks, Duplicate and Scroll to Track dialogs", "[ui][tracks][dialogs]")
{
    juce::MessageManager::getInstance();

    // New Tracks: rows, formats only where they apply, counts clamped, + and -.
    ui::NewTracksPanel panel;
    REQUIRE(panel.getNumRows() == 1);
    panel.getRow(0).count.setText("3");
    panel.getRow(0).format.setSelectedId(1); // mono
    panel.getRow(0).name.setText("Crowd");
    panel.getRow(0).add.onClick(); // (what a click on "+" runs)
    REQUIRE(panel.getNumRows() == 2);
    CHECK(panel.getRow(1).format.getSelectedId() == 1); // a new row starts like the one above
    panel.getRow(1).type.setSelectedId(3, juce::sendNotificationSync); // instrument: stereo only
    CHECK_FALSE(panel.getRow(1).format.isEnabled());
    panel.getRow(1).count.setText("250");
    panel.addRow(1);
    panel.getRow(2).type.setSelectedId(2, juce::sendNotificationSync); // aux input
    CHECK(panel.getRow(2).format.isEnabled());
    panel.getRow(2).count.setText("");

    auto rows = panel.getRows();
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].count == 3);
    CHECK(rows[0].type == ui::actions::NewTrackType::audio);
    CHECK(rows[0].format == session::TrackFormat::mono);
    CHECK(rows[0].name == "Crowd");
    CHECK(rows[1].type == ui::actions::NewTrackType::instrument);
    CHECK(rows[1].format == session::TrackFormat::stereo);
    CHECK(rows[1].count == 99);
    CHECK(rows[2].type == ui::actions::NewTrackType::auxInput);
    CHECK(rows[2].count == 1);

    panel.removeRow(1);
    CHECK(panel.getNumRows() == 2);
    panel.removeRow(0);
    panel.removeRow(0); // the last row stays
    CHECK(panel.getNumRows() == 1);

    std::vector<ui::actions::NewTracks> created;
    bool after = false;
    panel.getAfterSelectionToggle().setToggleState(false, juce::dontSendNotification);
    panel.onCreate = [&](const std::vector<ui::actions::NewTracks>& r, bool a)
    {
        created = r;
        after = a;
    };
    panel.create();
    CHECK(created.size() == 1);
    CHECK_FALSE(after);

    // Duplicate: the settings follow the toggles; "after the last selected" only for several tracks.
    ui::actions::DuplicateSettings initial;
    initial.copies = 2;
    initial.options.automation = false;
    ui::DuplicateTracksPanel single(1, initial);
    CHECK(single.copies.getText() == "2");
    CHECK_FALSE(single.automation.getToggleState());
    CHECK_FALSE(single.afterLastSelected.isEnabled());
    single.devices.setToggleState(false, juce::dontSendNotification);
    single.copies.setText("4");
    auto settings = single.getSettings();
    CHECK(settings.copies == 4);
    CHECK(settings.options.clips);
    CHECK_FALSE(settings.options.automation);
    CHECK_FALSE(settings.options.devices);
    CHECK(settings.options.sends);
    CHECK_FALSE(settings.afterLastSelected);
    ui::DuplicateTracksPanel several(3, initial);
    several.afterLastSelected.setToggleState(true, juce::dontSendNotification);
    CHECK(several.getSettings().afterLastSelected);
    int duplicated = 0;
    several.onDuplicate = [&](const ui::actions::DuplicateSettings& s) { duplicated = s.copies; };
    several.duplicate();
    CHECK(duplicated == 2);

    // Scroll to Track: a number from 1 to the track count.
    ui::ScrollToTrackPanel scroll(juce::StringArray{"Kick", "Snare", "Bass"}, 2);
    CHECK(scroll.number.getText() == "2");
    CHECK(scroll.getNumber() == 2);
    int target = 0;
    scroll.onScroll = [&](int n) { target = n; };
    scroll.number.setText("4");
    CHECK(scroll.getNumber() == 0);
    scroll.confirm(); // not a track: nothing happens
    CHECK(target == 0);
    scroll.number.setText("3");
    scroll.confirm();
    CHECK(target == 3);
}

TEST_CASE("Track menu commands in the main window; Scroll to Track moves the arranger and the mixer", "[ui][tracks][scroll]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;

    engine::AudioEngine engine(0);
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    session::Recorder recorder(project, controller);
    session::Exporter exporter(project, controller);
    juce::ApplicationProperties settings;
    juce::PropertiesFile::Options options;
    options.applicationName = "test";
    options.filenameSuffix = ".settings";
    options.folderName = temp.dir.getFullPathName();
    settings.setStorageParameters(options);
    engine::PreviewService preview(engine, project.getAssets().getFormatManager());
    ui::Preferences preferences(*settings.getUserSettings());
    juce::ApplicationCommandManager commands;
    ui::AppServices services{project, controller, commands, plugins, recorder, exporter, settings, preview, preferences, {}, {}, {}};

    auto& session = project.getSession();
    std::vector<juce::ValueTree> tracks;
    for (int i = 1; i <= 30; ++i)
        tracks.push_back(session.addTrack(TrackKind::audio, "Track " + juce::String(i)));
    auto hidden = session.groupIntoFolder({tracks[24], tracks[25]}, "Strings");
    track::setCollapsed(hidden, true, nullptr);

    {
        ui::MainComponent main(services);
        main.setSize(1400, 900);
        commands.registerAllCommandsForTarget(&main);
        commands.setFirstCommandTarget(&main);
        auto& arranger = main.getArranger();

        // Make Inactive / Make Active follow the selection.
        juce::ApplicationCommandInfo info(0);
        main.getEditorState().selectTrack(track::getId(tracks[2]));
        main.getCommandInfo(ui::commands::toggleTrackActive, info);
        CHECK(info.shortName == "Make Inactive");
        REQUIRE(commands.invokeDirectly(ui::commands::toggleTrackActive, false));
        CHECK_FALSE(track::isActive(tracks[2]));
        main.getCommandInfo(ui::commands::toggleTrackActive, info);
        CHECK(info.shortName == "Make Active");
        REQUIRE(commands.invokeDirectly(ui::commands::toggleTrackActive, false));
        CHECK(track::isActive(tracks[2]));

        // Split into Mono through the command.
        main.getCommandInfo(ui::commands::splitIntoMono, info);
        CHECK((info.flags & juce::ApplicationCommandInfo::isDisabled) == 0);
        REQUIRE(commands.invokeDirectly(ui::commands::splitIntoMono, false));
        CHECK(track::getName(session.getTrack(3)) == "Track 3.L");
        CHECK(track::getName(session.getTrack(4)) == "Track 3.R");
        main.getCommandInfo(ui::commands::splitIntoMono, info); // the mono tracks are selected: nothing to split
        CHECK((info.flags & juce::ApplicationCommandInfo::isDisabled) != 0);
        session.getUndoManager().undo();

        // Scroll to Track: track 20 at the top of the arranger.
        CHECK(arranger.getViewState().scrollY == 0);
        REQUIRE(main.scrollToTrackNumber(20));
        CHECK(main.getEditorState().getSelectedTrack() == track::getId(tracks[19]));
        CHECK(arranger.getTrackHeaderArea(tracks[19]).getY() == ui::theme::metrics::rulerHeight);

        // A track inside a collapsed folder: the folder opens.
        const int number = session.indexOf(tracks[25]) + 1;
        REQUIRE(main.scrollToTrackNumber(number));
        CHECK_FALSE(track::isCollapsed(hidden));
        CHECK(!arranger.getTrackHeaderArea(tracks[25]).isEmpty());

        CHECK_FALSE(main.scrollToTrackNumber(0));
        CHECK_FALSE(main.scrollToTrackNumber(session.getNumTracks() + 1));

        // Set Record Tracks to Input Only / Auto Input: named after what it switches to.
        main.getCommandInfo(ui::commands::toggleRecordMonitoring, info);
        CHECK(info.shortName == "Set Record Tracks to Input Only");
        REQUIRE(commands.invokeDirectly(ui::commands::toggleRecordMonitoring, false));
        CHECK(session.isInputOnlyMonitoring());
        main.getCommandInfo(ui::commands::toggleRecordMonitoring, info);
        CHECK(info.shortName == "Set Record Tracks to Auto Input");
        REQUIRE(commands.invokeDirectly(ui::commands::toggleRecordMonitoring, false));
        CHECK_FALSE(session.isInputOnlyMonitoring());

        // Punch in / out (Transport menu), ticked while on.
        REQUIRE(commands.invokeDirectly(ui::commands::togglePunchIn, false));
        main.getCommandInfo(ui::commands::togglePunchIn, info);
        CHECK((info.flags & juce::ApplicationCommandInfo::isTicked) != 0);
        CHECK(session.isPunchInEnabled());
        CHECK_FALSE(session.isPunchOutEnabled());
        REQUIRE(commands.invokeDirectly(ui::commands::togglePunchOut, false));
        CHECK(session.isPunchOutEnabled());
        CHECK(session.getPunchWindow() == std::pair{session.getLoopStart(), session.getLoopEnd()});
    }

    // The mixer: the strip as close to the left as possible.
    ui::EditorState editor;
    ui::MixerPanel mixer(project, controller, editor);
    mixer.setSize(800, 400);
    mixer.scrollToTrack(tracks[10]);
    CHECK(mixer.getScrollX() == session.indexOf(tracks[10]) * ui::MixerPanel::kStripWidth);
    mixer.scrollToTrack(tracks[0]);
    CHECK(mixer.getScrollX() == 0);
}

TEST_CASE("Track > Delete on a VCA Master coalesces it; New gives each Master Fader its own path", "[ui][tracks][vca]")
{
    ProjectRig rig;
    auto& session = rig.session();
    auto vox = session.addTrack(TrackKind::audio, "Vox");
    auto gtr = session.addTrack(TrackKind::audio, "Gtr");
    track::setVolumeDb(gtr, -2.0, nullptr);
    auto vca = session.addTrack(TrackKind::vca, "VCA 1");
    track::setVolumeDb(vca, -6.0, nullptr);
    track::setMuted(vca, true, nullptr);
    track::setVca(vox, track::getId(vca), nullptr);
    track::setVca(gtr, track::getId(vca), nullptr);
    CHECK(session.getVcaMembers(vca).size() == 2);

    rig.select({vca});
    REQUIRE(ui::actions::deleteSelectedTracks(session, rig.editor));
    CHECK_FALSE(session.findTrack(track::getId(vca)).isValid());
    CHECK_THAT(track::getVolumeDb(vox), WithinAbs(-6.0, 1e-9));
    CHECK_THAT(track::getVolumeDb(gtr), WithinAbs(-8.0, 1e-9));
    CHECK(track::isMuted(vox)); // the VCA was muted
    CHECK(track::getVca(vox) == 0);

    session.getUndoManager().undo(); // one step: the VCA and its members as they were
    CHECK(session.findTrack(track::getId(vca)).isValid());
    CHECK_THAT(track::getVolumeDb(vox), WithinAbs(0.0, 1e-9));
    CHECK(track::getVca(vox) == track::getId(vca));
    CHECK_FALSE(track::isMuted(vox));

    // New: MIDI tracks, VCA Masters and Master Faders (the main output's first, then a bus's).
    auto bus = session.addTrack(TrackKind::bus, "Drums");
    std::vector<ui::actions::NewTracks> rows(3);
    rows[0].type = ui::actions::NewTrackType::midi;
    rows[1].type = ui::actions::NewTrackType::vcaMaster;
    rows[2].type = ui::actions::NewTrackType::masterFader;
    rows[2].count = 2;
    const auto created = ui::actions::addNewTracks(session, rig.editor, rows, false);
    REQUIRE(created.size() == 4);
    CHECK(track::getKind(created[0]) == TrackKind::midi);
    CHECK(track::getKind(created[1]) == TrackKind::vca);
    CHECK(track::getKind(created[2]) == TrackKind::masterFader);
    CHECK(track::getControlledBus(created[2]) == 0);
    CHECK(track::getControlledBus(created[3]) == track::getId(bus));
    CHECK(track::getName(created[2]).startsWith("Master"));
}

TEST_CASE("Track > New makes multichannel tracks; Split into Mono gives one mono track per speaker", "[ui][tracks][multichannel]")
{
    ProjectRig rig;
    auto& session = rig.session();

    // New: every format for audio tracks, aux inputs and folders.
    ui::NewTracksPanel panel;
    panel.getRow(0).format.setSelectedId(4, juce::sendNotificationSync); // 5.1
    panel.getRow(0).name.setText("Ambience");
    panel.addRow(0);
    panel.getRow(1).type.setSelectedId(2, juce::sendNotificationSync); // aux input
    panel.getRow(1).format.setSelectedId(6, juce::sendNotificationSync); // Dolby Digital
    auto rows = panel.getRows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].format == TrackFormat::surround51);
    CHECK(rows[1].format == TrackFormat::dolbyDigital);
    const auto created = ui::actions::addNewTracks(session, rig.editor, rows);
    REQUIRE(created.size() == 2);
    CHECK(track::getFormat(created[0]) == TrackFormat::surround51);
    CHECK(track::getKind(created[1]) == TrackKind::bus);
    CHECK(track::getFormat(created[1]) == TrackFormat::dolbyDigital);

    ui::actions::NewTracks folderRow;
    folderRow.type = ui::actions::NewTrackType::folder;
    folderRow.format = TrackFormat::surround71;
    CHECK(ui::actions::hasFormatChoice(folderRow.type));
    const auto folder = ui::actions::addNewTracks(session, rig.editor, {folderRow});
    REQUIRE(folder.size() == 1);
    CHECK(track::getFormat(folder[0]) == TrackFormat::surround71);

    // Split into Mono: L R C LFE Ls Rs, each at its speaker, playing its channel of the file.
    auto ambience = created[0];
    test::TempFolder temp;
    std::vector<std::vector<float>> six(6, std::vector<float>(4800, 0.1f));
    const auto file = test::writeMultichannelWav(temp.dir.getChildFile("ambience.wav"), six, 48000.0);
    const auto stereoFile = test::writeMultichannelWav(temp.dir.getChildFile("lr.wav"), {six[0], six[1]}, 48000.0);
    juce::String error;
    REQUIRE(rig.project.getAssets().loadNow(file, error) != nullptr);
    REQUIRE(rig.project.getAssets().loadNow(stereoFile, error) != nullptr);
    session.addClip(ambience, file, 0.0, 0.1);
    session.addClip(ambience, stereoFile, 1.0, 0.1); // only feeds L and R
    track::setInput(ambience, 8, true, nullptr);
    rig.select({ambience});
    REQUIRE(ui::actions::getSplittableTracks(session, rig.editor).size() == 1);
    const auto mono = ui::actions::splitSelectedIntoMono(rig.project, rig.editor);
    REQUIRE(mono.size() == 6);
    const std::vector<juce::String> names{"Ambience.L", "Ambience.R", "Ambience.C", "Ambience.LFE", "Ambience.Ls", "Ambience.Rs"};
    for (int ch = 0; ch < 6; ++ch)
    {
        const auto& t = mono[static_cast<std::size_t>(ch)];
        CHECK(track::getName(t) == names[static_cast<std::size_t>(ch)]);
        CHECK(track::getFormat(t) == TrackFormat::mono);
        CHECK(track::getInput(t) == 8 + ch);
        const auto clips = childrenOf(t, ids::clip);
        REQUIRE(clips.size() == (ch < 2 ? 2u : 1u)); // the stereo file has nothing for C, LFE, Ls, Rs
        CHECK(clip::getSourceChannel(clips[0]) == ch);
    }
    CHECK_THAT(track::getPan(mono[0]), WithinAbs(-1.0, 1e-9));
    CHECK_THAT(track::getPanFrontRear(mono[0]), WithinAbs(-1.0, 1e-9));
    CHECK_THAT(track::getPanCentre(mono[2]), WithinAbs(1.0, 1e-9)); // C: the centre speaker
    CHECK_THAT(track::getPan(mono[5]), WithinAbs(1.0, 1e-9));       // Rs: right, rear
    CHECK_THAT(track::getPanFrontRear(mono[5]), WithinAbs(1.0, 1e-9));
}

TEST_CASE("The mixer: surround panner, multichannel meters and the master format", "[ui][mixer][multichannel]")
{
    ProjectRig rig;
    auto& session = rig.session();
    auto vocal = session.addTrack(TrackKind::audio, "Vocal");
    auto ambience = session.addTrack(TrackKind::audio, "Ambience");
    track::setFormat(ambience, TrackFormat::surround51, nullptr);

    ui::ChannelStrip vocalStrip(session, rig.editor, vocal, [] { return juce::StringArray{"1", "2", "3", "4", "5", "6", "7", "8"}; }, [] {});
    ui::ChannelStrip ambienceStrip(session, rig.editor, ambience, [] { return juce::StringArray{"1", "2", "3", "4", "5", "6", "7", "8"}; }, [] {});
    ui::MasterStrip master(session, rig.editor, [] {});
    for (juce::Component* c : std::initializer_list<juce::Component*>{&vocalStrip, &ambienceStrip})
        c->setSize(ui::MixerPanel::kStripWidth, 520);
    master.setSize(120, 520);

    // Stereo master: plain pan knobs.
    CHECK_FALSE(vocalStrip.isSurroundPanning());
    CHECK_FALSE(ambienceStrip.isSurroundPanning());

    // The master format box (undoable): a 5.1 master gives the stereo track a surround panner.
    master.getFormatBox().setSelectedId(4, juce::sendNotificationSync); // 5.1
    CHECK(session.getMasterFormat() == TrackFormat::surround51);
    CHECK(vocalStrip.isSurroundPanning());
    CHECK(vocalStrip.getSurroundPanner().isVisible());
    CHECK_FALSE(ambienceStrip.isSurroundPanning()); // a 5.1 track keeps its pan (balance)

    // Dragging the puck to the rear right: pan and front/rear, one undo step.
    auto& panner = vocalStrip.getSurroundPanner();
    REQUIRE(panner.getWidth() > 20);
    const auto at = panner.toPoint(0.5, 1.0);
    const auto now = juce::Time::getCurrentTime();
    const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
    const auto event = [&](juce::Point<float> p)
    {
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p, mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &panner, &panner, now,
                                panner.toPoint(0.0, -1.0), now, 1, true);
    };
    panner.mouseDown(event(panner.toPoint(0.0, -1.0)));
    panner.mouseDrag(event(at));
    panner.mouseUp(event(at));
    CHECK_THAT(track::getPan(vocal), WithinAbs(0.5, 0.02));
    CHECK_THAT(track::getPanFrontRear(vocal), WithinAbs(1.0, 0.02));
    session.getUndoManager().undo();
    CHECK_THAT(track::getPan(vocal), WithinAbs(0.0, 1e-9));
    CHECK_THAT(track::getPanFrontRear(vocal), WithinAbs(-1.0, 1e-9));

    // The multichannel track's inputs: as many as it has channels.
    track::setInput(ambience, 2, true, nullptr);
    CHECK(track::getInput(ambience) == 2);

    // Meters: one bar per channel of the destination.
    const std::array<float, 6> peaks{0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};
    vocalStrip.pushLevels(peaks.data(), 6, 0.02);
    CHECK(vocalStrip.getMeter().getNumChannels() == 6);
    master.pushLevels(peaks.data(), 6, 0.02);
    CHECK(master.getMeter().getNumChannels() == 6);

    // Back to stereo: the pan knob returns.
    session.getUndoManager().undo();
    CHECK(session.getMasterFormat() == TrackFormat::stereo);
    CHECK_FALSE(vocalStrip.isSurroundPanning());
    CHECK(master.getFormatBox().getSelectedId() == 2);
}

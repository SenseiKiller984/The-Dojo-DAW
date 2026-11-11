#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"
#include "ui/components/MixerPanel.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace dojo;
using session::Session;
namespace track = session::track;

namespace
{
std::set<std::uint32_t> idsOf(std::initializer_list<juce::ValueTree> tracks)
{
    std::set<std::uint32_t> ids;
    for (const auto& t : tracks)
        ids.insert(track::getId(t));
    return ids;
}

std::vector<juce::String> namesInOrder(const Session& session)
{
    std::vector<juce::String> names;
    for (const auto& t : session.getAllTracks())
        names.push_back(track::getName(t));
    return names;
}

/** An arranger over four audio tracks named like the user's live recording. */
struct HeaderRig
{
    HeaderRig()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); })
    {
        juce::MessageManager::getInstance();
        auto& session = project.getSession();
        for (const char* name : {"Audience Far - L", "Audience Far - R", "Audience Near - L", "Audience Near - R", "Vocals"})
            tracks.push_back(session.addTrack(session::TrackKind::audio, name));
        arranger = std::make_unique<ui::ArrangerView>(project, controller, editor);
        arranger->setSize(1400, 600);
    }

    /** A point on a track's header away from its buttons and fold arrow (the second line of text). */
    juce::Point<int> headerPoint(const juce::ValueTree& t) const
    {
        const auto area = arranger->getTrackHeaderArea(t);
        REQUIRE(!area.isEmpty());
        return {area.getX() + 70, area.getY() + 42};
    }

    juce::MouseEvent event(juce::Point<int> at, juce::ModifierKeys mods, juce::Point<int> downAt) const
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at.toFloat(), mods,
                                juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                juce::MouseInputSource::defaultTiltY, arranger.get(), arranger.get(), now, downAt.toFloat(), now, 1,
                                at != downAt);
    }

    /** A real click (mouse down and up) on a track's header, with keyboard modifiers. */
    void click(const juce::ValueTree& t, juce::ModifierKeys keys = {})
    {
        const auto at = headerPoint(t);
        const auto mods = keys.withFlags(juce::ModifierKeys::leftButtonModifier);
        arranger->mouseDown(event(at, mods, at));
        arranger->mouseUp(event(at, mods, at));
    }

    /** Drags a track's header and drops it onto `target`'s header (upper half: in front of it). */
    void drag(const juce::ValueTree& t, const juce::ValueTree& target)
    {
        const auto from = headerPoint(t);
        const auto area = arranger->getTrackHeaderArea(target);
        const juce::Point<int> to(from.x, area.getY() + 4);
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        arranger->mouseDown(event(from, mods, from));
        arranger->mouseDrag(event({from.x, (from.y + to.y) / 2}, mods, from));
        arranger->mouseDrag(event(to, mods, from));
        arranger->mouseUp(event(to, mods, from));
    }

    const std::set<std::uint32_t>& selection() const { return editor.getSelectedTracks(); }

    engine::AudioEngine engine{0};
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    session::Project project;
    ui::EditorState editor;
    std::unique_ptr<ui::ArrangerView> arranger;
    std::vector<juce::ValueTree> tracks;
};

const juce::ModifierKeys shift{juce::ModifierKeys::shiftModifier};
const juce::ModifierKeys ctrl{juce::ModifierKeys::commandModifier};
const juce::ModifierKeys ctrlShift{juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier};
} // namespace

TEST_CASE("Track selection works like selecting files: Shift ranges, Ctrl toggles", "[ui][selection][tracks]")
{
    ui::EditorState editor;
    const std::vector<std::uint32_t> order{11, 12, 13, 14, 15, 16};

    editor.selectTrack(12);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{12});

    // Shift: from the anchor to the clicked track, either direction; the anchor stays.
    editor.selectTrackWithModifiers(15, false, true, order);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{12, 13, 14, 15});
    CHECK(editor.getSelectedTrack() == 15); // the clicked track is the current one
    editor.selectTrackWithModifiers(11, false, true, order);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{11, 12});

    // Ctrl: toggles single tracks and moves the anchor.
    editor.selectTrackWithModifiers(14, true, false, order);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{11, 12, 14});
    CHECK(editor.getSelectedTrack() == 14);
    editor.selectTrackWithModifiers(14, true, false, order);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{11, 12});
    CHECK(editor.getSelectedTrack() == 12); // the nearest selected track takes over

    // Ctrl+Shift: adds the range from the anchor (14, the last Ctrl+click) to the clicked track.
    editor.selectTrackWithModifiers(16, true, true, order);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{11, 12, 14, 15, 16});

    // A plain click selects only that track.
    editor.selectTrackWithModifiers(13, false, false, order);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{13});

    // Ctrl+click on the only selected track keeps it (there is always a current track).
    editor.selectTrackWithModifiers(13, true, false, order);
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{13});
    CHECK(editor.getSelectedTrack() == 13);

    // Selecting the master clears nothing, but a track click deselects the master.
    editor.selectMaster();
    CHECK(editor.isMasterSelected());
    editor.selectTrackWithModifiers(16, false, true, order);
    CHECK_FALSE(editor.isMasterSelected());
    CHECK(editor.getSelectedTracks() == std::set<std::uint32_t>{13, 14, 15, 16});
}

TEST_CASE("Grouping several tracks into a folder keeps their order, even across folders", "[session][folder][tracks]")
{
    Session session;
    auto a = session.addTrack("A");
    auto b = session.addTrack("B");
    auto c = session.addTrack("C");
    auto d = session.addTrack("D");
    auto e = session.addTrack("E");
    auto inner = session.groupIntoFolder({c, d}, "Inner"); // A B [Inner: C D] E
    REQUIRE(inner.isValid());
    CHECK(namesInOrder(session) == std::vector<juce::String>{"A", "B", "Inner", "C", "D", "E"});

    // Passed out of order, from two levels: B (top level), D (inside Inner), E (top level).
    session.beginTransaction("Group");
    auto folder = session.groupIntoFolder({e, d, b}, "Crowd");
    REQUIRE(folder.isValid());
    CHECK(namesInOrder(session) == std::vector<juce::String>{"A", "Crowd", "B", "D", "E", "Inner", "C"});
    CHECK(Session::getParentFolder(b) == folder);
    CHECK(Session::getParentFolder(d) == folder);
    CHECK(Session::getParentFolder(e) == folder);
    CHECK(Session::getParentFolder(c) == inner);
    CHECK_FALSE(Session::getParentFolder(folder).isValid()); // where B (the topmost) was

    session.getUndoManager().undo(); // one step
    CHECK(namesInOrder(session) == std::vector<juce::String>{"A", "B", "Inner", "C", "D", "E"});
    CHECK(Session::getParentFolder(d) == inner);

    // A folder brings its contents: selecting Inner and C groups Inner (with C and D inside).
    auto outer = session.groupIntoFolder({c, inner, a}, "Outer");
    CHECK(namesInOrder(session) == std::vector<juce::String>{"Outer", "A", "Inner", "C", "D", "B", "E"});
    CHECK(Session::getParentFolder(inner) == outer);
    CHECK(Session::getParentFolder(c) == inner);

    CHECK_FALSE(session.groupIntoFolder({}).isValid());
    CHECK_FALSE(session.groupIntoFolder({juce::ValueTree("TRACK")}).isValid()); // not in the session
}

TEST_CASE("Moving several tracks keeps their order and can drop them into a folder", "[session][folder][tracks]")
{
    Session session;
    auto a = session.addTrack("A");
    auto b = session.addTrack("B");
    auto c = session.addTrack("C");
    auto d = session.addTrack("D");
    auto e = session.addTrack("E");
    auto folder = session.groupIntoFolder({e}, "F"); // A B C D [F: E]

    // B and D in front of A.
    session.moveTracks({d, b}, session.indexOf(a));
    CHECK(namesInOrder(session) == std::vector<juce::String>{"B", "D", "A", "C", "F", "E"});

    // Onto themselves: nothing changes.
    session.moveTracks({b, d}, session.indexOf(d));
    CHECK(namesInOrder(session) == std::vector<juce::String>{"B", "D", "A", "C", "F", "E"});

    // In front of E: into the folder.
    session.moveTracks({a, c}, session.indexOf(e));
    CHECK(namesInOrder(session) == std::vector<juce::String>{"B", "D", "F", "A", "C", "E"});
    CHECK(Session::getParentFolder(a) == folder);
    CHECK(Session::getParentFolder(c) == folder);

    // Below everything: out of the folder, at the end of the top level.
    session.moveTracks({a, b}, session.getNumTracks());
    CHECK(namesInOrder(session) == std::vector<juce::String>{"D", "F", "C", "E", "B", "A"});
    CHECK_FALSE(Session::getParentFolder(a).isValid());
}

TEST_CASE("Shift+click and Ctrl+click on track headers select tracks; Group into Folder takes them all", "[ui][selection][tracks]")
{
    HeaderRig rig;
    auto& session = rig.project.getSession();
    const auto& t = rig.tracks;

    // The user's case: click the first track, Shift+click the fourth.
    rig.click(t[0]);
    CHECK(rig.selection() == idsOf({t[0]}));
    rig.click(t[3], shift);
    CHECK(rig.selection() == idsOf({t[0], t[1], t[2], t[3]}));
    CHECK(rig.editor.getSelectedTrack() == track::getId(t[3]));

    // Ctrl+click takes one out and puts it back.
    rig.click(t[1], ctrl);
    CHECK(rig.selection() == idsOf({t[0], t[2], t[3]}));
    rig.click(t[1], ctrl);
    CHECK(rig.selection() == idsOf({t[0], t[1], t[2], t[3]}));

    // Every selected header is drawn highlighted (brighter than an unselected one).
    {
        const auto snapshot = rig.arranger->createComponentSnapshot(rig.arranger->getLocalBounds(), true, 1.0f);
        const auto backgroundAt = [&](const juce::ValueTree& tr)
        {
            const auto area = rig.arranger->getTrackHeaderArea(tr);
            return snapshot.getPixelAt(area.getRight() - 4, area.getBottom() - 4).getBrightness();
        };
        for (int i = 0; i < 4; ++i)
            CHECK(backgroundAt(t[static_cast<std::size_t>(i)]) > backgroundAt(t[4]));
    }

    // Mute, solo and arm on one of them switch all of them (one undo step); Vocals is untouched.
    for (const int button : {2, 1, 3})
    {
        INFO("button " << button);
        const auto at = rig.arranger->getTrackButtonArea(t[2], button).getCentre();
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier);
        rig.arranger->mouseDown(rig.event(at, mods, at));
        rig.arranger->mouseUp(rig.event(at, mods, at));
        const auto state = [button](const juce::ValueTree& tr)
        { return button == 2 ? track::isMuted(tr) : button == 1 ? track::isSoloed(tr) : track::isArmed(tr); };
        for (int i = 0; i < 4; ++i)
            CHECK(state(t[static_cast<std::size_t>(i)]));
        CHECK_FALSE(state(t[4]));
        CHECK(rig.selection() == idsOf({t[0], t[1], t[2], t[3]})); // the selection stays
        session.getUndoManager().undo();
        for (int i = 0; i < 4; ++i)
            CHECK_FALSE(state(t[static_cast<std::size_t>(i)]));
    }

    // Ctrl+Shift+G (Group into Folder) puts the four tracks into one folder, in order.
    const auto folder = ui::actions::groupSelectedTracksIntoFolder(session, rig.editor);
    REQUIRE(folder.isValid());
    CHECK(namesInOrder(session) == std::vector<juce::String>{track::getName(folder), "Audience Far - L", "Audience Far - R",
                                                             "Audience Near - L", "Audience Near - R", "Vocals"});
    for (int i = 0; i < 4; ++i)
        CHECK(Session::getParentFolder(t[static_cast<std::size_t>(i)]) == folder);
    CHECK(rig.selection() == idsOf({folder})); // the new folder is selected
    session.getUndoManager().undo();
    CHECK(namesInOrder(session) == std::vector<juce::String>{"Audience Far - L", "Audience Far - R", "Audience Near - L",
                                                             "Audience Near - R", "Vocals"});
}

TEST_CASE("Clicking and dragging the headers of a multi-selection", "[ui][selection][tracks]")
{
    HeaderRig rig;
    auto& session = rig.project.getSession();
    const auto& t = rig.tracks;

    rig.click(t[1]);
    rig.click(t[3], ctrl); // Far - R and Near - R
    CHECK(rig.selection() == idsOf({t[1], t[3]}));

    // Dragging one of them moves both, in order, in front of the first track.
    rig.drag(t[3], t[0]);
    CHECK(namesInOrder(session) == std::vector<juce::String>{"Audience Far - R", "Audience Near - R", "Audience Far - L",
                                                             "Audience Near - L", "Vocals"});
    CHECK(rig.selection() == idsOf({t[1], t[3]})); // still selected
    session.getUndoManager().undo(); // one step
    CHECK(namesInOrder(session) == std::vector<juce::String>{"Audience Far - L", "Audience Far - R", "Audience Near - L",
                                                             "Audience Near - R", "Vocals"});

    // A plain click (no drag) on one of them selects only it.
    rig.click(t[3]);
    CHECK(rig.selection() == idsOf({t[3]}));

    // Delete Tracks removes every selected track.
    rig.click(t[0], ctrl);
    CHECK(ui::actions::deleteSelectedTracks(session, rig.editor));
    CHECK(namesInOrder(session) == std::vector<juce::String>{"Audience Far - R", "Audience Near - L", "Vocals"});
    session.getUndoManager().undo();
    CHECK(session.getNumTracks() == 5);
}

TEST_CASE("Ctrl and Shift on mixer strips select tracks too", "[ui][selection][tracks]")
{
    juce::MessageManager::getInstance();
    Session session;
    ui::EditorState editor;
    std::vector<juce::ValueTree> tracks;
    std::vector<std::unique_ptr<ui::ChannelStrip>> strips;
    for (const char* name : {"A", "B", "C", "D"})
    {
        tracks.push_back(session.addTrack(name));
        strips.push_back(std::make_unique<ui::ChannelStrip>(session, editor, tracks.back(), [] { return juce::StringArray{"In 1"}; }, [] {}));
        strips.back()->setSize(90, 400);
    }

    const auto click = [](ui::ChannelStrip& strip, juce::ModifierKeys keys)
    {
        const juce::Point<float> at(45.0f, 4.0f); // the strip itself, above its controls
        const auto now = juce::Time::getCurrentTime();
        const juce::MouseEvent e(juce::Desktop::getInstance().getMainMouseSource(), at, keys.withFlags(juce::ModifierKeys::leftButtonModifier),
                                 juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                                 juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
                                 juce::MouseInputSource::defaultTiltY, &strip, &strip, now, at, now, 1, false);
        strip.mouseDown(e);
        strip.mouseUp(e);
    };

    click(*strips[0], {});
    click(*strips[2], shift);
    CHECK(editor.getSelectedTracks() == idsOf({tracks[0], tracks[1], tracks[2]}));
    click(*strips[1], ctrl);
    CHECK(editor.getSelectedTracks() == idsOf({tracks[0], tracks[2]}));
    click(*strips[3], ctrl);
    CHECK(editor.getSelectedTracks() == idsOf({tracks[0], tracks[2], tracks[3]}));
    click(*strips[1], {});
    CHECK(editor.getSelectedTracks() == idsOf({tracks[1]}));
}

#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using session::Session;
using Catch::Matchers::WithinAbs;
namespace clip = session::clip;

namespace
{
int numClips(const juce::ValueTree& t)
{
    int n = 0;
    for (const auto& c : t)
        n += Session::isClip(c) ? 1 : 0;
    return n;
}

/** An arranger over two audio tracks with a clip each (2..6 s), for drags. */
struct LockRig
{
    LockRig()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); })
    {
        juce::MessageManager::getInstance();
        auto& session = project.getSession();
        a = session.addTrack(session::TrackKind::audio, "A");
        b = session.addTrack(session::TrackKind::audio, "B");
        clipA = session.addClip(a, juce::File("/audio/a.wav"), 2.0, 4.0);
        clipB = session.addClip(b, juce::File("/audio/b.wav"), 8.0, 4.0);
        arranger = std::make_unique<ui::ArrangerView>(project, controller, editor);
        arranger->setSize(1400, 600);
    }

    juce::MouseEvent event(juce::Point<float> at, juce::ModifierKeys mods, juce::Point<float> downAt) const
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, mods, juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, arranger.get(),
                                arranger.get(), now, downAt, now, 1, at != downAt);
    }

    /** Drags from `from` by `by` pixels (Alt held: no snapping). */
    void drag(juce::Point<float> from, juce::Point<float> by)
    {
        const auto mods = juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | juce::ModifierKeys::altModifier);
        arranger->mouseDown(event(from, mods, from));
        arranger->mouseDrag(event(from + by * 0.5f, mods, from));
        arranger->mouseDrag(event(from + by, mods, from));
        arranger->mouseUp(event(from + by, mods, from));
    }

    /** The middle of a clip's body (moves it). */
    juce::Point<float> body(const juce::ValueTree& c) const { return arranger->getClipArea(c).toFloat().getCentre().translated(0.0f, 8.0f); }
    /** Just inside a clip's right edge, below the fade handles (trims its end). */
    juce::Point<float> rightEdge(const juce::ValueTree& c) const
    {
        const auto area = arranger->getClipArea(c).toFloat();
        return {area.getRight() - 2.0f, area.getBottom() - 6.0f};
    }
    float trackDistance() const { return static_cast<float>(arranger->getClipArea(clipB).getY() - arranger->getClipArea(clipA).getY()); }

    engine::AudioEngine engine{0};
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    session::Project project;
    ui::EditorState editor;
    std::unique_ptr<ui::ArrangerView> arranger;
    juce::ValueTree a, b, clipA, clipB;
};
} // namespace

TEST_CASE("Edit-locked clips cannot be moved, trimmed, split or deleted; copies stay locked", "[session][clip][locks]")
{
    Session session;
    auto t = session.addTrack(session::TrackKind::audio, "A");
    auto other = session.addTrack(session::TrackKind::audio, "B");
    auto c = session.addClip(t, juce::File("/audio/a.wav"), 2.0, 4.0);

    CHECK_FALSE(clip::isEditLocked(c));
    session.beginTransaction("Edit Lock");
    clip::setEditLocked(c, true, &session.getUndoManager());
    REQUIRE(clip::isEditLocked(c));
    CHECK_FALSE(clip::canEdit(c));
    CHECK_FALSE(clip::canMoveInTime(c));
    CHECK_FALSE(clip::canChangeTrack(c));
    CHECK_FALSE(clip::canRemove(c));

    // Not moved in time, nor to another track; not split, not deleted.
    session.beginTransaction("Move");
    session.moveClip(c, other, 5.0);
    CHECK(clip::getStart(c) == 2.0);
    CHECK(c.getParent() == t);
    CHECK_FALSE(session.splitClip(c, 3.0).isValid());
    CHECK(clip::getLength(c) == 4.0);
    CHECK_FALSE(session.removeClip(c));
    CHECK(numClips(t) == 1);

    // Recording over it leaves it alone.
    session.clearRange(t, 1.0, 7.0);
    CHECK(numClips(t) == 1);
    CHECK(clip::getLength(c) == 4.0);

    // Copies (paste anywhere, duplicate) are edit-locked too.
    auto pasted = session.pasteClip(c.createCopy(), other, 10.0);
    REQUIRE(pasted.isValid());
    CHECK(clip::getStart(pasted) == 10.0);
    CHECK(clip::isEditLocked(pasted));
    auto copy = session.duplicateClip(c);
    CHECK(clip::isEditLocked(copy));
    CHECK(clip::getStart(copy) == 6.0);

    // The lock is saved with the session.
    const auto reloaded = juce::ValueTree::fromXml(*c.createXml());
    CHECK(clip::isEditLocked(reloaded));

    // Unlocked, it edits again; locking undoes like any edit.
    session.beginTransaction("Edit Unlock");
    clip::setEditLocked(c, false, &session.getUndoManager());
    CHECK_FALSE(c.hasProperty(session::ids::editLocked));
    session.beginTransaction("Move");
    session.moveClip(c, t, 3.0);
    CHECK(clip::getStart(c) == 3.0);
    session.getUndoManager().undo(); // the move
    session.getUndoManager().undo(); // the unlock
    CHECK(clip::isEditLocked(c));
    CHECK(clip::getStart(c) == 2.0);
}

TEST_CASE("Time-locked clips stay at their time but can be trimmed, split and moved to another track", "[session][clip][locks]")
{
    Session session;
    auto t = session.addTrack(session::TrackKind::audio, "A");
    auto other = session.addTrack(session::TrackKind::audio, "B");
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    auto c = session.addClip(t, juce::File("/audio/a.wav"), 2.0, 4.0);
    clip::setTimeLocked(c, true, nullptr);
    CHECK(clip::canEdit(c));
    CHECK(clip::canChangeTrack(c));
    CHECK_FALSE(clip::canMoveInTime(c));
    CHECK_FALSE(clip::canRemove(c));

    // A move only changes the track (to one that can hold it); the start stays.
    session.moveClip(c, other, 5.0);
    CHECK(c.getParent() == other);
    CHECK(clip::getStart(c) == 2.0);
    session.moveClip(c, keys, 5.0); // an instrument track holds no audio
    CHECK(c.getParent() == other);
    CHECK(clip::getStart(c) == 2.0);

    // Not deleted, not recorded over.
    CHECK_FALSE(session.removeClip(c));
    session.clearRange(other, 0.0, 10.0);
    CHECK(numClips(other) == 1);

    // Split: both parts stay locked to their time locations.
    auto right = session.splitClip(c, 3.5);
    REQUIRE(right.isValid());
    CHECK(clip::isTimeLocked(right));
    CHECK(clip::getStart(right) == 3.5);
    CHECK(clip::isTimeLocked(c));
    CHECK(clip::getLength(c) == 1.5);

    // Trim (a property edit) is allowed.
    clip::setLength(right, 1.0, nullptr);
    CHECK(clip::getLength(right) == 1.0);

    // MIDI clips too.
    auto m = session.addMidiClip(keys, 4.0, 2.0);
    clip::setTimeLocked(m, true, nullptr);
    session.moveClip(m, keys, 1.0);
    CHECK(clip::getStart(m) == 4.0);
    CHECK(session.splitClip(m, 5.0).isValid());
}

TEST_CASE("Clip > Edit Lock / Time Lock toggle the selection; edits skip locked clips", "[ui][clip][locks]")
{
    Session session;
    ui::EditorState editor;
    auto t = session.addTrack(session::TrackKind::audio, "A");
    auto c1 = session.addClip(t, juce::File("/audio/1.wav"), 0.0, 2.0);
    auto c2 = session.addClip(t, juce::File("/audio/2.wav"), 3.0, 2.0);
    auto c3 = session.addClip(t, juce::File("/audio/3.wav"), 6.0, 2.0);
    using ui::actions::ClipLock;

    // Nothing selected: nothing to toggle.
    CHECK_FALSE(ui::actions::toggleSelectedClipsLock(session, editor, ClipLock::edit));
    CHECK_FALSE(ui::actions::selectedClipsLocked(session, editor, ClipLock::edit));

    // Any unlocked clip in the selection: the command locks them all (one undo step).
    clip::setEditLocked(c2, true, nullptr);
    editor.setSelectedClips({clip::getId(c1), clip::getId(c2)});
    CHECK_FALSE(ui::actions::selectedClipsLocked(session, editor, ClipLock::edit));
    REQUIRE(ui::actions::toggleSelectedClipsLock(session, editor, ClipLock::edit));
    CHECK(clip::isEditLocked(c1));
    CHECK(clip::isEditLocked(c2));
    CHECK(ui::actions::selectedClipsLocked(session, editor, ClipLock::edit));
    // ... and once all are locked it unlocks them.
    REQUIRE(ui::actions::toggleSelectedClipsLock(session, editor, ClipLock::edit));
    CHECK_FALSE(clip::isEditLocked(c1));
    CHECK_FALSE(clip::isEditLocked(c2));
    session.getUndoManager().undo();
    CHECK(clip::isEditLocked(c1));
    CHECK(clip::isEditLocked(c2));

    // Time lock is independent of the edit lock.
    editor.setSelectedClips({clip::getId(c3)});
    REQUIRE(ui::actions::toggleSelectedClipsLock(session, editor, ClipLock::time));
    CHECK(clip::isTimeLocked(c3));
    CHECK_FALSE(clip::isEditLocked(c3));

    // Delete: only the unlocked clips go; the locked ones stay selected.
    auto free = session.addClip(t, juce::File("/audio/4.wav"), 9.0, 1.0);
    editor.setSelectedClips({clip::getId(c1), clip::getId(c3), clip::getId(free)});
    REQUIRE(ui::actions::deleteSelectedClips(session, editor));
    CHECK(numClips(t) == 3);
    CHECK(c1.getParent() == t);
    CHECK(c3.getParent() == t);
    CHECK(editor.getSelectedClips() == std::set<std::uint32_t>{clip::getId(c1), clip::getId(c3)});
    CHECK_FALSE(ui::actions::deleteSelectedClips(session, editor)); // all locked: nothing to do

    // Cut takes only what it may remove; a locked-only selection cuts nothing.
    CHECK_FALSE(ui::actions::cutSelectedClips(session, editor));
    CHECK(numClips(t) == 3);

    // Split skips edit-locked clips but splits time-locked ones (the parts stay time-locked).
    editor.clearClipSelection();
    editor.setSelectedClips({clip::getId(c1), clip::getId(c3)});
    REQUIRE(ui::actions::splitAt(session, editor, 1.0) == false); // c1 is edit-locked; c3 is not under 1.0 s
    CHECK(clip::getLength(c1) == 2.0);
    REQUIRE(ui::actions::splitAt(session, editor, 7.0));
    CHECK(clip::getLength(c3) == 1.0);
    REQUIRE(editor.getSelectedClips().size() == 1);
    CHECK(clip::isTimeLocked(session.findClip(*editor.getSelectedClips().begin())));

    // Copy + paste of an edit-locked clip: the copy, wherever it lands, is edit-locked.
    editor.setSelectedClips({clip::getId(c1)});
    REQUIRE(ui::actions::copySelectedClips(session, editor));
    REQUIRE(ui::actions::pasteClips(session, editor, 20.0));
    REQUIRE(editor.getSelectedClips().size() == 1);
    const auto pasted = session.findClip(*editor.getSelectedClips().begin());
    CHECK(clip::getStart(pasted) == 20.0);
    CHECK(clip::isEditLocked(pasted));
}

TEST_CASE("The arranger does not drag locked clips out of place", "[ui][clip][locks]")
{
    LockRig rig;
    auto& session = rig.project.getSession();
    const float oneSecond = static_cast<float>(rig.arranger->getClipArea(rig.clipA).getWidth()) / 4.0f;
    REQUIRE(oneSecond > 10.0f);

    // Unlocked: a drag moves it by a second.
    rig.drag(rig.body(rig.clipA), {oneSecond, 0.0f});
    CHECK_THAT(clip::getStart(rig.clipA), WithinAbs(3.0, 0.02));
    session.getUndoManager().undo();
    CHECK_THAT(clip::getStart(rig.clipA), WithinAbs(2.0, 1e-9));

    // Edit-locked: it neither moves, nor trims, nor changes track.
    clip::setEditLocked(rig.clipA, true, nullptr);
    rig.drag(rig.body(rig.clipA), {oneSecond, rig.trackDistance()});
    CHECK(clip::getStart(rig.clipA) == 2.0);
    CHECK(rig.clipA.getParent() == rig.a);
    rig.drag(rig.rightEdge(rig.clipA), {oneSecond, 0.0f});
    CHECK(clip::getLength(rig.clipA) == 4.0);
    clip::setEditLocked(rig.clipA, false, nullptr);

    // Time-locked: a drag only takes it to another track, at the same time...
    clip::setTimeLocked(rig.clipA, true, nullptr);
    rig.drag(rig.body(rig.clipA), {oneSecond * 2.0f, rig.trackDistance()});
    CHECK(rig.clipA.getParent() == rig.b);
    CHECK(clip::getStart(rig.clipA) == 2.0);
    // ... and it can still be trimmed.
    rig.drag(rig.rightEdge(rig.clipA), {-oneSecond, 0.0f});
    CHECK_THAT(clip::getLength(rig.clipA), WithinAbs(3.0, 0.02));
    CHECK(clip::getStart(rig.clipA) == 2.0);
}

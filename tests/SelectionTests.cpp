#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/components/ArrangerView.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace dojo;

namespace
{
/** Two instrument tracks with MIDI clips: A at 0, 2, 4, 6 s and B at 1, 3, 5 s, each 1.5 s long. */
struct SelectionRig
{
    SelectionRig()
        : host(engine)
        , controller(engine, host)
        , project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); })
    {
        juce::MessageManager::getInstance();
        auto& session = project.getSession();
        auto trackA = session.addTrack(session::TrackKind::instrument, "A");
        auto trackB = session.addTrack(session::TrackKind::instrument, "B");
        for (const double start : {0.0, 2.0, 4.0, 6.0})
            a.push_back(session.addMidiClip(trackA, start, 1.5, "a"));
        for (const double start : {1.0, 3.0, 5.0})
            b.push_back(session.addMidiClip(trackB, start, 1.5, "b"));
        arranger = std::make_unique<ui::ArrangerView>(project, controller, editor);
        arranger->setSize(1600, 600);
    }

    static std::uint32_t id(const juce::ValueTree& clip) { return session::clip::getId(clip); }

    std::set<std::uint32_t> ids(std::initializer_list<juce::ValueTree> clips) const
    {
        std::set<std::uint32_t> result;
        for (const auto& c : clips)
            result.insert(id(c));
        return result;
    }

    void click(const juce::ValueTree& clip, juce::ModifierKeys mods = {}) { arranger->selectClipWithModifiers(clip, mods); }
    const std::set<std::uint32_t>& selection() const { return editor.getSelectedClips(); }

    /** The horizontal pixel of `seconds` (from a clip's drawn area). */
    int xOf(double seconds) const
    {
        const auto area = arranger->getClipArea(a[0]);
        const double pixelsPerSecond = area.getWidth() / 1.5;
        return area.getX() + juce::roundToInt((seconds - 0.0) * pixelsPerSecond);
    }

    engine::AudioEngine engine{0};
    engine::AudioDeviceHost host;
    engine::EngineController controller;
    session::Project project;
    ui::EditorState editor;
    std::unique_ptr<ui::ArrangerView> arranger;
    std::vector<juce::ValueTree> a, b;
};

const juce::ModifierKeys shift{juce::ModifierKeys::shiftModifier};
const juce::ModifierKeys ctrl{juce::ModifierKeys::commandModifier};
const juce::ModifierKeys ctrlShift{juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier};
} // namespace

TEST_CASE("Shift+click selects the clips between the anchor and the clicked clip on its track", "[ui][selection]")
{
    SelectionRig rig;

    rig.click(rig.a[1]);
    CHECK(rig.selection() == rig.ids({rig.a[1]}));

    rig.click(rig.a[3], shift);
    CHECK(rig.selection() == rig.ids({rig.a[1], rig.a[2], rig.a[3]}));

    // The anchor stays where the range started, like in Explorer.
    rig.click(rig.a[0], shift);
    CHECK(rig.selection() == rig.ids({rig.a[0], rig.a[1]}));

    // Ctrl toggles single clips (and moves the anchor).
    rig.click(rig.a[3], ctrl);
    CHECK(rig.selection() == rig.ids({rig.a[0], rig.a[1], rig.a[3]}));
    rig.click(rig.a[0], ctrl);
    CHECK(rig.selection() == rig.ids({rig.a[1], rig.a[3]}));

    // Ctrl+Shift adds a range; only the clicked clip's track is ranged (the anchor gives the time).
    rig.click(rig.a[3]);                 // anchor at 6 s (already selected: selection unchanged)
    rig.click(rig.b[1], ctrlShift);      // B clips starting between 3 and 6 s
    CHECK(rig.selection() == rig.ids({rig.a[1], rig.a[3], rig.b[1], rig.b[2]}));

    // Shift alone replaces the selection with the range.
    rig.click(rig.b[0], shift);          // B clips starting between 1 and 6 s
    CHECK(rig.selection() == rig.ids({rig.b[0], rig.b[1], rig.b[2]}));

    // A plain click on an unselected clip selects only it.
    rig.click(rig.a[2]);
    CHECK(rig.selection() == rig.ids({rig.a[2]}));
}

TEST_CASE("The rubber band selects every clip it touches, across tracks", "[ui][selection]")
{
    SelectionRig rig;
    const auto rowA = rig.arranger->getClipArea(rig.a[0]);
    const auto rowB = rig.arranger->getClipArea(rig.b[0]);
    REQUIRE(!rowA.isEmpty());
    REQUIRE(rowB.getY() > rowA.getBottom()); // B is below A

    // Inside track A only, from 2.6 to 3.2 s: only A's clip at 2 s.
    rig.arranger->selectClipsInArea({rig.xOf(2.6), rowA.getY() + 5, rig.xOf(3.2) - rig.xOf(2.6), 10}, {});
    CHECK(rig.selection() == rig.ids({rig.a[1]}));

    // Down into track B: B's clip at 3 s is touched too; B's clip at 1 s ends (2.5 s) before the band.
    rig.arranger->selectClipsInArea({rig.xOf(2.6), rowA.getY() + 5, rig.xOf(3.2) - rig.xOf(2.6), rowB.getCentreY() - rowA.getY()}, {});
    CHECK(rig.selection() == rig.ids({rig.a[1], rig.b[1]}));

    // A wide band over both tracks.
    rig.arranger->selectClipsInArea({rig.xOf(0.5), rowA.getCentreY(), rig.xOf(5.2) - rig.xOf(0.5), rowB.getCentreY() - rowA.getCentreY()}, {});
    CHECK(rig.selection() == rig.ids({rig.a[0], rig.a[1], rig.a[2], rig.b[0], rig.b[1], rig.b[2]}));

    // Ctrl/Shift drags add to the selection they started from.
    rig.arranger->selectClipsInArea({rig.xOf(6.2), rowA.getCentreY(), 20, 4}, rig.ids({rig.b[0]}));
    CHECK(rig.selection() == rig.ids({rig.a[3], rig.b[0]}));

    // Empty space selects nothing (without a base).
    rig.arranger->selectClipsInArea({rig.xOf(8.0), rowA.getCentreY(), 30, 30}, {});
    CHECK(rig.selection().empty());
    CHECK(rig.arranger->getMarqueeArea().isEmpty()); // no band while not dragging
}

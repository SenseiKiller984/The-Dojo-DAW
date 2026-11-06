#include "devices/DeviceRegistry.h"
#include "devices/Echo.h"
#include "devices/Impulse.h"
#include "devices/Instruments.h"
#include "devices/Operator.h"
#include "devices/WavetableSynth.h"
#include "session/Project.h"
#include "ui/BrowserDrag.h"
#include "ui/DeviceActions.h"
#include "ui/EditorState.h"
#include "ui/devices/DeviceEditors.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>

using namespace dojo;

namespace
{
/** A project with one instrument track holding a native device of `type`, and that device's editor. */
struct EditorRig
{
    explicit EditorRig(const juce::String& type)
        : project([](std::unique_ptr<engine::RenderGraph>) {},
                  [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); })
    {
        juce::MessageManager::getInstance();
        auto& session = project.getSession();
        track = session.addTrack(session::TrackKind::instrument, "Drums");
        device = session.addDevice(track, type, devices::findBuiltinDevice(type)->name);
        node = project.getDevices().getNode(device); // what the device card does
        REQUIRE(node != nullptr);
        int width = 0;
        editor = ui::createDeviceEditor(*node, ui::DeviceEditorContext{project, editorState, device, track}, width);
        REQUIRE(editor != nullptr);
        CHECK(width > 0);
        editor->setSize(800, 200);
    }

    juce::DragAndDropTarget& target()
    {
        auto* t = dynamic_cast<juce::DragAndDropTarget*>(editor.get());
        REQUIRE(t != nullptr);
        return *t;
    }

    /** Drops browser files at `position` (editor coordinates), as a drag from the browser does. */
    void drop(const juce::StringArray& files, juce::Point<int> position)
    {
        const juce::DragAndDropTarget::SourceDetails details(ui::browserDrag::describe(files), nullptr, position);
        REQUIRE(target().isInterestedInDragSource(details));
        target().itemDragEnter(details);
        target().itemDragMove(details);
        target().itemDropped(details);
    }

    template <typename T>
    T& processor()
    {
        auto* p = dynamic_cast<T*>(&node->getProcessor());
        REQUIRE(p != nullptr);
        return *p;
    }

    test::TempFolder temp;
    session::Project project;
    ui::EditorState editorState;
    juce::ValueTree track, device;
    std::shared_ptr<engine::ProcessorNode> node;
    std::unique_ptr<juce::Component> editor;
};

juce::StringArray makeSamples(const juce::File& folder, int count)
{
    juce::StringArray paths;
    for (int i = 0; i < count; ++i)
        paths.add(test::writeWav(folder.getChildFile("hit " + juce::String(i + 1) + ".wav"), test::sine(100.0 * (i + 1), 48'000.0, 2400), 48'000.0)
                      .getFullPathName());
    return paths;
}
} // namespace

TEST_CASE("Browser files dropped on a Drum Rack pad load onto that pad", "[ui][drop]")
{
    EditorRig rig(devices::DrumRack::kTypeId);
    auto& rack = rig.processor<devices::DrumRack>();
    const auto files = makeSamples(rig.temp.dir, 3);

    // 800 x 200 editor: a 4x4 pad grid in the left 400 px, pad 1 bottom-left. (150, 125) is the
    // second column, third row from the top: pad index 5.
    rig.drop({files[0]}, {150, 125});
    REQUIRE(rack.getPadSample(5) != nullptr);
    CHECK(rack.getPadSample(5)->file.getFullPathName() == files[0]);
    for (int pad = 0; pad < devices::DrumRack::kPads; ++pad)
        if (pad != 5)
            CHECK(rack.getPadSample(pad) == nullptr);

    // Several files fill consecutive pads from the one under the mouse (pad 1 is bottom-left).
    rig.drop({files[1], files[2]}, {20, 190});
    CHECK(rack.getPadSample(0)->file.getFullPathName() == files[1]);
    CHECK(rack.getPadSample(1)->file.getFullPathName() == files[2]);

    // Device drags are not for the pads.
    const juce::DragAndDropTarget::SourceDetails deviceDrag(juce::var("DojoDevice:builtin:eq"), nullptr, {20, 190});
    CHECK_FALSE(rig.target().isInterestedInDragSource(deviceDrag));

    // Files from the file manager work the same way.
    auto* fileTarget = dynamic_cast<juce::FileDragAndDropTarget*>(rig.editor.get());
    REQUIRE(fileTarget != nullptr);
    fileTarget->filesDropped({files[2]}, 380, 10); // top-right: pad 16
    CHECK(rack.getPadSample(15)->file.getFullPathName() == files[2]);
}

TEST_CASE("Browser files dropped on an Impulse slot load onto that slot", "[ui][drop]")
{
    EditorRig rig(devices::Impulse::kTypeId);
    auto& impulse = rig.processor<devices::Impulse>();
    const auto files = makeSamples(rig.temp.dir, 2);

    // Slots are a 4x2 grid in the left 300 px: (190, 150) is the third column, second row: slot 7.
    rig.drop({files[0]}, {190, 150});
    REQUIRE(impulse.getSlotSample(6) != nullptr);
    CHECK(impulse.getSlotSample(6)->file.getFullPathName() == files[0]);
    CHECK(impulse.getSlotSample(0) == nullptr);

    rig.drop({files[1]}, {20, 20});
    CHECK(impulse.getSlotSample(0)->file.getFullPathName() == files[1]);

    // Saved with the project's device state.
    juce::MemoryBlock state;
    impulse.getStateInformation(state);
    devices::Impulse copy;
    copy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(copy.getSlotSample(6)->file.getFullPathName() == files[0]);
}

TEST_CASE("A browser file dropped on the Sampler becomes its sample", "[ui][drop]")
{
    EditorRig rig(devices::Sampler::kTypeId);
    const auto files = makeSamples(rig.temp.dir, 1);
    rig.drop(files, {100, 40});
    REQUIRE(rig.processor<devices::Sampler>().getSample() != nullptr);
    CHECK(rig.processor<devices::Sampler>().getSample()->file.getFullPathName() == files[0]);
}

TEST_CASE("Every native device gets an editor", "[ui][drop]")
{
    for (const auto& info : devices::getBuiltinDevices())
    {
        EditorRig rig(info.type);
        rig.editor->setSize(900, 190);
        rig.editor->resized();
        CHECK(rig.editor->getNumChildComponents() > 0);
    }
}

TEST_CASE("Adding an instrument replaces the track's instrument, effects are added after it", "[ui][drop]")
{
    EditorRig rig(devices::Sampler::kTypeId);
    auto& session = rig.project.getSession();
    const auto choice = [](const juce::String& name)
    {
        for (const auto& c : ui::deviceActions::getNativeChoices())
            if (c.name == name)
                return c;
        FAIL("no device " << name);
        return ui::deviceActions::DeviceChoice{};
    };

    ui::deviceActions::addDevice(session, rig.editorState, rig.track, choice("Wavetable"));
    auto chain = session::Session::getDevices(rig.track);
    REQUIRE(chain.size() == 1);
    CHECK(session::device::getType(chain[0]) == juce::String(devices::WavetableSynth::kTypeId));
    CHECK(session::track::getName(rig.track) == "Drums"); // named by the user: kept

    // A track named after its instrument takes the new instrument's name.
    session::track::setName(rig.track, "Wavetable", nullptr);

    ui::deviceActions::addDevice(session, rig.editorState, rig.track, choice("Echo"));
    ui::deviceActions::addDevice(session, rig.editorState, rig.track, choice("Operator"));
    CHECK(session::track::getName(rig.track) == "Operator");
    chain = session::Session::getDevices(rig.track);
    REQUIRE(chain.size() == 2);
    CHECK(session::device::getType(chain[0]) == juce::String(devices::Operator::kTypeId));
    CHECK(session::device::getType(chain[1]) == juce::String(devices::Echo::kTypeId));

    // One undo step brings the previous instrument back.
    session.getUndoManager().undo();
    chain = session::Session::getDevices(rig.track);
    REQUIRE(chain.size() == 2);
    CHECK(session::device::getType(chain[0]) == juce::String(devices::WavetableSynth::kTypeId));
}

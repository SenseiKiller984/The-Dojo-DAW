#include "devices/AutoFilter.h"
#include "devices/DeviceRegistry.h"
#include "devices/LfoDevice.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/components/MixerPanel.h"
#include "ui/devices/DeviceEditors.h"
#include "ui/devices/MoreDeviceEditors.h"
#include "ui/devices/PagedEditor.h"
#include "ui/devices/ParameterControl.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace dojo;
using Catch::Matchers::WithinAbs;

namespace
{
session::Project makeProject()
{
    return session::Project([](std::unique_ptr<engine::RenderGraph>) {},
                            [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
}

juce::MouseEvent clickOn(juce::Component& c)
{
    const auto now = juce::Time::getCurrentTime();
    const auto at = c.getLocalBounds().getCentre().toFloat();
    return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), at, juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier),
                            juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
                            juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY,
                            &c, &c, now, at, now, 1, false);
}
} // namespace

TEST_CASE("Every native device has an editor; every page of the paged ones names real parameters", "[ui][devices]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    ui::EditorState editorState;
    for (const auto& info : devices::getBuiltinDevices())
    {
        INFO(info.name.toStdString());
        auto track = session.addTrack(info.isInstrument ? session::TrackKind::instrument : session::TrackKind::audio, info.name);
        auto device = session.addDevice(track, info.type, info.name);
        auto node = project.getDevices().getNode(device);
        REQUIRE(node != nullptr);
        int width = 0;
        auto editor = ui::createDeviceEditor(*node, ui::DeviceEditorContext{project, editorState, device, track}, width);
        REQUIRE(editor != nullptr);
        CHECK(width > 0);
        editor->setSize(width, 230);
        (void) editor->createComponentSnapshot(editor->getLocalBounds()); // paints without trouble
        if (auto* paged = dynamic_cast<ui::PagedEditor*>(editor.get()))
        {
            CHECK(paged->getMissingParameters().isEmpty());
            INFO(paged->getMissingParameters().joinIntoString(", ").toStdString());
            for (std::size_t p = 0; p < paged->getNumPages(); ++p)
            {
                paged->showPage(p);
                (void) editor->createComponentSnapshot(editor->getLocalBounds());
            }
        }
    }
}

TEST_CASE("The LFO editor maps from its menu or by Learn, describes and unmaps", "[ui][lfo]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    ui::EditorState editorState;
    auto pad = session.addTrack(session::TrackKind::audio, "Pad");
    auto other = session.addTrack(session::TrackKind::audio, "Other");
    auto filterDevice = session.addDevice(other, devices::AutoFilter::kTypeId, "Auto Filter");
    auto lfoDevice = session.addDevice(pad, devices::LfoDevice::kTypeId, "LFO");
    auto bus = session.addTrack(session::TrackKind::bus, "Verb");
    session.addSend(pad, bus, -6.0);
    auto filterNode = project.getDevices().getNode(filterDevice);
    auto lfoNode = project.getDevices().getNode(lfoDevice);
    REQUIRE(filterNode != nullptr);
    REQUIRE(lfoNode != nullptr);

    int width = 0;
    auto component = ui::createDeviceEditor(*lfoNode, ui::DeviceEditorContext{project, editorState, lfoDevice, pad}, width);
    auto* editor = dynamic_cast<ui::LfoEditor*>(component.get());
    REQUIRE(editor != nullptr);
    CHECK(width == ui::LfoEditor::preferredWidth());
    component->setSize(width, 230);

    // What it can map: every track's fader, pan and sends, the master, every device parameter (not
    // its own).
    const auto candidates = editor->getCandidates();
    const auto has = [&](const juce::String& description)
    {
        return std::any_of(candidates.begin(), candidates.end(), [&](const auto& c) { return c.description == description; });
    };
    CHECK(has("Pad > Volume"));
    CHECK(has("Pad > Pan"));
    CHECK(has("Pad > Send to Verb"));
    CHECK(has("Master > Volume"));
    CHECK(has("Other > Auto Filter > Frequency"));
    CHECK_FALSE(std::any_of(candidates.begin(), candidates.end(), [](const auto& c) { return c.description.startsWith("Pad > LFO"); }));

    // Map the pan from the list.
    for (const auto& c : candidates)
        if (c.description == "Pad > Pan")
            CHECK(editor->map(c));
    CHECK(editor->getMappingDescriptions() == juce::StringArray{"Pad > Pan"});

    // Learn: every control is outlined, a click maps it (instead of changing it) and Learn ends.
    auto& filter = dynamic_cast<devices::AutoFilter&>(filterNode->getProcessor());
    juce::AudioProcessorParameter& resonance = *filter.resonance_;
    const float before = resonance.getValue();
    ui::ParameterControl control(resonance);
    control.setSize(64, 72);
    control.setVisible(true);
    editor->beginLearn();
    CHECK(ui::ParameterControl::isMapping());
    REQUIRE(test::pumpMessagesUntil([&] { return control.isShowingMapping(); }, 2000));
    // (the click lands on the control itself, not on its knob)
    CHECK(control.getComponentAt(control.getLocalBounds().getCentre()) == &control);
    control.mouseDown(clickOn(control));
    CHECK_FALSE(editor->isLearning());
    CHECK_FALSE(ui::ParameterControl::isMapping());
    CHECK(resonance.getValue() == before);
    CHECK(editor->getMappingDescriptions() == juce::StringArray{"Pad > Pan", "Other > Auto Filter > Resonance"});
    REQUIRE(test::pumpMessagesUntil([&] { return !control.isShowingMapping(); }, 2000));
    CHECK(control.getComponentAt(control.getLocalBounds().getCentre()) != &control); // the knob again

    // Undo takes a mapping back.
    session.getUndoManager().undo();
    CHECK(editor->getMappingDescriptions().size() == 1);
    session.getUndoManager().redo();

    // Unmapping puts a parameter back where it was before the LFO moved it.
    resonance.setValueNotifyingHost(0.9f);
    editor->unmap(1);
    CHECK_THAT(resonance.getValue(), WithinAbs(before, 1e-6));
    CHECK(editor->getMappingDescriptions() == juce::StringArray{"Pad > Pan"});

    // Full: eight at most.
    for (const auto& c : editor->getCandidates())
        editor->map(c);
    CHECK(session::Session::getModulations(lfoDevice).size() == static_cast<std::size_t>(session::modulation::kMaxMappings));
    REQUIRE(test::pumpMessagesUntil([] { return false; }, 300) == false); // (lets the rows rebuild)
    (void) component->createComponentSnapshot(component->getLocalBounds());
}

TEST_CASE("Mixer strips follow a fader and pan an LFO moves, without editing the session", "[ui][lfo][mixer]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Pad");
    session::track::setVolumeDb(track, -3.0, nullptr);
    ui::ChannelStrip strip(session, editorState, track, [] { return juce::StringArray{}; }, [] {});
    strip.setSize(90, 400);

    const int actions = session.getUndoManager().getNumActionsInCurrentTransaction();
    strip.showMovingValues(static_cast<float>(juce::Decibels::decibelsToGain(-12.0)), -0.5f);
    CHECK(strip.isVolumeMoving());
    CHECK(strip.isPanMoving());
    CHECK_THAT(strip.getShownVolumeDb(), WithinAbs(-12.0, 0.01));
    CHECK_THAT(strip.getShownPan(), WithinAbs(-0.5, 0.01));
    CHECK_THAT(session::track::getVolumeDb(track), WithinAbs(-3.0, 1e-9)); // the session is untouched
    CHECK(session.getUndoManager().getNumActionsInCurrentTransaction() == actions);

    // Not moving any more: back to the track's own settings.
    strip.showMovingValues(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN());
    CHECK_FALSE(strip.isVolumeMoving());
    CHECK_THAT(strip.getShownVolumeDb(), WithinAbs(-3.0, 0.01));
    CHECK_THAT(strip.getShownPan(), WithinAbs(0.0, 0.01));
}

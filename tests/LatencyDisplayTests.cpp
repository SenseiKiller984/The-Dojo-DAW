#include "devices/BuiltinProcessor.h"
#include "devices/DeviceRegistry.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/Latency.h"
#include "ui/components/DevicePanel.h"
#include "DeviceTestSupport.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>

using namespace dojo;

namespace
{
session::Project makeProject()
{
    return session::Project([](std::unique_ptr<engine::RenderGraph>) {},
                            [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
}

void setParameter(session::Project& project, const juce::ValueTree& device, const juce::String& id, float value)
{
    test::devices::setParameter(dynamic_cast<devices::BuiltinProcessor&>(project.getDevices().getNode(device)->getProcessor()), id, value);
}
} // namespace

TEST_CASE("Delays are written in milliseconds, with the samples behind them", "[ui][latency]")
{
    using namespace ui::latency;
    CHECK(formatMilliseconds(0, 48000.0) == "0 ms");
    CHECK(formatMilliseconds(1, 48000.0) == "0.02 ms");
    CHECK(formatMilliseconds(48, 48000.0) == "1.00 ms");
    CHECK(formatMilliseconds(109, 48000.0) == "2.27 ms");
    CHECK(formatMilliseconds(61, 44100.0) == "1.38 ms");
    CHECK(formatMilliseconds(480, 48000.0) == "10.0 ms");
    CHECK(formatMilliseconds(4800, 48000.0) == "100 ms");
    CHECK(formatMilliseconds(100, 0.0) == "0 ms");
    CHECK(formatSamples(1) == "1 sample");
    CHECK(formatSamples(349) == "349 samples");
    CHECK(formatRate(48000.0) == "48 kHz");
    CHECK(formatRate(44100.0) == "44.1 kHz");
}

TEST_CASE("The Devices panel shows each device's delay and the whole chain's", "[ui][latency]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Vox");
    auto eq = session.addDevice(track, "builtin:eq", "EQ");
    auto gate = session.addDevice(track, "builtin:gate", "Gate");
    auto overdrive = session.addDevice(track, "builtin:overdrive", "Overdrive");
    auto limiter = session.addDevice(track, "builtin:limiter", "Limiter");
    setParameter(project, gate, "lookahead", 1.0f); // 1 ms = 48 samples (the overdrive: 61, the limiter: 5 ms = 240)
    editorState.selectTrack(session::track::getId(track));
    project.getCompiler().rebuildNow();

    ui::DevicePanel panel(project, plugins, editorState);
    panel.setSize(1800, 300);
    CHECK(panel.getLatencyText(eq) == "0 ms");
    CHECK(panel.getLatencyText(gate) == "1.00 ms");
    CHECK(panel.getLatencyText(overdrive) == "1.27 ms");
    CHECK(panel.getLatencyText(limiter) == "5.00 ms");
    CHECK(panel.getChainLatencySamples() == 349);
    CHECK(panel.getChainLatencyText() == "CHAIN DELAY 7.27 ms");

    // The tooltips: the samples, and the chain's delay up to each device.
    CHECK(panel.getLatencyTooltip(eq).startsWith("Adds no delay."));
    CHECK(panel.getLatencyTooltip(gate).contains("Adds 1.00 ms of delay (48 samples at 48 kHz)."));
    CHECK(panel.getLatencyTooltip(overdrive).contains("Chain delay up to here: 2.27 ms (109 samples)."));
    CHECK(panel.getLatencyTooltip(limiter).contains("Chain delay up to here: 7.27 ms (349 samples)."));
    CHECK(panel.getChainLatencyTooltip().contains("This track's devices delay its sound by 7.27 ms (349 samples at 48 kHz)"));
    CHECK(panel.getChainLatencyTooltip().contains("everything plays 7.27 ms (349 samples) after the timeline"));
    (void) panel.createComponentSnapshot(panel.getLocalBounds()); // paints the badges

    // A bypassed device adds nothing (the chain shrinks by its delay)...
    session::device::setBypassed(limiter, true, nullptr);
    CHECK(panel.getLatencyText(limiter) == "0 ms");
    CHECK(panel.getLatencyTooltip(limiter).startsWith("Bypassed: adds no delay now (5.00 ms, 240 samples, when on)."));
    CHECK(panel.getChainLatencyText() == "CHAIN DELAY 2.27 ms");

    // ...and a setting that changes a device's delay shows at once (here within a refresh).
    setParameter(project, gate, "lookahead", 4.0f); // 10 ms
    panel.refreshLatencies();
    CHECK(panel.getLatencyText(gate) == "10.0 ms");
    CHECK(panel.getChainLatencySamples() == 480 + 61);
    CHECK(panel.getChainLatencyText() == "CHAIN DELAY 11.3 ms");
    // (the panel also refreshes by itself)
    setParameter(project, gate, "lookahead", 0.0f);
    REQUIRE(test::pumpMessagesUntil([&] { return panel.getChainLatencySamples() == 61; }, 2000));

    // The master's chain too.
    session.addDevice(session.getMaster(), "builtin:limiter", "Limiter");
    editorState.selectMaster();
    REQUIRE(test::pumpMessagesUntil([&] { return panel.getChainLatencySamples() == 240; }, 2000));
    CHECK(panel.getChainLatencyTooltip().startsWith("The master's devices delay its sound by 5.00 ms"));
}

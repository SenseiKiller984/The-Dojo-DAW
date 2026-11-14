#include "engine/ProcessorNode.h"
#include "plugins/ClapHosting.h"
#include "plugins/PluginManager.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdlib>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

const juce::File testClap{DOJO_TEST_CLAP_PATH};
const juce::File scanner{DOJO_SCANNER_PATH};

struct MessageThread
{
    MessageThread() { juce::MessageManager::getInstance(); }
};

void setEnvironment(const char* name, const char* value)
{
#if JUCE_WINDOWS
    _putenv_s(name, value);
#else
    if (value[0] == '\0')
        unsetenv(name);
    else
        setenv(name, value, 1);
#endif
}

juce::PluginDescription findType(plugins::PluginManager& manager, const juce::String& name)
{
    for (const auto& type : manager.getKnownPlugins().getTypes())
        if (type.name == name)
            return type;
    FAIL("plug-in not found: " << name);
    return {};
}

std::vector<double> runMono(engine::ProcessorNode& node, const std::vector<float>& input, juce::MidiBuffer* firstBlockMidi = nullptr)
{
    std::vector<double> output;
    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::MidiBuffer midi;
    for (std::size_t offset = 0; offset < input.size(); offset += kBlock)
    {
        for (int i = 0; i < kBlock; ++i)
            buffer.setSample(0, i, input[offset + static_cast<std::size_t>(i)]), buffer.setSample(1, i, input[offset + static_cast<std::size_t>(i)]);
        midi.clear();
        if (offset == 0 && firstBlockMidi != nullptr)
            midi = *firstBlockMidi;
        node.process(buffer, kBlock, midi, nullptr);
        for (int i = 0; i < kBlock; ++i)
            output.push_back(buffer.getSample(0, i));
    }
    return output;
}
} // namespace

TEST_CASE("CLAP plug-ins are scanned out of process", "[plugins][clap]")
{
    REQUIRE(testClap.existsAsFile());
    REQUIRE(scanner.existsAsFile());
    MessageThread messageThread;
    test::TempFolder temp;

    plugins::PluginManager manager(scanner, temp.dir.getChildFile("plugins.xml"));
    const auto report = manager.scanFiles("CLAP", {testClap.getFullPathName()});
    INFO(report.failed.joinIntoString("\n") << report.crashed.joinIntoString("\n"));
    CHECK(report.pluginsFound == 2);
    CHECK(report.crashed.isEmpty());

    const auto gain = findType(manager, "Dojo Test Gain");
    CHECK(gain.pluginFormatName == "CLAP");
    CHECK_FALSE(gain.isInstrument);
    CHECK(findType(manager, "Dojo Test Synth").isInstrument);

    // The list survives a restart.
    manager.save();
    plugins::PluginManager reloaded(scanner, temp.dir.getChildFile("plugins.xml"));
    CHECK(reloaded.getKnownPlugins().getNumTypes() == 2);
}

TEST_CASE("A plug-in that crashes while being scanned is blacklisted, not fatal", "[plugins][clap]")
{
    MessageThread messageThread;
    test::TempFolder temp;
    plugins::PluginManager manager(scanner, temp.dir.getChildFile("plugins.xml"));

    setEnvironment("DOJO_TEST_CLAP_CRASH", "1");
    const auto report = manager.scanFiles("CLAP", {testClap.getFullPathName()});
    setEnvironment("DOJO_TEST_CLAP_CRASH", "");

    CHECK(report.pluginsFound == 0);
    REQUIRE(report.crashed.size() == 1);
    CHECK(manager.getKnownPlugins().getBlacklistedFiles().contains(testClap.getFullPathName()));
    CHECK(manager.getBlacklistReason(testClap.getFullPathName()).contains("crashed"));

    manager.clearBlacklist();
    CHECK(manager.getKnownPlugins().getBlacklistedFiles().isEmpty());
}

TEST_CASE("A hosted CLAP effect processes audio, follows its parameter, reports latency and keeps state", "[plugins][clap]")
{
    MessageThread messageThread;
    test::TempFolder temp;
    plugins::PluginManager manager(scanner, temp.dir.getChildFile("plugins.xml"));
    REQUIRE(manager.scanFiles("CLAP", {testClap.getFullPathName()}).pluginsFound == 2);

    juce::String error;
    auto instance = manager.createInstance(findType(manager, "Dojo Test Gain"), kRate, kBlock, error);
    REQUIRE(instance != nullptr);
    auto* processor = instance.get();
    engine::ProcessorNode node(std::move(instance));
    node.prepare(kRate, kBlock, nullptr);
    CHECK(node.getLatencySamples() == 64);

    auto& parameters = processor->getParameters();
    REQUIRE(parameters.size() == 1);
    CHECK(parameters[0]->getName(32) == "Gain");
    CHECK_THAT(parameters[0]->getValue(), WithinAbs(0.5, 1e-6)); // 1.0 in 0..2

    std::vector<float> impulse(2048, 0.0f);
    impulse[100] = 0.5f;
    auto out = runMono(node, impulse);
    CHECK(test::indexOfPeak(out) == 164);
    CHECK_THAT(out[164], WithinAbs(0.5, 1e-6));

    parameters[0]->setValueNotifyingHost(0.25f); // gain 0.5
    out = runMono(node, impulse);
    CHECK_THAT(out[164], WithinAbs(0.25, 1e-6));
    CHECK(parameters[0]->getText(0.25f, 16) == "0.50x");

    juce::MemoryBlock state;
    processor->getStateInformation(state);
    REQUIRE(state.getSize() == sizeof(double));

    auto second = manager.createInstance(findType(manager, "Dojo Test Gain"), kRate, kBlock, error);
    REQUIRE(second != nullptr);
    second->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK_THAT(second->getParameters()[0]->getValue(), WithinAbs(0.25, 1e-6));
}

TEST_CASE("A hosted CLAP instrument plays notes", "[plugins][clap]")
{
    MessageThread messageThread;
    test::TempFolder temp;
    plugins::PluginManager manager(scanner, temp.dir.getChildFile("plugins.xml"));
    REQUIRE(manager.scanFiles("CLAP", {testClap.getFullPathName()}).pluginsFound == 2);

    juce::String error;
    auto instance = manager.createInstance(findType(manager, "Dojo Test Synth"), kRate, kBlock, error);
    REQUIRE(instance != nullptr);
    engine::ProcessorNode node(std::move(instance));
    node.prepare(kRate, kBlock, nullptr);
    REQUIRE(node.isInstrument());

    juce::MidiBuffer midi;
    midi.addEvent(juce::MidiMessage::noteOn(1, 69, 1.0f), 0);
    const auto out = runMono(node, std::vector<float>(24000, 0.0f), &midi);
    CHECK_THAT(test::estimateFrequency(out, 1000, 24000, kRate), WithinRel(440.0, 0.01));
    CHECK_THAT(test::peak(out), WithinAbs(0.25, 1e-3));
}

TEST_CASE("Hosted plug-ins take part in delay compensation", "[plugins][clap][pdc]")
{
    MessageThread messageThread;
    test::TempFolder temp;
    plugins::PluginManager manager(scanner, temp.dir.getChildFile("plugins.xml"));
    REQUIRE(manager.scanFiles("CLAP", {testClap.getFullPathName()}).pluginsFound == 2);

    session::Session session;
    session::DeviceManager devices(session,
                                   [&](const juce::ValueTree& d, double rate, int block, juce::String& error) -> std::unique_ptr<juce::AudioProcessor>
                                   {
                                       juce::PluginDescription description;
                                       if (const auto xml = juce::parseXML(d.getProperty(session::ids::description).toString()))
                                           description.loadFromXml(*xml);
                                       return manager.createInstance(description, rate, block, error);
                                   });
    devices.prepareAll(kRate, kBlock);

    auto a = session.addTrack();
    auto b = session.addTrack();
    auto d = session.addDevice(a, plugins::PluginManager::kDeviceType, "Dojo Test Gain");
    d.setProperty(session::ids::description, findType(manager, "Dojo Test Gain").createXml()->toString(), nullptr);

    session::AudioAssetCache assets;
    session::GraphBuilder builder;
    const auto graph = builder.build(session, assets, &devices);
    REQUIRE(graph->tracks.size() == 2);
    CHECK(graph->tracks[0].devices.size() == 1);
    CHECK(graph->tracks[0].compensationDelay == 0);
    CHECK(graph->tracks[1].compensationDelay == 64); // the dry track waits for the plug-in
    CHECK(graph->totalLatency == 64);
    (void)b;
}

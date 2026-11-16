// Insert loops: a device (the Rhodes Suitcase) holding devices of its own, which the engine runs
// between the device's send and its return.

#include "devices/BuiltinProcessor.h"
#include "devices/DeviceRegistry.h"
#include "devices/RhodesSuitcase.h"
#include "engine/AudioEngine.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Presets.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/DeviceActions.h"
#include "ui/EditorState.h"
#include "ui/PresetActions.h"
#include "ui/components/DevicePanel.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

std::unique_ptr<juce::AudioProcessor> createDevice(const juce::ValueTree& device, double, int, juce::String& error)
{
    auto processor = devices::createBuiltinDevice(device::getType(device));
    if (processor == nullptr)
        error = "unknown";
    return processor;
}

/** A session wired to a real engine (driven by the test as if it were the audio device). */
struct Rig
{
    Rig()
        : engine(2)
        , devices(session, createDevice)
    {
        engine.prepare(kRate, kBlock, 2);
        devices.setPlayHead(engine.getPlayHead());
        devices.prepareAll(kRate, kBlock);
        builder.setRenderFormat(kRate, kBlock);
    }

    void publish()
    {
        auto graph = builder.build(session, assets, &devices);
        graph->serial = ++serial;
        engine::command::SwapGraph swap;
        swap.graph = graph.release();
        REQUIRE(engine.post(swap));
    }

    void play()
    {
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
    }

    void render(double seconds)
    {
        const int total = static_cast<int>(seconds * kRate);
        std::vector<float> l(kBlock), r(kBlock);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < total; done += kBlock)
        {
            engine.process(nullptr, 0, channels, 2, kBlock);
            left.insert(left.end(), l.begin(), l.end());
            right.insert(right.end(), r.begin(), r.end());
            engine.releaseRetiredGraphs();
        }
    }

    void set(const juce::ValueTree& device, const juce::String& id, float value)
    {
        auto node = devices.getNode(device);
        REQUIRE(node != nullptr);
        auto* parameter = dynamic_cast<devices::BuiltinProcessor&>(node->getProcessor()).getParameterById(id);
        REQUIRE(parameter != nullptr);
        parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    }

    /** A Keys track with a Rhodes (no vibrato, on its line out: linear) playing two notes. */
    std::pair<juce::ValueTree, juce::ValueTree> keys()
    {
        auto t = session.addTrack(TrackKind::instrument, "Keys");
        auto rhodes = session.addDevice(t, devices::RhodesSuitcase::kTypeId, "Rhodes");
        set(rhodes, "intensity", 0.0f);
        set(rhodes, "out", 1.0f);
        auto clip = session.addMidiClip(t, 0.0, 2.0);
        session.addNote(clip, 0.0, 4.0, 57, 0.8f);
        session.addNote(clip, 0.0, 4.0, 64, 0.7f);
        return {t, rhodes};
    }

    engine::AudioEngine engine;
    Session session;
    AudioAssetCache assets;
    DeviceManager devices;
    GraphBuilder builder;
    std::uint64_t serial = 0;
    std::vector<float> left, right;
};

std::size_t at(double seconds) { return static_cast<std::size_t>(seconds * kRate); }
double toDb(double gain) { return 20.0 * std::log10(std::max(gain, 1e-30)); }

double maxDifference(const std::vector<float>& a, const std::vector<float>& b)
{
    double d = 0.0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        d = std::max(d, static_cast<double>(std::abs(a[i] - b[i])));
    return d;
}

Project makeProject()
{
    return Project([](std::unique_ptr<engine::RenderGraph>) {},
                   [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(device::getType(d)); });
}

float valueOf(Project& project, const juce::ValueTree& device, const juce::String& id)
{
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto* parameter = dynamic_cast<devices::BuiltinProcessor&>(node->getProcessor()).getParameterById(id);
    REQUIRE(parameter != nullptr);
    return parameter->convertFrom0to1(parameter->getValue());
}

void setValue(Project& project, const juce::ValueTree& device, const juce::String& id, float value)
{
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto* parameter = dynamic_cast<devices::BuiltinProcessor&>(node->getProcessor()).getParameterById(id);
    REQUIRE(parameter != nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

ui::deviceActions::DeviceChoice nativeChoice(const juce::String& type)
{
    for (const auto& choice : ui::deviceActions::getNativeChoices())
        if (choice.type == type)
            return choice;
    FAIL(type.toStdString() + " is not a native device");
    return {};
}
} // namespace

TEST_CASE("A device's insert loop runs its devices between the device's send and its return", "[engine][insertloop]")
{
    using Setup = std::function<void(Rig&, juce::ValueTree, juce::ValueTree)>;
    const auto render = [](const Setup& setup)
    {
        Rig rig;
        auto [t, rhodes] = rig.keys();
        if (setup)
            setup(rig, t, rhodes);
        rig.publish();
        rig.play();
        rig.render(0.6);
        return std::make_pair(rig.left, rig.right);
    };
    const auto [reference, referenceRight] = render({});
    REQUIRE(test::rms(reference, at(0.1), at(0.6)) > 1e-3);

    // A 6 dB pad in the loop: the suitcase gets 6 dB less (on the line out, exactly 6 dB).
    const auto [padded, paddedRight] = render([](Rig& rig, juce::ValueTree, juce::ValueTree rhodes)
                                              { rig.set(rig.session.addDevice(rhodes, "builtin:utility", "Pad"), "gain", -6.0f); });
    CHECK_THAT(toDb(test::rms(padded, at(0.1), at(0.6)) / test::rms(reference, at(0.1), at(0.6))), WithinAbs(-6.0, 0.02));

    // The loop sends mono and its return is summed to mono (the vibrato takes one signal): with the
    // left side's polarity flipped inside the loop the two cancel. After the Rhodes they would not.
    const auto [cancelled, cancelledRight] = render([](Rig& rig, juce::ValueTree, juce::ValueTree rhodes)
                                                    { rig.set(rig.session.addDevice(rhodes, "builtin:utility", "Flip"), "phasel", 1.0f); });
    CHECK(test::peak(cancelled) < 1e-6);
    const auto [after, afterRight] = render([](Rig& rig, juce::ValueTree t, juce::ValueTree)
                                            { rig.set(rig.session.addDevice(t, "builtin:utility", "Flip"), "phasel", 1.0f); });
    CHECK(test::rms(after, at(0.1), at(0.6)) > 0.9 * test::rms(reference, at(0.1), at(0.6)));

    // Switched off, the loop's devices are skipped (the Rhodes runs in one piece again).
    const auto [off, offRight] = render(
        [](Rig& rig, juce::ValueTree, juce::ValueTree rhodes)
        {
            rig.set(rig.session.addDevice(rhodes, "builtin:utility", "Pad"), "gain", -6.0f);
            rig.set(rhodes, "loop", 0.0f);
        });
    CHECK(maxDifference(off, reference) < 1e-6);
    // A bypassed device in the loop passes the send through unchanged.
    const auto [bypassed, bypassedRight] = render(
        [](Rig& rig, juce::ValueTree, juce::ValueTree rhodes)
        {
            auto pad = rig.session.addDevice(rhodes, "builtin:utility", "Pad");
            rig.set(pad, "gain", -6.0f);
            device::setBypassed(pad, true, nullptr);
        });
    CHECK(maxDifference(bypassed, reference) < 1e-6);
    // A bypassed Rhodes is silent, loop and all.
    const auto [silent, silentRight] = render(
        [](Rig& rig, juce::ValueTree, juce::ValueTree rhodes)
        {
            rig.session.addDevice(rhodes, "builtin:utility", "Pad");
            device::setBypassed(rhodes, true, nullptr);
        });
    CHECK(test::peak(silent) < 1e-9);

    // The compiled graph: the loop's devices hang under their host, in order, while the loop is on.
    Rig rig;
    auto [t, rhodes] = rig.keys();
    auto echo = rig.session.addDevice(rhodes, "builtin:echo", "Echo");
    auto pad = rig.session.addDevice(rhodes, "builtin:utility", "Pad");
    rig.session.addDevice(t, "builtin:eq", "EQ"); // (after the Rhodes, on the track)
    {
        const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
        REQUIRE(graph->tracks.size() == 1);
        const auto& devices = graph->tracks.front().devices;
        REQUIRE(devices.size() == 2);
        CHECK(devices[0].node == rig.devices.getNode(rhodes).get());
        REQUIRE(devices[0].loop.size() == 2);
        CHECK(devices[0].loop[0].node == rig.devices.getNode(echo).get());
        CHECK(devices[0].loop[1].node == rig.devices.getNode(pad).get());
        CHECK(devices[1].loop.empty());
    }
    rig.set(rhodes, "loop", 0.0f);
    {
        const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
        CHECK(graph->tracks.front().devices.front().loop.empty());
    }
}

TEST_CASE("Delay inside an insert loop counts for its track, and automation in it follows the sound", "[engine][insertloop][pdc]")
{
    Rig rig;
    auto [t, rhodes] = rig.keys();
    auto limiter = rig.session.addDevice(rhodes, "builtin:limiter", "Limiter"); // 5 ms lookahead = 240 samples
    auto pad = rig.session.addDevice(rhodes, "builtin:utility", "Pad");
    auto trim = rig.session.addDevice(t, "builtin:utility", "Trim");
    rig.session.getAutomation(t, automation::parameterTarget(device::getId(limiter), 0), true, 0.5);
    rig.session.getAutomation(t, automation::parameterTarget(device::getId(pad), 0), true, 0.5);
    rig.session.getAutomation(t, automation::parameterTarget(device::getId(trim), 0), true, 0.5);

    {
        const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
        CHECK(rig.builder.getTotalLatency() == 240);
        const auto& rt = graph->tracks.front();
        CHECK(rt.faderLatency == 240);
        // Each lane is read by the delay of what comes before its device: the limiter's lane at
        // once, the pad's (after the limiter, in the loop) and the trim's (after the Rhodes) later.
        REQUIRE(rt.parameterLanes.size() == 3);
        const auto parameterOf = [&](const juce::ValueTree& d) { return rig.devices.getNode(d)->getProcessor().getParameters()[0]; };
        for (const auto& lane : rt.parameterLanes)
        {
            if (lane.parameter == parameterOf(limiter))
                CHECK(lane.latency == 0);
            else
            {
                CHECK((lane.parameter == parameterOf(pad) || lane.parameter == parameterOf(trim)));
                CHECK(lane.latency == 240);
            }
        }
    }

    // A bypassed device in the loop adds no delay; neither does a switched-off loop.
    device::setBypassed(limiter, true, nullptr);
    (void) rig.builder.build(rig.session, rig.assets, &rig.devices);
    CHECK(rig.builder.getTotalLatency() == 0);
    device::setBypassed(limiter, false, nullptr);
    rig.set(rhodes, "loop", 0.0f);
    (void) rig.builder.build(rig.session, rig.assets, &rig.devices);
    CHECK(rig.builder.getTotalLatency() == 0);
    rig.set(rhodes, "loop", 1.0f);
    (void) rig.builder.build(rig.session, rig.assets, &rig.devices);
    CHECK(rig.builder.getTotalLatency() == 240);

    // The other tracks wait for it: an impulse on an audio track lines up with the delayed Keys.
    test::TempFolder temp;
    const auto impulse = test::writeImpulse(temp.dir.getChildFile("imp.wav"), 4800, 9600, kRate, 0.4f);
    juce::String error;
    auto asset = rig.assets.loadNow(impulse, error);
    REQUIRE(asset != nullptr);
    auto drums = rig.session.addTrack();
    rig.session.addClip(drums, impulse, 0.0, asset->getLengthSeconds());
    track::setMuted(t, true, nullptr);
    rig.publish();
    rig.play();
    rig.render(0.2);
    CHECK(test::indexOfPeak(rig.left) == 4800 + 240);
}

TEST_CASE("Sidechains into an insert loop order the tracks and refuse feedback", "[engine][insertloop][routing]")
{
    Rig rig;
    auto drumBus = rig.session.addTrack(TrackKind::bus, "Drum Bus");
    auto [keys, rhodes] = rig.keys();
    auto compressor = rig.session.addDevice(rhodes, "builtin:compressor", "Ducker");
    CHECK(rig.session.canFeed(keys, drumBus));

    device::setSidechain(compressor, track::getId(drumBus), nullptr);
    // The Keys now listen to the Drum Bus: feeding the Keys into it would close a loop.
    CHECK_FALSE(rig.session.canFeed(keys, drumBus));
    CHECK_FALSE(rig.session.addSend(keys, drumBus).isValid());

    // The Drum Bus renders first (an earlier level), so the sidechain is ready for the loop.
    const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
    const auto levelOf = [&](const juce::ValueTree& t)
    {
        for (std::size_t level = 0; level < graph->levels.size(); ++level)
            for (const int index : graph->levels[level])
                if (graph->tracks[static_cast<std::size_t>(index)].trackId == track::getId(t))
                    return static_cast<int>(level);
        return -1;
    };
    REQUIRE(levelOf(drumBus) >= 0);
    CHECK(levelOf(keys) > levelOf(drumBus));
    const auto& keysTrack = *std::find_if(graph->tracks.begin(), graph->tracks.end(),
                                          [&](const engine::RenderTrack& rt) { return rt.trackId == track::getId(keys); });
    REQUIRE(keysTrack.devices.front().loop.size() == 1);
    CHECK(keysTrack.devices.front().loop.front().sidechainSource >= 0);
}

TEST_CASE("Devices in an insert loop belong to their track: found, removed, undone, duplicated, saved", "[session][insertloop]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    auto rhodes = session.addDevice(keys, devices::RhodesSuitcase::kTypeId, "Rhodes");
    auto echo = session.addDevice(rhodes, "builtin:echo", "Echo");
    auto pad = session.addDevice(rhodes, "builtin:utility", "Pad");
    auto eq = session.addDevice(keys, "builtin:eq", "EQ");
    setValue(project, pad, "gain", -6.0f);

    CHECK(Session::getDevices(keys) == std::vector<juce::ValueTree>{rhodes, eq});
    CHECK(Session::getDevices(rhodes) == std::vector<juce::ValueTree>{echo, pad});
    CHECK(Session::getAllDevices(keys) == std::vector<juce::ValueTree>{rhodes, echo, pad, eq});
    CHECK(Session::getDeviceOwner(pad) == keys);
    CHECK(Session::getDeviceOwner(rhodes) == keys);
    CHECK(session.findDevice(device::getId(pad)) == pad);

    // Moving inside the loop.
    session.moveDevice(pad, 0);
    CHECK(Session::getDevices(rhodes) == std::vector<juce::ValueTree>{pad, echo});
    session.moveDevice(pad, 1);

    // A lane on the track for a parameter of a device in the loop.
    session.getAutomation(keys, automation::parameterTarget(device::getId(pad), 0), true, 0.4);
    REQUIRE(Session::getAutomationLanes(keys).size() == 1);

    // Removing the Rhodes takes its loop and the loop's lanes with it; undo brings all of it back.
    session.getUndoManager().beginNewTransaction();
    session.removeDevice(rhodes);
    CHECK_FALSE(session.findDevice(device::getId(pad)).isValid());
    CHECK(Session::getAutomationLanes(keys).empty());
    session.getUndoManager().undo();
    REQUIRE(session.findDevice(device::getId(pad)).isValid());
    CHECK(Session::getAutomationLanes(keys).size() == 1);
    CHECK_THAT(valueOf(project, session.findDevice(device::getId(pad)), "gain"), WithinAbs(-6.0, 1e-3));

    // Removing one device of the loop takes only its own lanes.
    session.getUndoManager().beginNewTransaction();
    session.removeDevice(session.findDevice(device::getId(pad)));
    CHECK(Session::getDevices(rhodes).size() == 1);
    CHECK(Session::getAutomationLanes(keys).empty());
    session.getUndoManager().undo();
    pad = session.findDevice(device::getId(pad));
    REQUIRE(pad.isValid());

    // Duplicating the track copies the loop with fresh ids; its lanes follow the copies.
    project.getDevices().storeStates();
    auto copy = session.duplicateTrack(keys, {}, keys);
    REQUIRE(copy.isValid());
    const auto copied = Session::getAllDevices(copy);
    REQUIRE(copied.size() == 4);
    CHECK(device::getType(copied[0]) == devices::RhodesSuitcase::kTypeId);
    CHECK(Session::getDevices(copied[0]).size() == 2);
    for (std::size_t i = 0; i < copied.size(); ++i)
        CHECK(device::getId(copied[i]) != device::getId(Session::getAllDevices(keys)[i]));
    const auto lanes = Session::getAutomationLanes(copy);
    REQUIRE(lanes.size() == 1);
    CHECK(automation::getTarget(lanes.front()) == automation::parameterTarget(device::getId(copied[2]), 0));
    CHECK_THAT(valueOf(project, copied[2], "gain"), WithinAbs(-6.0, 1e-3));
    // Without its devices, no lanes for any of them (also those that were in the loop).
    Session::DuplicateOptions noDevices;
    noDevices.devices = false;
    auto bare = session.duplicateTrack(keys, noDevices, copy);
    REQUIRE(bare.isValid());
    CHECK(Session::getAllDevices(bare).empty());
    CHECK(Session::getAutomationLanes(bare).empty());

    // Saved and opened again: the loop, its settings and its lanes.
    test::TempFolder temp;
    const auto file = temp.dir.getChildFile("loop.dojo");
    REQUIRE(project.saveAs(file).wasOk());
    auto other = makeProject();
    REQUIRE(other.open(file).wasOk());
    const auto loaded = other.getSession().getTrack(0);
    REQUIRE(track::getName(loaded) == "Keys");
    const auto loadedDevices = Session::getAllDevices(loaded);
    REQUIRE(loadedDevices.size() == 4);
    CHECK(Session::getDevices(loadedDevices[0]).size() == 2);
    CHECK(device::getName(loadedDevices[2]) == "Pad");
    CHECK_THAT(valueOf(other, loadedDevices[2], "gain"), WithinAbs(-6.0, 1e-3));
    CHECK(Session::getAutomationLanes(loaded).size() == 1);
}

TEST_CASE("Chain presets keep the devices of an insert loop", "[presets][insertloop]")
{
    namespace presets = session::presets;
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    auto rhodes = session.addDevice(keys, devices::RhodesSuitcase::kTypeId, "Rhodes");
    auto pad = session.addDevice(rhodes, "builtin:utility", "Pad");
    session.addDevice(rhodes, "builtin:echo", "Echo");
    session.addDevice(keys, "builtin:eq", "EQ");
    setValue(project, rhodes, "treble", 4.0f);
    setValue(project, pad, "gain", -9.0f);

    const auto file = temp.dir.getChildFile("Suitcase Chain.dojopreset");
    const auto saved = ui::presetActions::saveChain(project, keys, file);
    REQUIRE(saved.ok);

    // The file: the loop's devices in a LOOP element inside the Rhodes' DEVICE (after its settings).
    juce::String error;
    const auto preset = presets::Preset::load(file, error);
    REQUIRE(preset.has_value());
    REQUIRE(preset->devices.size() == 2);
    REQUIRE(preset->devices[0].loop.size() == 2);
    CHECK(preset->devices[0].loop[0].key == "builtin:utility");
    CHECK(preset->devices[0].loop[1].key == "builtin:echo");
    CHECK(preset->devices[1].loop.empty());
    const auto xml = preset->toXml();
    const auto* first = xml->getChildByName("DEVICE");
    REQUIRE(first != nullptr);
    REQUIRE(first->getNumChildElements() == 2);
    CHECK(first->getChildElement(0)->hasTagName("DOJO_DEVICE"));
    CHECK(first->getChildElement(1)->hasTagName("LOOP"));

    // Added to another track: the Rhodes with its loop, all with their settings, as one undo step.
    auto other = session.addTrack(TrackKind::instrument, "Keys 2");
    juce::String message;
    session.getUndoManager().beginNewTransaction();
    const auto added = ui::presetActions::addChain(project, plugins, editorState, other, *preset, &message);
    REQUIRE(added.isValid());
    CHECK(message.contains("4 devices"));
    const auto devices = Session::getAllDevices(other);
    REQUIRE(devices.size() == 4);
    CHECK(Session::getDevices(other).size() == 2);
    CHECK(Session::getDevices(devices[0]).size() == 2);
    CHECK_THAT(valueOf(project, devices[0], "treble"), WithinAbs(4.0, 1e-3));
    CHECK_THAT(valueOf(project, devices[1], "gain"), WithinAbs(-9.0, 1e-3));
    session.getUndoManager().undo();
    CHECK(Session::getAllDevices(other).empty());

    // Older readers skip the LOOP element; a single-device preset never keeps one.
    presets::Preset single;
    single.name = "One";
    auto settings = preset->devices[0];
    single.put(settings);
    const auto reread = presets::Preset::fromXml(*single.toXml(), error);
    REQUIRE(reread.has_value());
    CHECK(reread->devices.front().loop.empty());
}

TEST_CASE("The Devices panel shows the loop inside its device; effects go in by its button or a drag", "[ui][insertloop]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto keys = session.addTrack(TrackKind::instrument, "Keys");
    auto rhodes = session.addDevice(keys, devices::RhodesSuitcase::kTypeId, "Rhodes");
    auto limiter = session.addDevice(rhodes, "builtin:limiter", "Limiter");
    editorState.selectTrack(track::getId(keys));
    project.getCompiler().rebuildNow();

    ui::DevicePanel panel(project, plugins, editorState);
    panel.setSize(1800, 360);
    CHECK(panel.findCard(rhodes) != nullptr);
    CHECK(panel.findCard(limiter) != nullptr);
    auto* end = dynamic_cast<juce::DragAndDropTarget*>(panel.findLoopEnd(rhodes));
    REQUIRE(end != nullptr);
    CHECK(panel.findLoopEnd(limiter) == nullptr); // (a limiter has no loop)

    // The loop's delay counts in the chain's; with the loop switched off it does not.
    CHECK(panel.getLatencyText(limiter) == "5.00 ms");
    CHECK(panel.getChainLatencySamples() == 240);
    setValue(project, rhodes, "loop", 0.0f);
    panel.refreshLatencies();
    CHECK(panel.getChainLatencySamples() == 0);
    setValue(project, rhodes, "loop", 1.0f);
    panel.refreshLatencies();
    CHECK(panel.getChainLatencySamples() == 240);
    (void) panel.createComponentSnapshot(panel.getLocalBounds()); // paints the frame and the loop's end

    // Effects fit in a loop; instruments and the LFO (which moves other devices) do not.
    using ui::deviceActions::fitsInLoop;
    CHECK(fitsInLoop(nativeChoice("builtin:echo")));
    CHECK(fitsInLoop(nativeChoice("builtin:tape800")));
    CHECK_FALSE(fitsInLoop(nativeChoice(devices::RhodesSuitcase::kTypeId)));
    CHECK_FALSE(fitsInLoop(nativeChoice("builtin:sampler")));
    CHECK_FALSE(fitsInLoop(nativeChoice("builtin:lfo")));

    // Dragged from the browser onto the loop's end: into the loop, at its end.
    const auto details = [](const ui::deviceActions::DeviceChoice& choice)
    { return juce::DragAndDropTarget::SourceDetails(ui::deviceActions::toDragDescription(choice), nullptr, {5, 5}); };
    CHECK(end->isInterestedInDragSource(details(nativeChoice("builtin:echo"))));
    CHECK_FALSE(end->isInterestedInDragSource(details(nativeChoice("builtin:sampler"))));
    session.getUndoManager().beginNewTransaction();
    end->itemDropped(details(nativeChoice("builtin:echo")));
    REQUIRE(Session::getDevices(rhodes).size() == 2);
    CHECK(device::getType(Session::getDevices(rhodes).back()) == "builtin:echo");
    CHECK(Session::getDevices(keys).size() == 1);

    // The "+ Add to Loop" button's action: one undo step.
    session.getUndoManager().beginNewTransaction();
    const auto chorus = ui::deviceActions::addDeviceToLoop(session, rhodes, nativeChoice("builtin:chorus"));
    REQUIRE(chorus.isValid());
    CHECK(chorus.getParent() == rhodes);
    CHECK(session.getUndoManager().getUndoDescription() == "Add Chorus-Ensemble to the Loop");
    CHECK_FALSE(ui::deviceActions::addDeviceToLoop(session, rhodes, nativeChoice("builtin:sampler")).isValid());
    session.getUndoManager().undo();
    CHECK(Session::getDevices(rhodes).size() == 2);

    // The panel follows (rebuilt asynchronously): the new device's card is in the loop.
    juce::MessageManager::getInstance()->runDispatchLoopUntil(20);
    CHECK(panel.findCard(Session::getDevices(rhodes).back()) != nullptr);
}

TEST_CASE("Projects saved with the separate Suitcase Base open with its effects in the Rhodes' loop", "[session][insertloop][rhodes]")
{
    // A file from the build with the separate return device: Rhodes > Echo > Chorus > Suitcase Base > EQ.
    const auto stateOf = [](const juce::String& type, std::initializer_list<std::pair<const char*, double>> parameters)
    {
        juce::XmlElement xml("DOJO_DEVICE");
        xml.setAttribute("type", type);
        xml.setAttribute("version", 1);
        for (const auto& [id, value] : parameters)
        {
            auto* element = xml.createNewChildElement("PARAM");
            element->setAttribute("id", id);
            element->setAttribute("value", value);
        }
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(xml, block);
        return block.toBase64Encoding();
    };
    Session old;
    auto keys = old.addTrack(TrackKind::instrument, "Keys");
    auto rhodes = old.addDevice(keys, devices::RhodesSuitcase::kTypeId, "Rhodes");
    rhodes.setProperty(ids::state, stateOf(devices::RhodesSuitcase::kTypeId, {{"loop", 1.0}, {"intensity", 3.0}, {"treble", 4.0}}), nullptr);
    const auto echo = old.addDevice(keys, "builtin:echo", "Echo");
    const auto chorus = old.addDevice(keys, "builtin:chorus", "Chorus-Ensemble");
    auto base = old.addDevice(keys, "builtin:suitcasebase", "Suitcase Base");
    base.setProperty(ids::state, stateOf("builtin:suitcasebase", {{"intensity", 8.5}, {"cabinet", 1.0}, {"master", -2.0}}), nullptr);
    const auto eq = old.addDevice(keys, "builtin:eq", "EQ");
    old.getAutomation(keys, automation::parameterTarget(device::getId(base), 1), true, 0.5);
    old.getAutomation(keys, automation::parameterTarget(device::getId(echo), 0), true, 0.5);
    // (and a Suitcase Base with no Rhodes before it stays as it was)
    auto other = old.addTrack(TrackKind::audio, "Other");
    old.addDevice(other, "builtin:suitcasebase", "Suitcase Base");

    test::TempFolder temp;
    const auto file = temp.dir.getChildFile("old.dojo");
    REQUIRE(old.save(file).wasOk());
    Session session;
    REQUIRE(session.load(file).wasOk());

    const auto loaded = session.getTrack(0);
    const auto chain = Session::getDevices(loaded);
    REQUIRE(chain.size() == 2);
    CHECK(device::getType(chain[0]) == devices::RhodesSuitcase::kTypeId);
    CHECK(device::getType(chain[1]) == "builtin:eq");
    CHECK(device::getId(chain[1]) == device::getId(eq));
    const auto loop = Session::getDevices(chain[0]);
    REQUIRE(loop.size() == 2);
    CHECK(device::getId(loop[0]) == device::getId(echo));
    CHECK(device::getId(loop[1]) == device::getId(chorus));
    // The Base's lane went with it; the Echo's stayed (on the track).
    const auto lanes = Session::getAutomationLanes(loaded);
    REQUIRE(lanes.size() == 1);
    CHECK(automation::getTarget(lanes.front()) == automation::parameterTarget(device::getId(echo), 0));

    // The Rhodes took the Base's suitcase settings and kept its own.
    auto rhodesNow = std::make_unique<devices::RhodesSuitcase>();
    juce::MemoryBlock block;
    REQUIRE(block.fromBase64Encoding(chain[0].getProperty(ids::state).toString()));
    rhodesNow->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
    CHECK_THAT(rhodesNow->stage.intensity->get(), WithinAbs(8.5, 1e-4));
    CHECK(rhodesNow->stage.cabinet->getIndex() == 1);
    CHECK_THAT(rhodesNow->stage.master->get(), WithinAbs(-2.0, 1e-4));
    CHECK_THAT(rhodesNow->treble_->get(), WithinAbs(4.0, 1e-4));
    CHECK(rhodesNow->loop_->get());

    CHECK(Session::getDevices(session.getTrack(1)).size() == 1);
}

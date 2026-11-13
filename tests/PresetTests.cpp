#include "devices/ChannelEq.h"
#include "devices/DeviceRegistry.h"
#include "devices/Equaliser.h"
#include "devices/TimeEffects.h"
#include "session/Presets.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/PresetActions.h"
#include "ui/components/DevicePanel.h"
#include "EffectTestHelpers.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>

using namespace dojo;
using Catch::Matchers::WithinAbs;
namespace presets = session::presets;

namespace
{
session::Project makeProject()
{
    return session::Project([](std::unique_ptr<engine::RenderGraph>) {},
                            [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
}

/** Points the user's presets folder at a temporary folder for the test. */
struct UserFolder
{
    UserFolder() { presets::setUserFolder(temp.dir.getChildFile("Presets")); }
    ~UserFolder() { presets::setUserFolder({}); }
    [[nodiscard]] juce::File folder() const { return temp.dir.getChildFile("Presets"); }
    test::TempFolder temp;
};

float valueOf(juce::AudioProcessor& processor, const juce::String& id)
{
    auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(dynamic_cast<devices::BuiltinProcessor&>(processor).getParameterById(id));
    REQUIRE(parameter != nullptr);
    return parameter->convertFrom0to1(parameter->getValue());
}

void setValue(juce::AudioProcessor& processor, const juce::String& id, float value)
{
    test::devices::setParameter(dynamic_cast<devices::BuiltinProcessor&>(processor), id, value);
}

presets::Preset synthBite()
{
    for (auto& preset : presets::getBuiltIn())
        if (preset.name == "Synth Bite")
            return preset;
    FAIL("Synth Bite is not built in");
    return {};
}

juce::ValueTree deviceTree(const juce::String& type, const juce::String& name)
{
    juce::ValueTree device(session::ids::device);
    device.setProperty(session::ids::type, type, nullptr);
    device.setProperty(session::ids::name, name, nullptr);
    return device;
}

/** dB change of a tone through a device. */
double gainDbAt(juce::AudioProcessor& processor, double frequency)
{
    using namespace test::devices;
    juce::MemoryBlock state;
    processor.getStateInformation(state);
    auto copy = devices::createBuiltinDevice(dynamic_cast<devices::BuiltinProcessor&>(processor).getTypeId());
    copy->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    engine::ProcessorNode node(std::move(copy));
    node.prepare(kRate, kBlock, nullptr);
    const std::size_t n = 24'000;
    const auto in = tone(frequency, n, 0.1);
    const auto out = run(node, in);
    return juce::Decibels::gainToDecibels(amplitudeAt(out.left, frequency, n / 2, n) / amplitudeAt(in.left, frequency, n / 2, n));
}
} // namespace

TEST_CASE("A preset keeps every setting of a device and loads it back", "[presets]")
{
    devices::Equaliser eq;
    setValue(eq, "band3Gain", 7.5f);
    setValue(eq, "band3Freq", 2200.0f);
    setValue(eq, "band1Shape", 0.0f);
    setValue(eq, "band4On", 0.0f);
    setValue(eq, "output", -3.0f);

    presets::Preset preset;
    preset.name = "Bright";
    preset.put(presets::capture(deviceTree(devices::Equaliser::kTypeId, "EQ"), eq));
    REQUIRE(preset.devices.size() == 1);
    CHECK(preset.devices[0].key == devices::Equaliser::kTypeId);
    CHECK(preset.devices[0].deviceName == "EQ");

    // Written out readably: every parameter by id and value.
    const auto xml = preset.toXml();
    const auto text = xml->toString();
    CHECK(xml->hasTagName("DOJO_PRESET"));
    CHECK(text.contains("<PARAM id=\"band3Gain\" value=\"7.5"));
    CHECK(text.contains("id=\"band2Q\"")); // (unchanged settings too)
    CHECK_FALSE(xml->getChildByName("DEVICE")->hasAttribute("state"));

    // Into a fresh EQ, through a file.
    UserFolder user;
    const auto file = user.folder().getChildFile("Bright");
    REQUIRE(preset.save(file).wasOk());
    CHECK(user.folder().getChildFile("Bright.dojopreset").existsAsFile());
    juce::String error;
    const auto loaded = presets::Preset::load(user.folder().getChildFile("Bright.dojopreset"), error);
    REQUIRE(loaded.has_value());
    CHECK(loaded->name == "Bright");
    devices::Equaliser fresh;
    const auto* settings = loaded->findFor(devices::Equaliser::kTypeId);
    REQUIRE(settings != nullptr);
    presets::restore(*settings, fresh);
    CHECK_THAT(valueOf(fresh, "band3Gain"), WithinAbs(7.5, 1e-4));
    CHECK_THAT(valueOf(fresh, "band3Freq"), WithinAbs(2200.0, 0.5));
    CHECK_THAT(valueOf(fresh, "band1Shape"), WithinAbs(0.0, 1e-6));
    CHECK_THAT(valueOf(fresh, "band4On"), WithinAbs(0.0, 1e-6));
    CHECK_THAT(valueOf(fresh, "output"), WithinAbs(-3.0, 1e-4));
    CHECK_THAT(fresh.getMagnitudeAt(2200.0), WithinAbs(eq.getMagnitudeAt(2200.0), 1e-9));
}

TEST_CASE("One preset file holds settings for several kinds of device, one each", "[presets]")
{
    devices::Equaliser eq;
    devices::ChannelEq channelEq;
    setValue(eq, "band2Gain", -4.0f);
    setValue(channelEq, "mid", 6.0f);

    presets::Preset preset;
    preset.name = "Shared";
    preset.description = "For both";
    preset.put(presets::capture(deviceTree(devices::Equaliser::kTypeId, "EQ"), eq));
    preset.put(presets::capture(deviceTree(devices::ChannelEq::kTypeId, "Channel EQ"), channelEq));
    CHECK(preset.getDeviceNames() == juce::StringArray{"EQ", "Channel EQ"});
    CHECK(preset.fits(devices::Equaliser::kTypeId));
    CHECK(preset.fits(devices::ChannelEq::kTypeId));
    CHECK_FALSE(preset.fits("builtin:reverb"));

    // The same kind again replaces its entry.
    setValue(eq, "band2Gain", -8.0f);
    preset.put(presets::capture(deviceTree(devices::Equaliser::kTypeId, "EQ"), eq));
    CHECK(preset.devices.size() == 2);

    // A plug-in's own state is kept as it is (base64).
    presets::DeviceSettings plugin;
    plugin.key = "plugin:VST3:Somebody:Thing";
    plugin.deviceName = "Thing";
    const char bytes[] = {0, 1, 2, 3, static_cast<char>(0xff), 42};
    plugin.state.append(bytes, sizeof(bytes));
    preset.put(plugin);

    juce::String error;
    const auto parsed = presets::Preset::parse(preset.toXml()->toString(), error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->name == "Shared");
    CHECK(parsed->description == "For both");
    REQUIRE(parsed->devices.size() == 3);
    CHECK(parsed->findFor("plugin:VST3:Somebody:Thing")->state == plugin.state);

    // Each device takes its own entry.
    devices::Equaliser eq2;
    devices::ChannelEq channelEq2;
    presets::restore(*parsed->findFor(devices::Equaliser::kTypeId), eq2);
    presets::restore(*parsed->findFor(devices::ChannelEq::kTypeId), channelEq2);
    CHECK_THAT(valueOf(eq2, "band2Gain"), WithinAbs(-8.0, 1e-4));
    CHECK_THAT(valueOf(channelEq2, "mid"), WithinAbs(6.0, 1e-4));
    CHECK_THAT(valueOf(eq2, "output"), WithinAbs(0.0, 1e-6));
}

TEST_CASE("Files that are not presets are refused with a reason", "[presets]")
{
    juce::String error;
    CHECK_FALSE(presets::Preset::parse("not xml at all <", error).has_value());
    CHECK(error.isNotEmpty());
    CHECK_FALSE(presets::Preset::parse("<DOJO_SESSION/>", error).has_value());
    CHECK(error.contains("not a Dojo preset"));
    CHECK_FALSE(presets::Preset::parse("<DOJO_PRESET version=\"99\" name=\"x\"><DEVICE key=\"builtin:eq\" state=\"AAAA\"/></DOJO_PRESET>", error).has_value());
    CHECK(error.contains("newer"));
    CHECK_FALSE(presets::Preset::parse("<DOJO_PRESET version=\"1\" name=\"Empty\"/>", error).has_value());
    CHECK(error.contains("no device settings"));

    test::TempFolder temp;
    CHECK_FALSE(presets::Preset::load(temp.dir.getChildFile("missing.dojopreset"), error).has_value());

    CHECK(presets::isPresetFile(juce::File("/x/a.dojopreset")));
    CHECK_FALSE(presets::isPresetFile(juce::File("/x/a.wav")));
    CHECK(presets::hasNonPresetFile({"/x/a.wav", "/x/b.dojopreset"}));
    CHECK_FALSE(presets::hasNonPresetFile({"/x/b.dojopreset"}));
    CHECK(presets::fileNameFor("Lead: Bright?") .endsWith(".dojopreset"));
    CHECK_FALSE(presets::fileNameFor("Lead: Bright?").containsAnyOf(":?"));
}

TEST_CASE("Synth Bite is built in, with its own settings for the EQ and the Channel EQ", "[presets][synthbite]")
{
    const auto preset = synthBite();
    CHECK(preset.builtIn);
    CHECK(preset.description.isNotEmpty());
    REQUIRE(preset.devices.size() == 2);
    CHECK(preset.fits(devices::Equaliser::kTypeId));
    CHECK(preset.fits(devices::ChannelEq::kTypeId));
    CHECK_FALSE(preset.fits(devices::Reverb::kTypeId));
    CHECK(preset.getSourceId() == "builtin:Synth Bite");
    CHECK(presets::findBySourceId("builtin:Synth Bite").has_value());

    // Each EQ loads its own entry: every setting is defined (nothing left over from before).
    devices::Equaliser eq;
    setValue(eq, "band2Shape", 4.0f); // (would survive a partial preset)
    setValue(eq, "band4Q", 5.0f);
    presets::restore(*preset.findFor(devices::Equaliser::kTypeId), eq);
    CHECK_THAT(valueOf(eq, "band1Shape"), WithinAbs(0.0, 1e-6)); // low cut
    CHECK_THAT(valueOf(eq, "band2Shape"), WithinAbs(2.0, 1e-6)); // peak
    CHECK_THAT(valueOf(eq, "band2Gain"), WithinAbs(-3.0, 1e-4));
    CHECK_THAT(valueOf(eq, "band3Freq"), WithinAbs(3000.0, 0.5));
    CHECK_THAT(valueOf(eq, "band3Gain"), WithinAbs(6.0, 1e-4));
    CHECK_THAT(valueOf(eq, "band4Shape"), WithinAbs(3.0, 1e-6)); // high shelf
    CHECK_THAT(valueOf(eq, "band4Q"), WithinAbs(0.71, 1e-3));
    CHECK_THAT(valueOf(eq, "output"), WithinAbs(-1.5, 1e-4));

    devices::ChannelEq channelEq;
    presets::restore(*preset.findFor(devices::ChannelEq::kTypeId), channelEq);
    CHECK_THAT(valueOf(channelEq, "lowcut"), WithinAbs(1.0, 1e-6));
    CHECK_THAT(valueOf(channelEq, "midfreq"), WithinAbs(3000.0, 0.5));
    CHECK_THAT(valueOf(channelEq, "mid"), WithinAbs(6.0, 1e-4));
    CHECK_THAT(valueOf(channelEq, "high"), WithinAbs(5.5, 1e-4));

    // The same effect on either device: rumble gone, low mids down, presence and top up.
    for (auto* processor : std::initializer_list<juce::AudioProcessor*>{&eq, &channelEq})
    {
        INFO(processor->getName().toStdString());
        const double rumble = gainDbAt(*processor, 35.0);
        const double mud = gainDbAt(*processor, 350.0);
        const double presence = gainDbAt(*processor, 3000.0);
        const double air = gainDbAt(*processor, 10000.0);
        CHECK(rumble < -12.0);
        CHECK(mud < -1.0);
        CHECK(presence > 4.0);
        CHECK(air > 2.0);
        CHECK(presence > mud + 5.5);
    }
    for (const double f : {80.0, 1500.0, 3000.0, 5000.0, 10000.0, 14000.0})
    {
        INFO(f);
        CHECK(std::abs(gainDbAt(eq, f) - gainDbAt(channelEq, f)) < 1.5);
    }
}

TEST_CASE("Loading a preset into a device is one undo step", "[presets]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::audio, "Lead");
    auto eqDevice = session.addDevice(track, devices::Equaliser::kTypeId, "EQ");
    auto reverbDevice = session.addDevice(track, devices::Reverb::kTypeId, "Reverb");
    auto node = project.getDevices().getNode(eqDevice);
    REQUIRE(node != nullptr);
    auto& eq = node->getProcessor();
    setValue(eq, "band3Gain", -2.0f);

    const auto preset = synthBite();
    const auto outcome = ui::presetActions::load(project, preset, eqDevice);
    CHECK(outcome.ok);
    CHECK(outcome.message == "Loaded \"Synth Bite\"");
    CHECK_THAT(valueOf(eq, "band3Gain"), WithinAbs(6.0, 1e-4));
    CHECK(project.hasUnsavedChanges());

    session.getUndoManager().undo();
    CHECK_THAT(valueOf(eq, "band3Gain"), WithinAbs(-2.0, 1e-4));
    session.getUndoManager().redo();
    CHECK_THAT(valueOf(eq, "band3Gain"), WithinAbs(6.0, 1e-4));

    // A preset with nothing for a device changes nothing and says what it is for.
    const auto refused = ui::presetActions::load(project, preset, reverbDevice);
    CHECK_FALSE(refused.ok);
    CHECK(refused.message == "\"Synth Bite\" has no settings for Reverb (it is for EQ, Channel EQ).");
}

TEST_CASE("Saving presets: on their own, added to a file, or the whole chain", "[presets]")
{
    juce::MessageManager::getInstance();
    UserFolder user;
    auto project = makeProject();
    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::audio, "Keys");
    auto eqDevice = session.addDevice(track, devices::Equaliser::kTypeId, "EQ");
    auto channelDevice = session.addDevice(track, devices::ChannelEq::kTypeId, "Channel EQ");
    auto reverbDevice = session.addDevice(track, devices::Reverb::kTypeId, "Reverb");
    auto& eq = project.getDevices().getNode(eqDevice)->getProcessor();
    auto& channelEq = project.getDevices().getNode(channelDevice)->getProcessor();
    setValue(eq, "band2Freq", 640.0f);
    setValue(channelEq, "high", -4.0f);

    // On its own.
    const auto file = user.folder().getChildFile("Warm Keys.dojopreset");
    auto outcome = ui::presetActions::save(project, eqDevice, file);
    CHECK(outcome.ok);
    CHECK(outcome.message == "Saved preset \"Warm Keys\"");
    auto listed = presets::getUserPresets();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].name == "Warm Keys");
    CHECK(listed[0].getDeviceNames() == juce::StringArray{"EQ"});
    CHECK(listed[0].file == file);

    // The Channel EQ added to the same file: one file, two devices.
    outcome = ui::presetActions::addToFile(project, channelDevice, file);
    CHECK(outcome.ok);
    CHECK(outcome.message == "Added Channel EQ to \"Warm Keys\" (EQ, Channel EQ)");
    juce::String error;
    auto both = presets::Preset::load(file, error);
    REQUIRE(both.has_value());
    CHECK(both->getDeviceNames() == juce::StringArray{"EQ", "Channel EQ"});
    // ...and again: replaced, not doubled.
    setValue(channelEq, "high", -6.0f);
    outcome = ui::presetActions::addToFile(project, channelDevice, file);
    CHECK(outcome.message == "Replaced the Channel EQ settings in \"Warm Keys\"");
    both = presets::Preset::load(file, error);
    REQUIRE(both.has_value());
    CHECK(both->devices.size() == 2);

    // What fits which device: built-in first, then the user's.
    const auto forEq = presets::getFor(devices::Equaliser::kTypeId);
    REQUIRE(forEq.size() == 11); // (Synth Bite, every built-in chain has an EQ, and the user's)
    CHECK(std::any_of(forEq.begin(), forEq.end(), [](const auto& p) { return p.name == "Synth Bite"; }));
    CHECK(forEq.back().name == "Warm Keys");
    CHECK(presets::getFor("builtin:autopan").empty());

    // Loading the file into the other devices: each takes its own settings; the Reverb is not in it.
    setValue(eq, "band2Freq", 100.0f);
    setValue(channelEq, "high", 0.0f);
    CHECK(ui::presetActions::loadFirstFitting(project, {file.getFullPathName()}, eqDevice).ok);
    CHECK(ui::presetActions::loadFirstFitting(project, {file.getFullPathName()}, channelDevice).ok);
    CHECK_THAT(valueOf(eq, "band2Freq"), WithinAbs(640.0, 0.5));
    CHECK_THAT(valueOf(channelEq, "high"), WithinAbs(-6.0, 1e-4));
    outcome = ui::presetActions::loadFirstFitting(project, {file.getFullPathName()}, reverbDevice);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.message.contains("has no settings for Reverb"));
    CHECK_FALSE(ui::presetActions::loadFirstFitting(project, {"/nowhere/sound.wav"}, eqDevice).ok);

    // The whole chain in one file.
    const auto chainFile = user.folder().getChildFile("Keys Chain.dojopreset");
    outcome = ui::presetActions::saveChain(project, track, chainFile);
    CHECK(outcome.ok);
    auto chain = presets::Preset::load(chainFile, error);
    REQUIRE(chain.has_value());
    CHECK(chain->chain);
    CHECK(chain->getDeviceNames() == juce::StringArray{"EQ", "Channel EQ", "Reverb"});
}

TEST_CASE("Presets dropped on a device card load into it; dropped on the panel they add a device", "[presets][ui]")
{
    juce::MessageManager::getInstance();
    UserFolder user;
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Lead");
    auto eqDevice = session.addDevice(track, devices::Equaliser::kTypeId, "EQ");
    auto channelDevice = session.addDevice(track, devices::ChannelEq::kTypeId, "Channel EQ");
    auto reverbDevice = session.addDevice(track, devices::Reverb::kTypeId, "Reverb");
    auto autoPanDevice = session.addDevice(track, "builtin:autopan", "Auto Pan");
    editorState.selectTrack(session::track::getId(track));

    ui::DevicePanel panel(project, plugins, editorState);
    panel.setSize(1600, 300);
    auto* eqCard = panel.findCard(eqDevice);
    auto* channelCard = panel.findCard(channelDevice);
    auto* reverbCard = panel.findCard(reverbDevice);
    REQUIRE(eqCard != nullptr);
    REQUIRE(channelCard != nullptr);
    REQUIRE(reverbCard != nullptr);
    auto& eq = project.getDevices().getNode(eqDevice)->getProcessor();
    auto& channelEq = project.getDevices().getNode(channelDevice)->getProcessor();

    // The presets menu lists what fits, then the save/load actions.
    const auto items = panel.getPresetMenuItems(eqDevice);
    CHECK(items.contains("[Built-in Presets]"));
    CHECK(items.contains("Synth Bite"));
    for (const auto* action : {"Save Preset...", "Add to Preset File...", "Save Whole Chain as Preset...", "Load Preset File...", "Open Presets Folder"})
        CHECK(items.contains(action));
    CHECK(panel.getPresetMenuItems(reverbDevice).contains("Dreamy Wide Space  (from chain)"));
    CHECK(panel.getPresetMenuItems(autoPanDevice).contains("No presets for Auto Pan yet"));

    // From the file manager: a .dojopreset file dropped on the card.
    const auto file = user.folder().getChildFile("Synth Bite.dojopreset");
    REQUIRE(synthBite().save(file).wasOk());
    auto* fileTarget = dynamic_cast<juce::FileDragAndDropTarget*>(eqCard);
    REQUIRE(fileTarget != nullptr);
    CHECK(fileTarget->isInterestedInFileDrag({file.getFullPathName()}));
    CHECK_FALSE(fileTarget->isInterestedInFileDrag({temp.dir.getChildFile("kick.wav").getFullPathName()}));
    fileTarget->fileDragEnter({file.getFullPathName()}, 10, 10);
    (void) eqCard->createComponentSnapshot(eqCard->getLocalBounds()); // (paints the drop outline)
    fileTarget->filesDropped({file.getFullPathName()}, 10, 10);
    CHECK_THAT(valueOf(eq, "band3Gain"), WithinAbs(6.0, 1e-4));
    CHECK(panel.getNotice(eqDevice) == "Loaded \"Synth Bite\"");
    (void) eqCard->createComponentSnapshot(eqCard->getLocalBounds());

    // From the browser: the same preset dragged onto the Channel EQ takes the Channel EQ settings.
    auto* dragTarget = dynamic_cast<juce::DragAndDropTarget*>(channelCard);
    REQUIRE(dragTarget != nullptr);
    const juce::DragAndDropTarget::SourceDetails details(ui::presetActions::toDragDescription(synthBite()), nullptr, {10, 10});
    CHECK(dragTarget->isInterestedInDragSource(details));
    dragTarget->itemDragEnter(details);
    dragTarget->itemDropped(details);
    CHECK_THAT(valueOf(channelEq, "mid"), WithinAbs(6.0, 1e-4));
    CHECK_THAT(valueOf(channelEq, "midfreq"), WithinAbs(3000.0, 0.5));

    // Onto a device it has nothing for: nothing changes, the card says why.
    auto* reverbTarget = dynamic_cast<juce::DragAndDropTarget*>(reverbCard);
    REQUIRE(reverbTarget != nullptr);
    reverbTarget->itemDropped(details);
    CHECK(panel.getNotice(reverbDevice).contains("has no settings for Reverb"));
    (void) reverbCard->createComponentSnapshot(reverbCard->getLocalBounds());

    // Onto the panel itself: a new device that starts from the preset (one undo step).
    const auto before = session::Session::getDevices(track).size();
    REQUIRE(panel.isInterestedInDragSource(details));
    panel.itemDropped(details);
    const auto chain = session::Session::getDevices(track);
    REQUIRE(chain.size() == before + 1);
    const auto added = chain.back();
    CHECK(session::device::getType(added) == devices::Equaliser::kTypeId);
    CHECK_THAT(valueOf(project.getDevices().getNode(added)->getProcessor(), "band3Gain"), WithinAbs(6.0, 1e-4));
    CHECK(panel.getPanelNotice() == "Added EQ with \"Synth Bite\"");
    session.getUndoManager().undo();
    CHECK(session::Session::getDevices(track).size() == before);
    session.getUndoManager().redo();
    REQUIRE(session::Session::getDevices(track).size() == before + 1);
    CHECK_THAT(valueOf(project.getDevices().getNode(session::Session::getDevices(track).back())->getProcessor(), "band3Gain"),
               WithinAbs(6.0, 1e-4));

    // A preset file from the file manager dropped on the panel does the same.
    CHECK(panel.isInterestedInFileDrag({file.getFullPathName()}));
    CHECK_FALSE(panel.isInterestedInFileDrag({temp.dir.getChildFile("kick.wav").getFullPathName()}));
    panel.filesDropped({file.getFullPathName()}, 10, 10);
    CHECK(session::Session::getDevices(track).size() == before + 2);
}

TEST_CASE("Using a preset on a track loads it into a device it fits, or adds one", "[presets]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Pad");
    session.addDevice(track, devices::Reverb::kTypeId, "Reverb");
    auto channelDevice = session.addDevice(track, devices::ChannelEq::kTypeId, "Channel EQ");

    // The Channel EQ on the track takes it.
    auto outcome = ui::presetActions::useOn(project, plugins, editorState, track, synthBite());
    CHECK(outcome.ok);
    CHECK(session::Session::getDevices(track).size() == 2);
    CHECK_THAT(valueOf(project.getDevices().getNode(channelDevice)->getProcessor(), "mid"), WithinAbs(6.0, 1e-4));

    // A track with nothing it fits gets an EQ (its first device) with the preset.
    auto other = session.addTrack(session::TrackKind::audio, "Bass");
    editorState.selectTrack(session::track::getId(other));
    outcome = ui::presetActions::useOnSelection(project, plugins, editorState, synthBite());
    CHECK(outcome.ok);
    const auto devicesOnOther = session::Session::getDevices(other);
    REQUIRE(devicesOnOther.size() == 1);
    CHECK(session::device::getType(devicesOnOther[0]) == devices::Equaliser::kTypeId);

    // A preset only for a plug-in that is not installed adds nothing.
    presets::Preset foreign;
    foreign.name = "Foreign";
    presets::DeviceSettings entry;
    entry.key = "plugin:VST3:Nobody:Missing";
    entry.deviceName = "Missing";
    entry.state.append("x", 1);
    foreign.put(entry);
    outcome = ui::presetActions::useOn(project, plugins, editorState, other, foreign);
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.message.contains("Missing"));
}

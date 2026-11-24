#include "devices/DrumKits.h"
#include "devices/Instruments.h"
#include "engine/AudioEngine.h"
#include "session/DrumKitDemo.h"
#include "session/Presets.h"
#include "session/Project.h"
#include "devices/DeviceRegistry.h"
#include "ui/EditorState.h"
#include "ui/PresetActions.h"
#include "ui/components/BrowserPanel.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <map>
#include <set>

using namespace dojo;
using Catch::Matchers::WithinAbs;
namespace presets = session::presets;
namespace kits = devices::kits;

namespace
{
/** The kit library in a temporary folder for the test. */
struct Library
{
    Library() { kits::setLibraryFolder(folder()); }
    ~Library() { kits::setLibraryFolder({}); }
    [[nodiscard]] juce::File folder() const { return temp.dir.getChildFile("Drum Machines"); }
    test::TempFolder temp;
};

std::vector<presets::Preset> builtInKits()
{
    std::vector<presets::Preset> found;
    for (auto& preset : presets::getBuiltIn())
        if (preset.isDrumKit())
            found.push_back(preset);
    return found;
}

presets::Preset builtInKit(const juce::String& name)
{
    for (auto& kit : builtInKits())
        if (kit.name == name)
            return kit;
    FAIL("No built-in kit " << name);
    return {};
}

/** The Drum Rack state of a kit as XML (its DOJO_DEVICE). */
std::unique_ptr<juce::XmlElement> stateOf(const presets::Preset& kit)
{
    const auto* settings = kit.findFor(devices::DrumRack::kTypeId);
    REQUIRE(settings != nullptr);
    auto xml = juce::AudioProcessor::getXmlFromBinary(settings->state.getData(), static_cast<int>(settings->state.getSize()));
    REQUIRE(xml != nullptr);
    return xml;
}

float parameter(const juce::XmlElement& state, const juce::String& id)
{
    for (const auto* p : state.getChildWithTagNameIterator("PARAM"))
        if (p->getStringAttribute("id") == id)
            return static_cast<float>(p->getDoubleAttribute("value"));
    FAIL("No parameter " << id);
    return 0.0f;
}

double rms(const juce::AudioBuffer<float>& audio, double from, double to, double rate)
{
    const auto a = std::max(0, static_cast<int>(from * rate));
    const auto b = std::min(audio.getNumSamples(), static_cast<int>(to * rate));
    return b > a ? static_cast<double>(audio.getRMSLevel(0, a, b - a)) : 0.0;
}

/** The first sample at or after `from` (seconds) louder than `threshold`, in seconds (-1: none). */
double onsetAfter(const juce::AudioBuffer<float>& audio, double from, double rate, float threshold = 1.0e-3f)
{
    for (auto i = static_cast<int>(from * rate); i < audio.getNumSamples(); ++i)
        if (std::abs(audio.getSample(0, i)) > threshold || std::abs(audio.getSample(1, i)) > threshold)
            return static_cast<double>(i) / rate;
    return -1.0;
}
} // namespace

TEST_CASE("Five drum machines are built in, and every one of their samples is on a kit's pad", "[kits]")
{
    // The machines and their samples (the bytes as they came, renamed readably).
    const auto machines = kits::getMachines();
    CHECK(machines == juce::StringArray{"AdrenaLinn", "Linn 9000", "Linn LM-1", "Linn LM-2", "LinnDrum"});
    const std::map<juce::String, int> counts{{"AdrenaLinn", 40}, {"Linn 9000", 79}, {"Linn LM-1", 19}, {"Linn LM-2", 23}, {"LinnDrum", 30}};
    int total = 0;
    for (const auto& machine : machines)
    {
        const auto names = kits::getSampleNames(machine);
        CHECK(names.size() == counts.at(machine));
        total += names.size();
        for (const auto& name : names)
            CHECK(name.endsWith(".wav"));
    }
    CHECK(total == 191);
    CHECK(kits::getSampleNames("Linn LM-1").contains("Kick 1.wav"));
    CHECK(kits::getSampleNames("Nothing").isEmpty());

    // Sixteen kits, each a Drum Rack preset with all sixteen pads arranged from one machine.
    const auto all = builtInKits();
    REQUIRE(all.size() == 16);
    std::map<juce::String, std::set<juce::String>> used;
    for (const auto& kit : all)
    {
        INFO(kit.name.toStdString());
        CHECK(kit.kit);
        CHECK(kit.builtIn);
        CHECK(kit.category == "Drums");
        CHECK(machines.contains(kit.machine));
        CHECK(kit.description.isNotEmpty());
        CHECK_FALSE(kit.demo.isEmpty());
        CHECK(kit.demo.tempo >= 60.0);
        const auto state = stateOf(kit);
        const auto* extra = state->getChildByName("EXTRA");
        REQUIRE(extra != nullptr);
        std::set<int> pads;
        for (const auto* pad : extra->getChildWithTagNameIterator("PAD"))
        {
            const auto reference = pad->getStringAttribute("sample");
            REQUIRE(reference.startsWith("kit:" + kit.machine + "/"));
            const auto name = reference.fromFirstOccurrenceOf("/", false, false);
            CHECK(kits::getSampleNames(kit.machine).contains(name));
            used[kit.machine].insert(name);
            pads.insert(pad->getIntAttribute("index"));
            // Hats on the hat pads choke each other.
            const int index = pad->getIntAttribute("index");
            if ((index == 6 || index == 8 || index == 10) && name.startsWith("Hat"))
                CHECK(parameter(*state, "pad" + juce::String(index + 1) + "Choke") == 1.0f);
        }
        CHECK(pads.size() == 16);
        // The demo plays pads that have samples (all of them do), and the kick pad at least.
        bool kick = false;
        for (const auto& lane : kit.demo.lanes)
        {
            CHECK(lane.pad >= 0);
            CHECK(lane.pad < 16);
            CHECK(lane.steps.length() == kit.demo.bars * 16);
            kick = kick || lane.pad == 0;
        }
        CHECK(kick);
    }
    for (const auto& machine : machines)
        CHECK(static_cast<int>(used[machine].size()) == counts.at(machine)); // (every sample on a pad somewhere)

    // The kits round-trip through a preset file as they are.
    juce::String error;
    const auto lm1 = builtInKit("LM-1 Kit");
    const auto again = presets::Preset::fromXml(*lm1.toXml(), error);
    REQUIRE(again.has_value());
    CHECK(again->kit);
    CHECK(again->machine == "Linn LM-1");
    CHECK(again->demo.lanes.size() == lm1.demo.lanes.size());
    CHECK(again->demo.lanes[0].steps == lm1.demo.lanes[0].steps);
    CHECK_THAT(again->demo.tempo, WithinAbs(lm1.demo.tempo, 1e-9));
    CHECK(again->isDrumKit());

    // Any Drum Rack preset counts as a kit (the user's own); other presets do not.
    presets::Preset own;
    own.name = "My Kit";
    own.devices.push_back(lm1.devices.front());
    CHECK(own.isDrumKit());
    CHECK(own.demo.isEmpty());
    own.category = "Mixing";
    CHECK_FALSE(own.isDrumKit());
    for (const auto& preset : presets::getBuiltIn())
        if (!preset.kit)
            CHECK_FALSE(preset.isDrumKit());
}

TEST_CASE("Kit samples are unpacked when first needed and saved by reference", "[kits]")
{
    juce::MessageManager::getInstance();
    Library library;
    CHECK_FALSE(library.folder().exists());

    // A reference unpacks its machine's samples (all of them), once.
    const auto kick = kits::fileFor("kit:Linn LM-1/Kick 1.wav");
    REQUIRE(kick.existsAsFile());
    CHECK(kick == library.folder().getChildFile("Linn LM-1/Kick 1.wav"));
    CHECK(library.folder().getChildFile("Linn LM-1").findChildFiles(juce::File::findFiles, false, "*.wav").size() == 19);
    CHECK_FALSE(library.folder().getChildFile("Linn 9000").exists());
    const auto size = kick.getSize();
    CHECK(size > 1000);

    // A damaged sample is put back by unpacking again.
    REQUIRE(kick.replaceWithText("broken"));
    juce::String error;
    REQUIRE(kits::unpack("Linn LM-1", error));
    CHECK(kick.getSize() == size);
    CHECK_FALSE(kits::unpack("No Such Machine", error));
    CHECK(error.isNotEmpty());

    // Back to references; anything else stays a path. Unknown or unsafe references give nothing.
    CHECK(kits::referenceFor(kick) == "kit:Linn LM-1/Kick 1.wav");
    const auto elsewhere = library.temp.dir.getChildFile("kick.wav");
    CHECK(kits::referenceFor(elsewhere) == elsewhere.getFullPathName());
    CHECK(kits::fileFor(elsewhere.getFullPathName()) == elsewhere);
    CHECK(kits::fileFor("kit:No Such Machine/Kick.wav") == juce::File());
    CHECK(kits::fileFor("kit:Linn LM-1/../../secret.wav") == juce::File());
    CHECK(kits::fileFor("kit:Linn LM-1/Not There.wav") == juce::File());
    CHECK(kits::fileFor("") == juce::File());

    // A Drum Rack loading a kit: every pad's sample from the library, the levels and chokes too...
    const auto kit = builtInKit("LM-2 Kit");
    devices::DrumRack rack;
    presets::restore(*kit.findFor(devices::DrumRack::kTypeId), rack);
    for (int pad = 0; pad < devices::DrumRack::kPads; ++pad)
    {
        const auto sample = rack.getPadSample(pad);
        REQUIRE(sample != nullptr);
        CHECK(sample->file.isAChildOf(library.folder().getChildFile("Linn LM-2")));
        CHECK(sample->getNumFrames() > 0);
    }
    CHECK(rack.getPadSample(0)->file.getFileName() == "Kick Hard.wav");
    CHECK(rack.getPad(6).choke->getIndex() == 1);
    CHECK(rack.getPad(10).choke->getIndex() == 1);
    CHECK(rack.getPad(0).choke->getIndex() == 0);
    CHECK_THAT(rack.getPad(13).volume->get(), WithinAbs(-6.0, 1e-4)); // (the crash, under the drums)

    // ...saved by reference, not by this computer's path, and opened elsewhere it unpacks there.
    juce::MemoryBlock state;
    rack.getStateInformation(state);
    const auto xml = juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()));
    REQUIRE(xml != nullptr);
    const auto text = xml->toString();
    CHECK(text.contains("kit:Linn LM-2/Kick Hard.wav"));
    CHECK_FALSE(text.contains(library.folder().getFullPathName()));
    test::TempFolder elsewhereLibrary;
    kits::setLibraryFolder(elsewhereLibrary.dir);
    devices::DrumRack copy;
    copy.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    REQUIRE(copy.getPadSample(2) != nullptr);
    CHECK(copy.getPadSample(2)->file == elsewhereLibrary.dir.getChildFile("Linn LM-2/Snare Hard.wav"));
    kits::setLibraryFolder(library.folder());

    // Impulse and the Sampler save kit samples by reference too.
    auto impulse = devices::createBuiltinDevice("builtin:impulse");
    REQUIRE(impulse != nullptr);
    {
        juce::XmlElement device("DOJO_DEVICE");
        device.setAttribute("type", "builtin:impulse");
        auto* extra = device.createNewChildElement("EXTRA");
        auto* slot = extra->createNewChildElement("SLOT");
        slot->setAttribute("index", 0);
        slot->setAttribute("sample", "kit:LinnDrum/Clap.wav");
        juce::MemoryBlock block;
        juce::AudioProcessor::copyXmlToBinary(device, block);
        impulse->setStateInformation(block.getData(), static_cast<int>(block.getSize()));
        juce::MemoryBlock saved;
        impulse->getStateInformation(saved);
        CHECK(juce::AudioProcessor::getXmlFromBinary(saved.getData(), static_cast<int>(saved.getSize()))->toString().contains("kit:LinnDrum/Clap.wav"));
    }
}

TEST_CASE("A kit's demo beat: its hits at its tempo and swing, played by a Drum Rack with the kit", "[kits]")
{
    juce::MessageManager::getInstance();
    Library library;
    constexpr double kRate = 48'000.0;

    // The built-in kits' beats: their bars at their tempo, then the ring-out; loud but not clipping.
    for (const auto& kit : builtInKits())
    {
        INFO(kit.name.toStdString());
        const auto audio = presets::renderDemo(kit, kRate);
        REQUIRE(audio.getNumChannels() == 2);
        CHECK(audio.getNumSamples() == static_cast<int>(std::ceil((presets::demoSeconds(kit.demo) + 1.2) * kRate)));
        const float peak = audio.getMagnitude(0, audio.getNumSamples());
        CHECK(peak > 0.25f);
        CHECK(peak <= 0.8915f);
        CHECK(rms(audio, 0.0, presets::demoSeconds(kit.demo), kRate) > 0.01);
        CHECK(audio.getMagnitude(0, audio.getNumSamples() - 32, 32) < 1.0e-3f); // (faded out)
    }

    // Timing: one lane, the kick on steps 0 and 8 at 120 BPM (a sixteenth is 0.125 s).
    auto kit = builtInKit("LM-1 Kit");
    kit.demo.tempo = 120.0;
    kit.demo.bars = 1;
    kit.demo.swing = 0.0;
    kit.demo.lanes = {{0, "X.......X......."}};
    auto audio = presets::renderDemo(kit, kRate);
    CHECK_THAT(onsetAfter(audio, 0.0, kRate), WithinAbs(0.0, 0.002));
    CHECK(rms(audio, 0.6, 0.99, kRate) < 1.0e-4); // (the first kick has died away)
    CHECK_THAT(onsetAfter(audio, 0.6, kRate), WithinAbs(1.0, 0.002));

    // Swing plays every second sixteenth late: step 9 at (9 + 0.5) x 0.125 s, step 8 on time.
    kit.demo.swing = 0.5;
    kit.demo.lanes = {{2, ".........x......"}};
    audio = presets::renderDemo(kit, kRate);
    CHECK_THAT(onsetAfter(audio, 0.0, kRate), WithinAbs(1.1875, 0.002));
    kit.demo.lanes = {{2, "........x......."}};
    audio = presets::renderDemo(kit, kRate);
    CHECK_THAT(onsetAfter(audio, 0.0, kRate), WithinAbs(1.0, 0.002));

    // Softer steps play softer (the same kick: X, then o: velocity 64, which a Drum Rack plays at
    // 0.2 + 0.8 x 64/127 of full level).
    kit.demo.swing = 0.0;
    kit.demo.lanes = {{0, "X.......o......."}};
    audio = presets::renderDemo(kit, kRate);
    const double loud = static_cast<double>(audio.getMagnitude(0, 0, static_cast<int>(0.2 * kRate)));
    const double soft = static_cast<double>(audio.getMagnitude(0, static_cast<int>(1.0 * kRate), static_cast<int>(0.2 * kRate)));
    CHECK_THAT(soft / loud, WithinAbs(0.2 + 0.8 * 64.0 / 127.0, 0.01));

    // A kit with no beat of its own plays the standard one; something that is not a kit plays nothing.
    kit.demo = {};
    audio = presets::renderDemo(kit, kRate);
    CHECK(audio.getNumSamples() == static_cast<int>(std::ceil((presets::demoSeconds(presets::DemoBeat::standard()) + 1.2) * kRate)));
    CHECK(rms(audio, 0.0, 2.0, kRate) > 0.01);
    for (const auto& preset : presets::getBuiltIn())
        if (preset.name == "Synth Bite")
            CHECK(presets::renderDemo(preset, kRate).getNumSamples() == 0);
}

TEST_CASE("The browser lists the drum kits; selecting one plays its demo beat when the headphones are on", "[kits][browser][ui]")
{
    juce::MessageManager::getInstance();
    Library library;
    test::TempFolder temp;
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    engine::AudioEngine engine{0};
    engine.prepare(48'000.0, 256, 2);
    juce::PropertiesFile::Options options;
    options.applicationName = "settings";
    options.filenameSuffix = ".xml";
    options.storageFormat = juce::PropertiesFile::storeAsXML;
    juce::PropertiesFile settings(temp.dir.getChildFile("settings.xml"), options);
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    {
        engine::PreviewService preview(engine, formats);
        ui::BrowserPanel browser(formats.getWildcardForAllFormats(), plugins, preview, settings);
        browser.setSize(260, 900);

        // DRUM KITS: the sixteen kits (not among the other presets).
        const auto listed = browser.getListedKitNames();
        CHECK(listed.size() == 16);
        CHECK(listed.contains("LM-1 Kit"));
        CHECK(listed.contains("Linn 9000 Selector Kit"));
        CHECK_FALSE(browser.getListedPresetNames().isEmpty());
        browser.setDeviceSearch("9000");
        CHECK(browser.getListedKitNames().size() == 6);
        browser.setDeviceSearch("lm-2 soft");
        CHECK(browser.getListedKitNames() == juce::StringArray{"LM-2 Soft Kit"});
        browser.setDeviceSearch("");

        // Selected with the headphones on: its demo beat goes to the preview player (which the audio
        // device plays; none runs here), named after it.
        browser.setPreviewEnabled(true);
        REQUIRE(browser.selectListedPreset("LinnDrum Kit"));
        CHECK(preview.getCurrentName() == "LinnDrum Kit (demo beat)");
        CHECK(preview.getCurrentFile() == juce::File());
        const auto sound = engine.getPreviewPlayer().getCurrent();
        REQUIRE(sound != nullptr);
        CHECK(sound->totalFrames == static_cast<int>(std::ceil((presets::demoSeconds(builtInKit("LinnDrum Kit").demo) + 1.2) * 48'000.0)));
        CHECK(sound->framesReady.load() == sound->totalFrames);
        CHECK(sound->audio.getMagnitude(0, sound->totalFrames) > 0.25f);
        CHECK(library.folder().getChildFile("LinnDrum/Kick.wav").existsAsFile()); // (unpacked to play it)

        // Another kit replaces it; with the headphones off, selecting one plays nothing.
        REQUIRE(browser.selectListedPreset("AdrenaLinn Kit"));
        CHECK(preview.getCurrentName() == "AdrenaLinn Kit (demo beat)");
        const auto adrenaLinn = engine.getPreviewPlayer().getCurrent();
        CHECK(adrenaLinn != sound);
        browser.setPreviewEnabled(false);
        REQUIRE(browser.selectListedPreset("LM-1 Kit"));
        CHECK(preview.getCurrentName() == "AdrenaLinn Kit (demo beat)");
        CHECK(engine.getPreviewPlayer().getCurrent() == adrenaLinn);

        // Rendered audio at another rate is resampled to the engine's.
        juce::AudioBuffer<float> tone(1, 44'100);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            tone.setSample(0, i, 0.5f * static_cast<float>(std::sin(2.0 * test::kPi * 441.0 * i / 44'100.0)));
        REQUIRE(preview.preview("Tone", tone, 44'100.0));
        const auto resampled = engine.getPreviewPlayer().getCurrent();
        REQUIRE(resampled != nullptr);
        CHECK(resampled->totalFrames == 48'000);
        CHECK(resampled->audio.getMagnitude(1, 0, 40'000) > 0.45f); // (mono to both channels)
        preview.stop();
    }
    engine.release();
}

TEST_CASE("Using a kit on a track gives it a Drum Rack with the kit's pads", "[kits][presets]")
{
    juce::MessageManager::getInstance();
    Library library;
    test::TempFolder temp;
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::instrument, "Drums");

    auto outcome = ui::presetActions::useOn(project, plugins, editorState, track, builtInKit("LM-1 Kit"));
    CHECK(outcome.ok);
    const auto added = session::Session::getDevices(track);
    REQUIRE(added.size() == 1);
    CHECK(session::device::getType(added[0]) == devices::DrumRack::kTypeId);
    CHECK(session::device::getName(added[0]) == "LM-1 Kit"); // (named after the kit)
    CHECK(session::track::getName(track) == "Drums");      // (the user's track keeps its name)
    auto* rack = dynamic_cast<devices::DrumRack*>(&project.getDevices().getNode(added[0])->getProcessor());
    REQUIRE(rack != nullptr);
    REQUIRE(rack->getPadSample(0) != nullptr);
    CHECK(rack->getPadSample(0)->file.getFileName() == "Kick 1.wav");
    CHECK(rack->getPadSample(15)->file.getFileName() == "Ride.wav");

    // Another kit on the same track loads into that Drum Rack (one undo step back).
    outcome = ui::presetActions::useOn(project, plugins, editorState, track, builtInKit("Linn 9000 Kit"));
    CHECK(outcome.ok);
    CHECK(session::Session::getDevices(track).size() == 1);
    CHECK(rack->getPadSample(0)->file == library.folder().getChildFile("Linn 9000/Kick 1.wav"));
    session.getUndoManager().undo();
    CHECK(rack->getPadSample(0)->file == library.folder().getChildFile("Linn LM-1/Kick 1.wav"));

    // With no track selected, a new instrument track is made for it, named after the kit too.
    outcome = ui::presetActions::useOn(project, plugins, editorState, juce::ValueTree(), builtInKit("AdrenaLinn Sub Kit"));
    CHECK(outcome.ok);
    juce::ValueTree newTrack;
    for (int i = 0; i < session.getNumTracks(); ++i)
        if (session.getTrack(i) != track)
            newTrack = session.getTrack(i);
    REQUIRE(newTrack.isValid());
    CHECK(session::track::getName(newTrack) == "AdrenaLinn Sub Kit");
    REQUIRE(session::Session::getDevices(newTrack).size() == 1);
    CHECK(session::device::getName(session::Session::getDevices(newTrack)[0]) == "AdrenaLinn Sub Kit");
}

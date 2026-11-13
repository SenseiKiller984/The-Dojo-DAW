#include "devices/DeviceRegistry.h"
#include "devices/Equaliser.h"
#include "session/Presets.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/DemoProject.h"
#include "ui/PresetActions.h"
#include "ui/components/DevicePanel.h"
#include "EffectTestHelpers.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <map>

using namespace dojo;
using Catch::Matchers::WithinAbs;
namespace presets = session::presets;
using test::devices::kRate;
using test::devices::Stereo;

namespace
{
session::Project makeProject()
{
    return session::Project([](std::unique_ptr<engine::RenderGraph>) {},
                            [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
}

presets::Preset builtIn(const juce::String& name)
{
    for (auto& preset : presets::getBuiltIn())
        if (preset.name == name)
            return preset;
    FAIL(name.toStdString() + " is not built in");
    return {};
}

float valueOf(juce::AudioProcessor& processor, const juce::String& id)
{
    auto* parameter = dynamic_cast<juce::RangedAudioParameter*>(dynamic_cast<devices::BuiltinProcessor&>(processor).getParameterById(id));
    REQUIRE(parameter != nullptr);
    return parameter->convertFrom0to1(parameter->getValue());
}

/** A chain preset's devices, set up as the preset says, run one after the other. */
struct Chain
{
    explicit Chain(const presets::Preset& preset)
    {
        for (const auto& entry : preset.devices)
        {
            auto processor = devices::createBuiltinDevice(entry.key);
            REQUIRE(processor != nullptr);
            presets::restore(entry, *processor);
            auto node = std::make_unique<engine::ProcessorNode>(std::move(processor));
            node->prepare(kRate, test::devices::kBlock, nullptr);
            nodes.push_back(std::move(node));
        }
    }

    Stereo run(const Stereo& input)
    {
        auto signal = input;
        for (auto& node : nodes)
            signal = test::devices::run(*node, signal);
        return signal;
    }

    std::vector<std::unique_ptr<engine::ProcessorNode>> nodes;
};

std::size_t samples(double seconds)
{
    return static_cast<std::size_t>(seconds * kRate);
}

double db(double gain)
{
    return 20.0 * std::log10(std::max(gain, 1e-12));
}

Stereo silence(std::size_t n)
{
    return Stereo(n);
}

/** White noise (repeatable), at an RMS level in dBFS. */
Stereo noise(std::size_t n, double rmsDb, std::uint32_t seed = 1)
{
    Stereo s(n);
    const double amplitude = std::pow(10.0, rmsDb / 20.0) * std::sqrt(3.0);
    std::uint32_t state = seed;
    const auto next = [&state]
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<double>(state) / 4294967295.0 * 2.0 - 1.0;
    };
    for (std::size_t i = 0; i < n; ++i)
    {
        s.left[i] = amplitude * next();
        s.right[i] = amplitude * next();
    }
    return s;
}

/** Steady-state level change (dB) of a tone at `levelDb` dBFS peak through a fresh chain. */
double gainAt(const presets::Preset& preset, double frequency, double levelDb, double seconds = 1.0)
{
    Chain chain(preset);
    const auto n = samples(seconds);
    const auto in = test::devices::tone(frequency, n, std::pow(10.0, levelDb / 20.0));
    const auto out = chain.run(in);
    const auto from = n / 2;
    const double outLevel = 0.5 * (test::devices::amplitudeAt(out.left, frequency, from, n) + test::devices::amplitudeAt(out.right, frequency, from, n));
    return db(outLevel / test::devices::amplitudeAt(in.left, frequency, from, n));
}

/** The strongest component within `spread` Hz of `frequency` (detuned or modulated tones). */
double amplitudeNear(const std::vector<double>& x, double frequency, double spread, std::size_t begin, std::size_t end)
{
    double best = 0.0;
    for (double f = frequency - spread; f <= frequency + spread; f += 1.0)
        best = std::max(best, test::devices::amplitudeAt(x, f, begin, end));
    return best;
}

double rmsDb(const Stereo& s, std::size_t begin, std::size_t end)
{
    return db(0.5 * (test::devices::rmsOf(s.left, begin, end) + test::devices::rmsOf(s.right, begin, end)));
}

double peakOf(const Stereo& s)
{
    return std::max(test::devices::peakOf(s.left), test::devices::peakOf(s.right));
}

bool finite(const Stereo& s)
{
    const auto ok = [](const std::vector<double>& x) { return std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); }); };
    return ok(s.left) && ok(s.right);
}

/** A short 1 kHz burst (10 ms) at -10 dBFS followed by silence. */
Stereo burst(double seconds)
{
    auto s = silence(samples(seconds));
    const auto tone = test::devices::tone(1000.0, samples(0.01), std::pow(10.0, -10.0 / 20.0));
    std::copy(tone.left.begin(), tone.left.end(), s.left.begin());
    std::copy(tone.right.begin(), tone.right.end(), s.right.begin());
    return s;
}

/** The preset without the device playing `role` (to hear what that device does in context). */
presets::Preset without(presets::Preset preset, const juce::String& role)
{
    auto& d = preset.devices;
    d.erase(std::remove_if(d.begin(), d.end(), [&](const auto& e) { return e.deviceName == role; }), d.end());
    return preset;
}

/** The chain up to and including the device playing `role` (before the delays and reverbs). */
presets::Preset upTo(presets::Preset preset, const juce::String& role)
{
    auto& d = preset.devices;
    const auto it = std::find_if(d.begin(), d.end(), [&](const auto& e) { return e.deviceName == role; });
    REQUIRE(it != d.end());
    d.erase(std::next(it), d.end());
    return preset;
}

Stereo mono(Stereo s)
{
    s.right = s.left;
    return s;
}

Stereo leftOnly(Stereo s)
{
    std::fill(s.right.begin(), s.right.end(), 0.0);
    return s;
}

Stereo plus(const Stereo& a, const Stereo& b)
{
    return test::devices::sum(a, b);
}

double toneDb(double levelDb)
{
    return std::pow(10.0, levelDb / 20.0);
}

const juce::StringArray kChains{"Punchy Drum Bus",   "Dusty Lo-Fi Drums", "Pop Vocal Polish",
                                "Upfront Rap Vocal", "Dreamy Wide Space", "Old Radio",
                                "Drivers License Vocal Saturation", "Gritty Bass Weight", "Shimmer Bloom"};
} // namespace


//==================================================================================================
TEST_CASE("Chain presets keep their devices in order, the same kind more than once", "[presets][chains]")
{
    devices::Equaliser subtractive, additive;
    test::devices::setParameter(subtractive, "band2Gain", -4.0f);
    test::devices::setParameter(additive, "band4Gain", 6.0f);
    juce::ValueTree eqTree(session::ids::device);
    eqTree.setProperty(session::ids::type, devices::Equaliser::kTypeId, nullptr);

    presets::Preset chain;
    chain.name = "Two EQs";
    chain.category = "Vocals";
    chain.chain = true;
    auto first = presets::capture(eqTree, subtractive);
    first.deviceName = "Subtractive EQ";
    auto second = presets::capture(eqTree, additive);
    second.deviceName = "Air EQ";
    chain.add(first);
    presets::DeviceSettings gate;
    gate.key = "builtin:gate";
    gate.deviceName = "Gate";
    gate.state.append("x", 1);
    chain.add(gate);
    chain.add(second);
    CHECK(chain.devices.size() == 3);

    juce::String error;
    const auto parsed = presets::Preset::parse(chain.toXml()->toString(), error);
    REQUIRE(parsed.has_value());
    CHECK(parsed->chain);
    CHECK(parsed->category == "Vocals");
    CHECK(parsed->getDeviceNames() == juce::StringArray{"Subtractive EQ", "Gate", "Air EQ"});
    CHECK(parsed->findFor(devices::Equaliser::kTypeId, 0)->deviceName == "Subtractive EQ");
    CHECK(parsed->findFor(devices::Equaliser::kTypeId, 1)->deviceName == "Air EQ");
    CHECK(parsed->findFor(devices::Equaliser::kTypeId, 4)->deviceName == "Subtractive EQ"); // (past the last: the first)
    CHECK(parsed->findFor("builtin:gate", 1)->deviceName == "Gate");
    CHECK(parsed->findFor("builtin:reverb") == nullptr);

    // A device preset keeps one entry per kind.
    presets::Preset single;
    single.put(first);
    single.put(second);
    CHECK(single.devices.size() == 1);
    CHECK(single.findFor(devices::Equaliser::kTypeId, 1)->deviceName == "Air EQ");
}

TEST_CASE("Nine built-in chains, each device in its place with its own role", "[presets][chains]")
{
    const std::map<juce::String, std::pair<juce::String, juce::StringArray>> expected{
        {"Punchy Drum Bus", {"Drums", {"Drum EQ", "Glue Compressor", "Saturation", "Bass Mono", "Soft Clipper"}}},
        {"Dusty Lo-Fi Drums", {"Drums", {"Sampler Grit", "Crunch", "Smash", "Dusty Filter", "Thump EQ"}}},
        {"Pop Vocal Polish", {"Vocals", {"Expander", "Clean-Up EQ", "Vocal Compressor", "De-Esser", "Quarter Echo", "Vocal Plate"}}},
        {"Upfront Rap Vocal", {"Vocals", {"Tight Gate", "Vocal EQ", "Fast Compressor", "Grit", "De-Esser", "Eighth Throw", "Level Lock"}}},
        {"Dreamy Wide Space", {"General", {"Clean Lows", "Ensemble", "Dream Echo", "Big Hall", "Wider"}}},
        {"Old Radio", {"General", {"Band-Pass", "Tube Distortion", "Low Rate", "Mono"}}},
        {"Drivers License Vocal Saturation",
         {"Vocals", {"Noise Gate", "Subtractive EQ", "Fast Compressor", "Saturation", "Air EQ (Red Light)", "Slapback Delay", "Short Plate"}}},
        {"Gritty Bass Weight", {"Bass", {"Bass Compressor", "Mid Grit", "Weight & Growl", "Mono Lows"}}},
        {"Shimmer Bloom", {"Pads & Keys", {"Clean Lows", "Octave Shimmer", "Chorus", "Shimmer Hall"}}},
    };

    int chains = 0;
    for (const auto& preset : presets::getBuiltIn())
    {
        if (!preset.chain)
            continue;
        ++chains;
        INFO(preset.name.toStdString());
        const auto it = expected.find(preset.name);
        REQUIRE(it != expected.end());
        CHECK(preset.category == it->second.first);
        CHECK(preset.getDeviceNames() == it->second.second);
        CHECK(preset.description.length() > 60);
        for (const auto& entry : preset.devices)
        {
            auto processor = devices::createBuiltinDevice(entry.key);
            REQUIRE(processor != nullptr);
            // Every parameter is set: loading a chain defines each device completely.
            const auto xml = juce::AudioProcessor::getXmlFromBinary(entry.state.getData(), static_cast<int>(entry.state.getSize()));
            REQUIRE(xml != nullptr);
            CHECK(xml->getNumChildElements() == processor->getParameters().size() + 1); // (+ EXTRA)
        }
    }
    CHECK(chains == 9);

    // The Drivers License chain, as asked: gate, subtractive EQ, compressor, overdrive, additive EQ,
    // slapback delay, short plate.
    const auto dl = builtIn("Drivers License Vocal Saturation");
    juce::StringArray kinds;
    for (const auto& entry : dl.devices)
        kinds.add(entry.key);
    CHECK(kinds == juce::StringArray{"builtin:gate", "builtin:eq", "builtin:compressor", "builtin:overdrive", "builtin:eq",
                                     "builtin:echo", "builtin:plate"});

    // The chains use the dedicated devices: a real De-Esser, Glue Compressor, Expander, Soft Clipper
    // and Plate Reverb (each also in the browser on its own).
    const auto kindOf = [](const presets::Preset& preset, const juce::String& role)
    {
        for (const auto& entry : preset.devices)
            if (entry.deviceName == role)
                return entry.key;
        return juce::String();
    };
    CHECK(kindOf(builtIn("Pop Vocal Polish"), "De-Esser") == "builtin:deesser");
    CHECK(kindOf(builtIn("Upfront Rap Vocal"), "De-Esser") == "builtin:deesser");
    CHECK(kindOf(builtIn("Pop Vocal Polish"), "Expander") == "builtin:expander");
    CHECK(kindOf(builtIn("Pop Vocal Polish"), "Vocal Plate") == "builtin:plate");
    CHECK(kindOf(builtIn("Punchy Drum Bus"), "Glue Compressor") == "builtin:gluecomp");
    CHECK(kindOf(builtIn("Punchy Drum Bus"), "Soft Clipper") == "builtin:softclip");
}

TEST_CASE("Adding a chain puts every device on the track, in order, as one undo step", "[presets][chains]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Lead Vocal");
    session.addDevice(track, "builtin:utility", "Utility");
    const auto dl = builtIn("Drivers License Vocal Saturation");

    juce::String message;
    session.getUndoManager().beginNewTransaction();
    const auto first = ui::presetActions::addChain(project, plugins, editorState, track, dl, &message);
    REQUIRE(first.isValid());
    CHECK(message == "Added chain \"Drivers License Vocal Saturation\" (7 devices)");
    auto chain = session::Session::getDevices(track);
    REQUIRE(chain.size() == 8);
    juce::StringArray names;
    for (const auto& d : chain)
        names.add(session::device::getName(d));
    CHECK(names == juce::StringArray{"Utility", "Noise Gate", "Subtractive EQ", "Fast Compressor", "Saturation", "Air EQ (Red Light)",
                                     "Slapback Delay", "Short Plate"});
    const auto processorOf = [&](const juce::ValueTree& d) -> juce::AudioProcessor& { return project.getDevices().getNode(d)->getProcessor(); };
    CHECK_THAT(valueOf(processorOf(chain[1]), "threshold"), WithinAbs(-50.0, 1e-3));
    CHECK_THAT(valueOf(processorOf(chain[2]), "band2Gain"), WithinAbs(-3.5, 1e-4));
    CHECK_THAT(valueOf(processorOf(chain[2]), "band1Freq"), WithinAbs(90.0, 0.1));
    CHECK_THAT(valueOf(processorOf(chain[3]), "attack"), WithinAbs(1.0, 1e-3));
    CHECK_THAT(valueOf(processorOf(chain[5]), "band4Gain"), WithinAbs(5.5, 1e-4));
    CHECK_THAT(valueOf(processorOf(chain[5]), "band1On"), WithinAbs(0.0, 1e-6));
    CHECK_THAT(valueOf(processorOf(chain[6]), "timel"), WithinAbs(92.0, 0.01));
    CHECK_THAT(valueOf(processorOf(chain[7]), "decay"), WithinAbs(0.8, 1e-3)); // (the Plate Reverb)

    // One undo takes the whole chain away; redo brings it back with its settings.
    session.getUndoManager().undo();
    CHECK(session::Session::getDevices(track).size() == 1);
    session.getUndoManager().redo();
    chain = session::Session::getDevices(track);
    REQUIRE(chain.size() == 8);
    CHECK_THAT(valueOf(processorOf(chain[5]), "band4Gain"), WithinAbs(5.5, 1e-4));
    CHECK_THAT(valueOf(processorOf(chain[2]), "band2Gain"), WithinAbs(-3.5, 1e-4));

    // With no track selected, a chain gets a new audio track.
    const auto tracksBefore = session.getAllTracks().size();
    const auto added = ui::presetActions::addChain(project, plugins, editorState, {}, builtIn("Old Radio"));
    REQUIRE(added.isValid());
    CHECK(session.getAllTracks().size() == tracksBefore + 1);
    CHECK(session::Session::getDevices(added.getParent()).size() == 4);

    // Using a chain on a track (a drop on it, or a double-click in the browser) adds it too, even
    // when the track already has devices it would fit.
    const auto outcome = ui::presetActions::useOn(project, plugins, editorState, track, builtIn("Shimmer Bloom"));
    CHECK(outcome.ok);
    CHECK(session::Session::getDevices(track).size() == 12);
}

TEST_CASE("A device of a chain loads its own part of the chain, by its position", "[presets][chains]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Vox");
    auto firstEq = session.addDevice(track, devices::Equaliser::kTypeId, "EQ");
    auto compressor = session.addDevice(track, "builtin:compressor", "Compressor");
    auto secondEq = session.addDevice(track, devices::Equaliser::kTypeId, "EQ 2");
    const auto dl = builtIn("Drivers License Vocal Saturation");
    CHECK(presets::occurrenceOf(firstEq) == 0);
    CHECK(presets::occurrenceOf(secondEq) == 1);
    CHECK(presets::occurrenceOf(compressor) == 0);

    auto& eq1 = project.getDevices().getNode(firstEq)->getProcessor();
    auto& eq2 = project.getDevices().getNode(secondEq)->getProcessor();
    CHECK(ui::presetActions::load(project, dl, secondEq).ok);
    CHECK_THAT(valueOf(eq2, "band4Gain"), WithinAbs(5.5, 1e-4)); // the Air EQ
    CHECK_THAT(valueOf(eq1, "band2Gain"), WithinAbs(0.0, 1e-4)); // (untouched)
    CHECK(ui::presetActions::load(project, dl, firstEq).ok);
    CHECK_THAT(valueOf(eq1, "band2Gain"), WithinAbs(-3.5, 1e-4)); // the subtractive EQ
    CHECK(ui::presetActions::load(project, dl, compressor).ok);
    CHECK_THAT(valueOf(project.getDevices().getNode(compressor)->getProcessor(), "ratio"), WithinAbs(6.0, 1e-3));

    // Dropped on a card of the Devices panel: the same.
    editorState.selectTrack(session::track::getId(track));
    ui::DevicePanel panel(project, plugins, editorState);
    panel.setSize(1600, 300);
    test::devices::setParameter(dynamic_cast<devices::BuiltinProcessor&>(eq2), "band4Gain", 0.0f);
    auto* card = dynamic_cast<juce::DragAndDropTarget*>(panel.findCard(secondEq));
    REQUIRE(card != nullptr);
    card->itemDropped(juce::DragAndDropTarget::SourceDetails(ui::presetActions::toDragDescription(dl), nullptr, {5, 5}));
    CHECK_THAT(valueOf(eq2, "band4Gain"), WithinAbs(5.5, 1e-4));
    CHECK(panel.getNotice(secondEq) == "Loaded \"Drivers License Vocal Saturation\"");
    CHECK(panel.getPresetMenuItems(compressor).contains("Drivers License Vocal Saturation  (from chain)"));

    // The "+ Add Device" menu offers every chain; dropped on the panel a chain is added whole.
    const auto offered = panel.getAddMenuChainItems();
    for (const auto& name : kChains)
        CHECK(offered.contains(name));
    panel.itemDropped(juce::DragAndDropTarget::SourceDetails(ui::presetActions::toDragDescription(builtIn("Punchy Drum Bus")), nullptr, {5, 5}));
    CHECK(session::Session::getDevices(track).size() == 8);
    CHECK(panel.getPanelNotice() == "Added chain \"Punchy Drum Bus\" (5 devices)");
}

TEST_CASE("Save Whole Chain keeps every device and its order, and adds back the same chain", "[presets][chains]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    presets::setUserFolder(temp.dir.getChildFile("Presets"));
    auto project = makeProject();
    auto& session = project.getSession();
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Gtr");
    auto eqA = session.addDevice(track, devices::Equaliser::kTypeId, "Low EQ");
    session.addDevice(track, "builtin:compressor", "Compressor");
    auto eqB = session.addDevice(track, devices::Equaliser::kTypeId, "High EQ");
    test::devices::setParameter(dynamic_cast<devices::BuiltinProcessor&>(project.getDevices().getNode(eqA)->getProcessor()), "band1Freq", 150.0f);
    test::devices::setParameter(dynamic_cast<devices::BuiltinProcessor&>(project.getDevices().getNode(eqB)->getProcessor()), "band4Gain", 7.0f);

    const auto file = temp.dir.getChildFile("Presets/Guitar Chain.dojopreset");
    const auto outcome = ui::presetActions::saveChain(project, track, file);
    CHECK(outcome.ok);
    CHECK(outcome.message == "Saved chain \"Guitar Chain\" (Low EQ, Compressor, High EQ)");
    juce::String error;
    const auto saved = presets::Preset::load(file, error);
    REQUIRE(saved.has_value());
    CHECK(saved->chain);
    CHECK(saved->getDeviceNames() == juce::StringArray{"Low EQ", "Compressor", "High EQ"});

    auto other = session.addTrack(session::TrackKind::audio, "Gtr 2");
    REQUIRE(ui::presetActions::addChain(project, plugins, editorState, other, *saved).isValid());
    const auto copy = session::Session::getDevices(other);
    REQUIRE(copy.size() == 3);
    CHECK(session::device::getName(copy[2]) == "High EQ");
    CHECK_THAT(valueOf(project.getDevices().getNode(copy[0])->getProcessor(), "band1Freq"), WithinAbs(150.0, 0.1));
    CHECK_THAT(valueOf(project.getDevices().getNode(copy[2])->getProcessor(), "band4Gain"), WithinAbs(7.0, 1e-4));
    presets::setUserFolder({});
}

//==================================================================================================
// What each built-in chain does to sound (measured through its devices, set up from the preset).

TEST_CASE("Every built-in chain runs cleanly at a sensible level", "[presets][chains][audio]")
{
    for (const auto& name : kChains)
    {
        INFO(name.toStdString());
        const auto preset = builtIn(name);
        Chain chain(preset);
        const auto out = chain.run(noise(samples(2.0), -20.0));
        REQUIRE(finite(out));
        const double level = rmsDb(out, samples(1.0), samples(2.0));
        CHECK(level > -26.0);
        CHECK(level < -16.0);
        Chain hot(preset);
        const auto loud = hot.run(noise(samples(1.0), -6.0, 7));
        REQUIRE(finite(loud));
        CHECK(peakOf(loud) < 1.5);
    }
}

TEST_CASE("Drivers License Vocal Saturation: each of its seven stages does its job", "[presets][chains][audio]")
{
    const auto dl = builtIn("Drivers License Vocal Saturation");

    // Gate: fan, AC and street noise between phrases is shut off completely.
    {
        Chain chain(dl);
        const auto out = chain.run(noise(samples(2.0), -66.0));
        CHECK(rmsDb(out, samples(0.5), samples(2.0)) < -120.0);
    }

    // Subtractive EQ: the rumble goes (and the boxy 450 Hz region dips below its neighbours).
    const auto clean = upTo(dl, "Air EQ (Red Light)");
    CHECK(gainAt(clean, 40.0, -24.0) < gainAt(clean, 1000.0, -24.0) - 15.0);
    const auto beforeCompression = upTo(dl, "Subtractive EQ");
    CHECK(gainAt(beforeCompression, 450.0, -24.0) < -3.0);
    CHECK(gainAt(beforeCompression, 450.0, -24.0) < gainAt(beforeCompression, 1500.0, -24.0) - 2.5);

    // Compressor: a whisper (-40 dBFS) and a belt (-6 dBFS), 34 dB apart, come out less than 22 dB apart.
    CHECK(gainAt(dl, 1000.0, -40.0) - gainAt(dl, 1000.0, -6.0) > 12.0);

    // Overdrive: subtle, but there: a little third harmonic, far below the tone.
    {
        Chain chain(upTo(dl, "Saturation"));
        const auto n = samples(1.0);
        const auto out = chain.run(test::devices::tone(1000.0, n, toneDb(-12.0)));
        const double third = db(test::devices::amplitudeAt(out.left, 3000.0, n / 2, n) / test::devices::amplitudeAt(out.left, 1000.0, n / 2, n));
        CHECK(third > -60.0);
        CHECK(third < -25.0);
    }

    // Additive EQ: the air above 10 kHz lifted over the body of the voice.
    CHECK(gainAt(clean, 12000.0, -24.0) > gainAt(clean, 1000.0, -24.0) + 2.0);

    // Slapback: one tight, quiet echo, at a different time on each side (width), no long tail.
    {
        Chain chain(dl);
        const auto out = chain.run(burst(2.0));
        const auto window = [&](const std::vector<double>& x, double fromMs, double toMs)
        { return db(test::devices::rmsOf(x, samples(fromMs / 1000.0), samples(toMs / 1000.0))); };
        const double dryLevel = window(out.left, 0.0, 10.0);
        CHECK(window(out.left, 90.0, 102.0) > dryLevel - 20.0);  // left echo at 92 ms
        CHECK(window(out.right, 116.0, 128.0) > dryLevel - 20.0); // right echo at 118 ms
        CHECK(window(out.left, 90.0, 102.0) > window(out.right, 90.0, 102.0) + 6.0);
        CHECK(window(out.right, 116.0, 128.0) > window(out.left, 116.0, 128.0) + 6.0);
        // Short plate: the space has died away within a second.
        CHECK(rmsDb(out, samples(1.0), samples(1.5)) < dryLevel - 70.0);
    }
}

TEST_CASE("The drum chains: a punchy bus and a dusty lo-fi kit", "[presets][chains][audio]")
{
    const auto punchy = builtIn("Punchy Drum Bus");
    CHECK(gainAt(punchy, 60.0, -24.0) > gainAt(punchy, 420.0, -24.0) + 3.0); // kick weight over cardboard
    {
        Chain chain(punchy); // hot drums stay under the clipper's ceiling (-1 dBFS)
        CHECK(peakOf(chain.run(noise(samples(1.0), -4.0, 3))) < 0.93);
    }
    {
        Chain chain(punchy); // the low end is mono
        const auto out = chain.run(leftOnly(test::devices::tone(60.0, samples(1.0), 0.3)));
        const double left = test::devices::amplitudeAt(out.left, 60.0, samples(0.5), samples(1.0));
        const double right = test::devices::amplitudeAt(out.right, 60.0, samples(0.5), samples(1.0));
        CHECK(std::abs(db(left / right)) < 1.5);
    }

    const auto dusty = builtIn("Dusty Lo-Fi Drums");
    CHECK(gainAt(dusty, 12000.0, -24.0) < gainAt(dusty, 1000.0, -24.0) - 12.0); // dark, old-sampler top
    CHECK(gainAt(dusty, 90.0, -24.0) > gainAt(dusty, 1000.0, -24.0));           // extra thump
}

TEST_CASE("The vocal chains de-ess loud sibilance only, gate the noise, and the rap chain is limited", "[presets][chains][audio]")
{
    for (const auto& [name, sibilance] : std::vector<std::pair<juce::String, double>>{{"Pop Vocal Polish", 8000.0}, {"Upfront Rap Vocal", 7000.0}})
    {
        INFO(name.toStdString());
        const auto chain = builtIn(name);
        const auto noDeEsser = without(chain, "De-Esser");
        // A loud "s": clearly reduced by the de-esser.
        CHECK(gainAt(chain, sibilance, -10.0) < gainAt(noDeEsser, sibilance, -10.0) - 3.0);
        // The normal air of a voice under its body: left alone.
        const auto n = samples(1.0);
        const auto voice = plus(test::devices::tone(1000.0, n, toneDb(-12.0)), test::devices::tone(sibilance, n, toneDb(-40.0)));
        Chain with(chain), withoutIt(noDeEsser);
        const auto a = with.run(voice);
        const auto b = withoutIt.run(voice);
        CHECK(std::abs(db(test::devices::amplitudeAt(a.left, sibilance, n / 2, n) / test::devices::amplitudeAt(b.left, sibilance, n / 2, n))) < 1.0);
        // Room noise between phrases is kept down.
        Chain quiet(chain);
        CHECK(rmsDb(quiet.run(noise(samples(1.5), -70.0)), samples(0.5), samples(1.5)) < -80.0);
    }
    Chain rap(builtIn("Upfront Rap Vocal"));
    CHECK(peakOf(rap.run(noise(samples(1.0), -3.0, 5))) < 0.92); // the limiter's -1 dBFS ceiling
}

TEST_CASE("The general chains: a wide dreamy space and an old radio", "[presets][chains][audio]")
{
    const auto dreamy = builtIn("Dreamy Wide Space");
    {
        Chain chain(dreamy);
        const auto out = chain.run(mono(burst(4.0)));
        std::vector<double> side(out.left.size()), mid(out.left.size());
        for (std::size_t i = 0; i < side.size(); ++i)
        {
            side[i] = 0.5 * (out.left[i] - out.right[i]);
            mid[i] = 0.5 * (out.left[i] + out.right[i]);
        }
        // A mono source comes out wide...
        CHECK(test::devices::rmsOf(side, samples(0.2), samples(2.0)) > 0.5 * test::devices::rmsOf(mid, samples(0.2), samples(2.0)));
        // ...with a long tail (seconds after a 10 ms sound).
        CHECK(rmsDb(out, samples(2.0), samples(3.0)) > -75.0);
    }

    const auto radio = builtIn("Old Radio");
    {
        Chain chain(radio);
        const auto out = chain.run(noise(samples(1.0), -20.0));
        CHECK(out.left == out.right); // mono
    }
    CHECK(gainAt(radio, 100.0, -24.0) < gainAt(radio, 1500.0, -24.0) - 30.0);  // no lows
    CHECK(gainAt(radio, 8000.0, -24.0) < gainAt(radio, 1500.0, -24.0) - 30.0); // no highs
    {
        Chain chain(radio); // tube distortion and a low sample rate: plenty of harmonics
        const auto n = samples(1.0);
        const auto out = chain.run(test::devices::tone(1000.0, n, toneDb(-12.0)));
        CHECK(test::devices::amplitudeAt(out.left, 3000.0, n / 2, n) > 0.05 * test::devices::amplitudeAt(out.left, 1000.0, n / 2, n));
    }
}

TEST_CASE("Gritty Bass Weight keeps the lows clean and mono and puts the grit in the mids", "[presets][chains][audio]")
{
    const auto bass = builtIn("Gritty Bass Weight");
    const auto harmonics = [&](double f)
    {
        Chain chain(bass);
        const auto n = samples(1.0);
        const auto out = chain.run(test::devices::tone(f, n, 0.3));
        const double fundamental = test::devices::amplitudeAt(out.left, f, n / 2, n);
        return std::max(test::devices::amplitudeAt(out.left, 2 * f, n / 2, n), test::devices::amplitudeAt(out.left, 3 * f, n / 2, n)) /
               fundamental;
    };
    CHECK(db(harmonics(60.0)) < -30.0);  // clean weight
    CHECK(db(harmonics(400.0)) > -20.0); // gritty mids
    Chain chain(bass);
    const auto out = chain.run(leftOnly(test::devices::tone(60.0, samples(1.0), 0.3)));
    CHECK(std::abs(db(test::devices::amplitudeAt(out.left, 60.0, samples(0.5), samples(1.0)) /
                      test::devices::amplitudeAt(out.right, 60.0, samples(0.5), samples(1.0)))) < 1.5);
}

TEST_CASE("Shimmer Bloom adds a rising octave and a long, bright tail", "[presets][chains][audio]")
{
    const auto shimmer = builtIn("Shimmer Bloom");
    Chain chain(shimmer);
    const auto n = samples(2.0);
    const auto out = chain.run(test::devices::tone(440.0, n, 0.3));
    const double fundamental = test::devices::amplitudeAt(out.left, 440.0, n / 2, n);
    CHECK(amplitudeNear(out.left, 880.0, 15.0, n / 2, n) > toneDb(-22.0) * fundamental);  // an octave up
    CHECK(amplitudeNear(out.left, 1760.0, 30.0, n / 2, n) > toneDb(-40.0) * fundamental); // and another (feedback)
    Chain tail(shimmer);
    CHECK(rmsDb(tail.run(burst(4.0)), samples(3.0), samples(4.0)) > -80.0);
}

TEST_CASE("A chain adds only its own latency, also inside a folder next to a sidechained track", "[presets][chains][pdc]")
{
    juce::MessageManager::getInstance();
    auto project = makeProject();
    auto& session = project.getSession();
    test::TempFolder temp;
    plugins::PluginManager plugins(juce::File(), temp.dir.getChildFile("plugins.xml"));
    ui::EditorState editorState;
    REQUIRE(ui::createDemoProject(project, temp.dir).wasOk()); // (a folder of drums and a bass sidechained from them)
    project.getCompiler().rebuildNow();
    const int before = project.getCompiler().getTotalLatency();

    juce::ValueTree folder;
    for (const auto& t : session.getAllTracks())
        if (session::track::getKind(t) == session::TrackKind::bus && session::track::getName(t) == "Rhythm")
            folder = t;
    REQUIRE(folder.isValid());
    auto track = session.addTrack(session::TrackKind::audio, "Vox");
    REQUIRE(session.moveIntoFolder(track, folder));
    REQUIRE(ui::presetActions::addChain(project, plugins, editorState, track, builtIn("Drivers License Vocal Saturation")).isValid());
    project.getCompiler().rebuildNow();

    int chainLatency = 0;
    for (const auto& d : session::Session::getDevices(track))
        chainLatency += project.getDevices().getNode(d)->getLatencySamples();
    CHECK(chainLatency == 48 + 61); // the gate's 1 ms lookahead and the overdrive's oversampling
    CHECK(project.getCompiler().getTotalLatency() == before + chainLatency);
}

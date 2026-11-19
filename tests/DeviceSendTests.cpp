// Device sends: a send that takes a device's own extra outputs (a console channel's bus
// assignments, aux and cue sends) to a bus, summed there with every other channel's.

#include "devices/BuiltinProcessor.h"
#include "devices/DeviceRegistry.h"
#include "devices/Harrison4032.h"
#include "engine/AudioEngine.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::session;
using Catch::Matchers::WithinAbs;
using Outputs = devices::Harrison4032::Outputs;

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

    juce::ValueTree trackWith(const juce::File& file)
    {
        juce::String error;
        auto asset = assets.loadNow(file, error);
        REQUIRE(asset != nullptr);
        auto t = session.addTrack();
        session.addClip(t, file, 0.0, asset->getLengthSeconds());
        return t;
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
} // namespace

TEST_CASE("A device send takes a device's own outputs to a bus, as a console's bus assignment does", "[engine][devicesend]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(1000.0, kRate, 48000, 0.1f), kRate);
    const auto render = [&](const std::function<void(Rig&, juce::ValueTree, juce::ValueTree, juce::ValueTree)>& setup)
    {
        Rig rig;
        auto t = rig.trackWith(tone);
        auto console = rig.session.addDevice(t, devices::Harrison4032::kTypeId, "Harrison 4032");
        rig.set(console, "input", 1.0f); // (the line input: unity)
        rig.set(console, "mix", 0.0f);   // the channel only on its buses: the track's own output is silent
        auto bus = rig.session.addTrack(TrackKind::bus, "Bus 5-6");
        if (setup)
            setup(rig, t, console, bus);
        rig.publish();
        rig.play();
        rig.render(0.6);
        return std::make_pair(test::rms(rig.left, at(0.3), at(0.6)), test::rms(rig.right, at(0.3), at(0.6)));
    };
    const double reference = 0.1 / std::sqrt(2.0);

    // Buses 5 and 6 (the pair's left and right) to the "Bus 5-6" bus.
    const auto [left, right] = render(
        [](Rig& rig, juce::ValueTree t, juce::ValueTree console, juce::ValueTree bus)
        {
            rig.set(console, "bus5", 1.0f);
            rig.set(console, "bus6", 1.0f);
            REQUIRE(rig.session.addDeviceSend(t, bus, console, Outputs::bus(5), Outputs::bus(6)).isValid());
        });
    CHECK_THAT(toDb(left / reference), WithinAbs(0.0, 0.05));
    CHECK_THAT(toDb(right / reference), WithinAbs(0.0, 0.05));
    // Only bus 5: the pair's left side.
    const auto [onlyLeft, silentRight] = render(
        [](Rig& rig, juce::ValueTree t, juce::ValueTree console, juce::ValueTree bus)
        {
            rig.set(console, "bus5", 1.0f);
            rig.session.addDeviceSend(t, bus, console, Outputs::bus(5), Outputs::bus(6));
        });
    CHECK_THAT(toDb(onlyLeft / reference), WithinAbs(0.0, 0.05));
    CHECK(silentRight < reference * 1e-3); // (the C's crosstalk: -90 dB)
    // An aux send (mono) to both sides of its bus, at the send's level.
    const auto [auxLeft, auxRight] = render(
        [](Rig& rig, juce::ValueTree t, juce::ValueTree console, juce::ValueTree bus)
        {
            rig.set(console, "aux2", -6.0f);
            rig.session.addDeviceSend(t, bus, console, Outputs::aux(2), Outputs::aux(2), -3.0);
        });
    CHECK_THAT(toDb(auxLeft / reference), WithinAbs(-9.0, 0.05));
    CHECK_THAT(toDb(auxRight / reference), WithinAbs(-9.0, 0.05));
    // The source track muted: its device sends are silent too.
    const auto [mutedLeft, mutedRight] = render(
        [](Rig& rig, juce::ValueTree t, juce::ValueTree console, juce::ValueTree bus)
        {
            rig.set(console, "bus5", 1.0f);
            rig.session.addDeviceSend(t, bus, console, Outputs::bus(5), Outputs::bus(6));
            track::setMuted(t, true, nullptr);
        });
    CHECK(mutedLeft < 1e-9);
    // No such outputs (or no such device): the send carries nothing.
    Rig rig;
    auto t = rig.trackWith(tone);
    auto console = rig.session.addDevice(t, devices::Harrison4032::kTypeId, "Harrison 4032");
    auto bus = rig.session.addTrack(TrackKind::bus, "Bus");
    rig.session.addDeviceSend(t, bus, console, 60, 61);
    auto eq = rig.session.addDevice(t, "builtin:eq", "EQ");
    rig.session.addDeviceSend(t, bus, eq, 0, 1); // (an EQ has no extra outputs)
    rig.session.addDeviceSend(t, bus, console, Outputs::aux(1), Outputs::aux(1));
    const auto graph = rig.builder.build(rig.session, rig.assets, &rig.devices);
    const auto& busTrack = *std::find_if(graph->tracks.begin(), graph->tracks.end(),
                                         [&](const engine::RenderTrack& rt) { return rt.trackId == track::getId(bus); });
    REQUIRE(busTrack.sends.size() == 1);
    CHECK(busTrack.sends.front().auxNode == rig.devices.getNode(console).get());
    CHECK(busTrack.sends.front().auxLeft == Outputs::aux(1));
}

TEST_CASE("Device sends are delay-compensated from where their device sits in the chain", "[engine][devicesend][pdc]")
{
    // Two tracks play the same click into one bus: one through a 5 ms limiter and the console
    // (its bus assignment, a device send), the other routed there directly. They meet in time.
    test::TempFolder temp;
    const auto click = test::writeImpulse(temp.dir.getChildFile("click.wav"), 4800, 9600, kRate, 0.2f);
    Rig rig;
    auto a = rig.trackWith(click);
    auto b = rig.trackWith(click);
    auto bus = rig.session.addTrack(TrackKind::bus, "Bus 1-2");
    rig.set(rig.session.addDevice(a, "builtin:limiter", "Limiter"), "lookahead", 5.0f); // 240 samples
    auto console = rig.session.addDevice(a, devices::Harrison4032::kTypeId, "Harrison 4032");
    rig.set(console, "input", 1.0f);
    rig.set(console, "mix", 0.0f);
    rig.set(console, "bus1", 1.0f);
    rig.set(console, "bus2", 1.0f);
    REQUIRE(rig.session.addDeviceSend(a, bus, console, Outputs::bus(1), Outputs::bus(2)).isValid());
    track::setOutput(b, track::getId(bus), nullptr);
    rig.publish();
    rig.play();
    rig.render(0.3);
    // One peak, both clicks on it (0.2 + 0.2), late by the limiter.
    CHECK(test::indexOfPeak(rig.left) == 4800 + 240);
    CHECK_THAT(test::peak(rig.left), WithinAbs(0.4, 0.01));
    CHECK(rig.builder.getTotalLatency() == 240);
}

TEST_CASE("Device sends go with their device, follow track copies, and show where they come from", "[session][devicesend]")
{
    Session session;
    auto t = session.addTrack(TrackKind::audio, "Vocal");
    auto console = session.addDevice(t, devices::Harrison4032::kTypeId, "Harrison 4032");
    auto busA = session.addTrack(TrackKind::bus, "Bus 1-2");
    auto busB = session.addTrack(TrackKind::bus, "Aux 1");
    const auto s1 = session.addDeviceSend(t, busA, console, Outputs::bus(1), Outputs::bus(2));
    const auto s2 = session.addDeviceSend(t, busB, console, Outputs::aux(1), Outputs::aux(1), -4.0);
    REQUIRE(s1.isValid());
    REQUIRE(s2.isValid());
    CHECK(send::getSourceDevice(s1) == device::getId(console));
    CHECK(send::getSourceChannels(s2) == std::make_pair(Outputs::aux(1), Outputs::aux(1)));
    CHECK(send::getGainDb(s2) == -4.0);
    CHECK(Session::findDeviceSend(t, device::getId(console), Outputs::bus(1), Outputs::bus(2)) == s1);
    CHECK_FALSE(Session::findDeviceSend(t, device::getId(console), Outputs::bus(3), Outputs::bus(4)).isValid());
    // A device send cannot close a loop either.
    CHECK_FALSE(session.addDeviceSend(busA, busA, console, 0, 1).isValid());

    // Duplicated with the track: the copies take the copied device's outputs.
    auto copy = session.duplicateTrack(t, {}, t);
    REQUIRE(copy.isValid());
    const auto copiedDevice = Session::getDevices(copy).front();
    const auto copiedSends = Session::getSends(copy);
    REQUIRE(copiedSends.size() == 2);
    for (const auto& s : copiedSends)
        CHECK(send::getSourceDevice(s) == device::getId(copiedDevice));
    Session::DuplicateOptions noDevices;
    noDevices.devices = false;
    auto bare = session.duplicateTrack(t, noDevices, copy);
    CHECK(Session::getSends(bare).empty());

    // Removing the device removes its sends; undo brings them back.
    session.getUndoManager().beginNewTransaction();
    session.removeDevice(console);
    CHECK(Session::getSends(t).empty());
    session.getUndoManager().undo();
    CHECK(Session::getSends(t).size() == 2);
}

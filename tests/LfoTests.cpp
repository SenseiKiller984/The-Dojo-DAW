#include "devices/AutoFilter.h"
#include "devices/DeviceRegistry.h"
#include "devices/LfoDevice.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "DeviceTestSupport.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::devices;
using Catch::Matchers::WithinAbs;
using test::devices::setParameter;

//==================================================================================================
// The device

TEST_CASE("LFO shape, depth, offset, phase and invert", "[devices][lfo]")
{
    LfoDevice lfo;
    CHECK_THAT(lfo.shapeAt(0.0), WithinAbs(0.5, 1e-9));
    CHECK_THAT(lfo.shapeAt(0.25), WithinAbs(1.0, 1e-9));
    CHECK_THAT(lfo.shapeAt(0.75), WithinAbs(0.0, 1e-9));

    setParameter(lfo, "depth", 50.0f);
    CHECK_THAT(lfo.shapeAt(0.25), WithinAbs(0.75, 1e-6));
    setParameter(lfo, "offset", 50.0f);
    CHECK_THAT(lfo.shapeAt(0.25), WithinAbs(1.0, 1e-6)); // (clamped)
    CHECK_THAT(lfo.shapeAt(0.75), WithinAbs(0.5, 1e-6));
    setParameter(lfo, "offset", 0.0f);
    setParameter(lfo, "invert", 1.0f);
    CHECK_THAT(lfo.shapeAt(0.25), WithinAbs(0.25, 1e-6));
    setParameter(lfo, "invert", 0.0f);
    setParameter(lfo, "depth", 100.0f);
    setParameter(lfo, "phase", 90.0f);
    CHECK_THAT(lfo.shapeAt(0.0), WithinAbs(1.0, 1e-6));

    // Every shape stays within 0..1.
    setParameter(lfo, "phase", 0.0f);
    for (int shape = 0; shape < 8; ++shape)
    {
        setParameter(lfo, "shape", static_cast<float>(shape));
        for (double p = 0.0; p < 4.0; p += 0.01)
        {
            const double v = lfo.shapeAt(p);
            REQUIRE(v >= 0.0);
            REQUIRE(v <= 1.0);
        }
    }
}

TEST_CASE("LFO runs free or locked to the song; hold, smooth and jitter", "[devices][lfo]")
{
    LfoDevice lfo;
    // 2 Hz: an eighth of a second is a quarter cycle (the sine's top).
    setParameter(lfo, "rate", 2.0f);
    CHECK_THAT(lfo.advanceModulation(6000, 48000.0, 120.0, 0.0, false), WithinAbs(1.0, 1e-6));
    CHECK_THAT(lfo.getPhase(), WithinAbs(0.25, 1e-6));

    // Synced to 1/4 while playing: the phase is the song position in beats.
    setParameter(lfo, "sync", 1.0f);
    setParameter(lfo, "division", 9.0f); // 1/4
    CHECK_THAT(lfo.advanceModulation(256, 48000.0, 120.0, 10.75, true), WithinAbs(0.0, 1e-6)); // three quarters
    CHECK_THAT(lfo.advanceModulation(256, 48000.0, 90.0, 3.25, true), WithinAbs(1.0, 1e-6));

    // Hold freezes it.
    setParameter(lfo, "hold", 1.0f);
    const double held = lfo.advanceModulation(256, 48000.0, 90.0, 3.5, true);
    CHECK_THAT(lfo.advanceModulation(256, 48000.0, 90.0, 3.9, true), WithinAbs(held, 1e-6));
    setParameter(lfo, "hold", 0.0f);
    setParameter(lfo, "sync", 0.0f);

    // A square jumps between 0 and 1; Smooth turns the jumps into slopes.
    setParameter(lfo, "shape", 4.0f);
    setParameter(lfo, "rate", 1.0f);
    const auto biggestStep = [&]
    {
        double previous = lfo.advanceModulation(256, 48000.0, 120.0, 0.0, false), step = 0.0;
        for (int block = 0; block < 400; ++block)
        {
            const double v = lfo.advanceModulation(256, 48000.0, 120.0, 0.0, false);
            step = std::max(step, std::abs(v - previous));
            previous = v;
        }
        return step;
    };
    CHECK_THAT(biggestStep(), WithinAbs(1.0, 1e-6));
    setParameter(lfo, "smooth", 50.0f);
    CHECK(biggestStep() < 0.2);
    setParameter(lfo, "smooth", 0.0f);

    // Jitter: the same point of different cycles lands differently.
    setParameter(lfo, "shape", 0.0f);
    setParameter(lfo, "jitter", 60.0f);
    setParameter(lfo, "rate", 1.0f);
    lfo.advanceModulation(1, 48000.0, 120.0, 0.0, false);
    std::vector<double> tops;
    for (int cycle = 0; cycle < 4; ++cycle)
        tops.push_back(lfo.advanceModulation(48000, 48000.0, 120.0, 0.0, false));
    CHECK(std::abs(tops[0] - tops[1]) + std::abs(tops[1] - tops[2]) + std::abs(tops[2] - tops[3]) > 0.05);
}

//==================================================================================================
// Mapped across the project

namespace
{
struct LfoRig
{
    LfoRig()
        : project([this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                  [](const juce::ValueTree& d, double, int, juce::String&) { return createBuiltinDevice(session::device::getType(d)); })
    {
        juce::MessageManager::getInstance();
        project.getCompiler().setRenderFormat(48'000.0, 256);
    }

    /** Rebuilds the graph and renders `seconds`, calling `perBlock()` after each block. */
    template <typename PerBlock>
    void render(double seconds, PerBlock&& perBlock)
    {
        project.getCompiler().rebuildNow();
        std::vector<float> l(256), r(256);
        float* channels[2] = {l.data(), r.data()};
        for (int done = 0; done < static_cast<int>(seconds * 48'000.0); done += 256)
        {
            engine.process(nullptr, 0, channels, 2, 256);
            controller.pump();
            perBlock();
        }
    }

    engine::AudioEngine engine{0};
    bool prepared = (engine.prepare(48'000.0, 256, 2), true);
    engine::AudioDeviceHost host{engine};
    engine::EngineController controller{engine, host};
    session::Project project;
};
} // namespace

TEST_CASE("The LFO moves mapped parameters, faders, pans, sends and the master, everywhere", "[devices][lfo][engine]")
{
    test::TempFolder temp;
    LfoRig rig;
    auto& session = rig.project.getSession();
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, 48'000.0, 48'000 * 4, 0.5f), 48'000.0);

    juce::String error;
    REQUIRE(rig.project.getAssets().loadNow(file, error) != nullptr); // (decoded before it plays)
    auto pad = session.addTrack(session::TrackKind::audio, "Pad");
    session.addClip(pad, file, 0.0, 4.0);
    auto other = session.addTrack(session::TrackKind::audio, "Other");
    auto filter = session.addDevice(other, AutoFilter::kTypeId, "Auto Filter");
    auto lfoTree = session.addDevice(pad, LfoDevice::kTypeId, "LFO");
    auto bus = session.addTrack(session::TrackKind::bus, "Reverb Bus");
    auto send = session.addSend(pad, bus, 0.0);

    auto lfoNode = rig.project.getDevices().getNode(lfoTree);
    auto filterNode = rig.project.getDevices().getNode(filter);
    REQUIRE(lfoNode != nullptr);
    REQUIRE(filterNode != nullptr);
    auto& lfo = dynamic_cast<LfoDevice&>(lfoNode->getProcessor());
    setParameter(lfo, "rate", 4.0f);
    auto& autoFilter = dynamic_cast<AutoFilter&>(filterNode->getProcessor());
    const int frequencyIndex = autoFilter.frequency_->getParameterIndex();
    juce::AudioProcessorParameter& frequency = *autoFilter.frequency_;

    // A parameter of a device on another track, swept between 20 % and 80 % of its range.
    const double base = frequency.getValue();
    auto toFilter = session.addModulation(lfoTree, session::track::getId(other),
                                          session::automation::parameterTarget(session::device::getId(filter), frequencyIndex), base);
    REQUIRE(toFilter.isValid());
    session::modulation::setRange(toFilter, 0.2, 0.8, nullptr);
    // (the same target twice is refused)
    CHECK_FALSE(session.addModulation(lfoTree, session::track::getId(other),
                                      session::automation::parameterTarget(session::device::getId(filter), frequencyIndex), base)
                    .isValid());
    CHECK(session.findModulationsOf(0, session::automation::parameterTarget(session::device::getId(filter), frequencyIndex)).size() == 1);

    float lowest = 1.0f, highest = 0.0f;
    rig.render(1.0,
               [&]
               {
                   lowest = std::min(lowest, frequency.getValue());
                   highest = std::max(highest, frequency.getValue());
               });
    CHECK_THAT(lowest, WithinAbs(0.2, 0.01));
    CHECK_THAT(highest, WithinAbs(0.8, 0.01));

    // The track's fader and pan, and the master fader: the engine reports where they are.
    auto toVolume = session.addModulation(lfoTree, session::track::getId(pad), session::automation::volumeTarget(), 0.9);
    auto toPan = session.addModulation(lfoTree, session::track::getId(pad), session::automation::panTarget(), 0.5);
    auto toMaster = session.addModulation(lfoTree, 0, session::automation::volumeTarget(), 0.9);
    session::modulation::setRange(toVolume, 0.5, 0.9, nullptr);
    const int padSlot = rig.project.getCompiler().getSlot(session::track::getId(pad));
    float quietest = 10.0f, loudest = 0.0f, left = 1.0f, right = -1.0f;
    bool masterMoved = false;
    rig.render(1.0,
               [&]
               {
                   const float volume = rig.engine.getMovingVolume(padSlot);
                   const float pan = rig.engine.getMovingPan(padSlot);
                   REQUIRE_FALSE(std::isnan(volume));
                   quietest = std::min(quietest, volume);
                   loudest = std::max(loudest, volume);
                   left = std::min(left, pan);
                   right = std::max(right, pan);
                   masterMoved = masterMoved || !std::isnan(rig.engine.getMovingMasterVolume());
               });
    CHECK_THAT(quietest, WithinAbs(engine::automationToGain(0.5), 0.01));
    CHECK_THAT(loudest, WithinAbs(engine::automationToGain(0.9), 0.02));
    CHECK(left < -0.95f);
    CHECK(right > 0.95f);
    CHECK(masterMoved);

    // A send level: the bus hears the pad come and go with the LFO.
    session.removeModulation(toVolume);
    session.removeModulation(toPan);
    session.removeModulation(toMaster);
    session.addModulation(lfoTree, session::track::getId(pad), session::automation::sendTarget(session::send::getId(send)), 0.9);
    const int busSlot = rig.project.getCompiler().getSlot(session::track::getId(bus));
    rig.controller.play(); // (the clip sounds while playing)
    float busLow = 10.0f, busHigh = 0.0f;
    int blocks = 0;
    rig.render(1.0,
               [&]
               {
                   const auto peak = rig.engine.collectTrackPeak(busSlot);
                   if (++blocks > 10)
                   {
                       busLow = std::min(busLow, peak.left);
                       busHigh = std::max(busHigh, peak.left);
                   }
               });
    CHECK(busHigh > 0.3f);
    CHECK(busLow < 0.02f);

    // Bypassed: the fader goes back to its own setting.
    session.addModulation(lfoTree, session::track::getId(pad), session::automation::volumeTarget(), 0.9);
    session::device::setBypassed(lfoTree, true, nullptr);
    rig.render(0.2, [] {});
    CHECK(std::isnan(rig.engine.getMovingVolume(padSlot)));
    session::device::setBypassed(lfoTree, false, nullptr);

    // Removing the target device removes the mappings to it.
    session.removeDevice(filter);
    CHECK(session.findModulationsOf(0, session::automation::parameterTarget(session::device::getId(filter), frequencyIndex)).empty());
    CHECK(session::Session::getModulations(lfoTree).size() == 2);

    // Saved and loaded with the session.
    const auto path = temp.dir.getChildFile("lfo.dojo");
    REQUIRE(session.save(path).wasOk());
    session::Session loaded;
    REQUIRE(loaded.load(path).wasOk());
    const auto loadedLfo = loaded.findDevice(session::device::getId(lfoTree));
    REQUIRE(loadedLfo.isValid());
    CHECK(session::Session::getModulations(loadedLfo).size() == 2);
}

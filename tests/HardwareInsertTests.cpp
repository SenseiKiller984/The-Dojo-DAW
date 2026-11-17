// HW Insert: outboard gear patched in through the sound card's channels, with its round trip
// measured by a ping and reported for the delay compensation.

#include "core/rt/RealtimeGuard.h"
#include "devices/DeviceRegistry.h"
#include "devices/HardwareInsert.h"
#include "engine/AudioEngine.h"
#include "session/DeviceManager.h"
#include "session/GraphCompiler.h"
#include "session/Project.h"
#include "session/Session.h"
#include "ui/DeviceActions.h"
#include "ui/EditorState.h"
#include "ui/devices/DeviceEditors.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using Catch::Matchers::WithinAbs;
using devices::HardwareInsert;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

void setChoice(juce::AudioParameterChoice& parameter, int index)
{
    parameter.setValueNotifyingHost(parameter.convertTo0to1(static_cast<float>(index)));
}

void setFloat(juce::AudioParameterFloat& parameter, float value)
{
    parameter.setValueNotifyingHost(parameter.convertTo0to1(value));
}

void setBool(juce::AudioParameterBool& parameter, bool value)
{
    parameter.setValueNotifyingHost(value ? 1.0f : 0.0f);
}

/**
    A HW Insert on its own with a simulated sound card of four channels in and out: a cable from
    outputs 3/4 back to inputs 3/4 delays what was sent by a callback plus `extra` samples (and
    scales it by `gain`), as an analog loop through a sound card does.
*/
struct Loopback
{
    explicit Loopback(HardwareInsert& device, int extraDelay = 37, float loopGain = 1.0f, bool connected = true)
        : insert(device)
        , extra(extraDelay)
        , gain(loopGain)
        , cable(connected)
    {
        for (auto& h : history)
            h.reserve(1 << 20);
    }

    /** One callback with `input` on the track; returns the device's output (left). */
    std::vector<double> run(double input = 0.0, bool live = true, int reported = 0)
    {
        const auto at = static_cast<std::int64_t>(history[0].size());
        std::array<std::vector<double>, 4> in;
        for (auto& channel : in)
            channel.assign(kBlock, 0.0);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                if (const auto source = at + i - kBlock - extra; cable && source >= 0)
                    in[static_cast<std::size_t>(2 + c)][static_cast<std::size_t>(i)] = gain * history[static_cast<std::size_t>(c)][static_cast<std::size_t>(source)];
        const double* inputs[4] = {in[0].data(), in[1].data(), in[2].data(), in[3].data()};
        HardwarePorts ports;
        ports.inputs = live ? inputs : nullptr;
        ports.numInputs = live ? 4 : 0;
        ports.numOutputs = live ? 4 : 0;
        ports.bufferSize = kBlock;
        ports.reportedLatency = reported;
        ports.live = live;
        insert.setHardwarePorts(ports);

        juce::AudioBuffer<double> buffer(2, kBlock);
        for (int i = 0; i < kBlock; ++i)
        {
            buffer.setSample(0, i, input);
            buffer.setSample(1, i, input);
        }
        juce::MidiBuffer midi;
        insert.processBlock(buffer, midi);

        std::array<std::vector<float>, 4> out;
        for (auto& channel : out)
            channel.assign(kBlock, 0.0f);
        float* outputs[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
        insert.addSends(outputs, 4, 0, kBlock);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                history[static_cast<std::size_t>(c)].push_back(static_cast<double>(out[static_cast<std::size_t>(2 + c)][static_cast<std::size_t>(i)]));
        lastSend = out;
        return {buffer.getReadPointer(0), buffer.getReadPointer(0) + kBlock};
    }

    /** Runs until a measurement has been recorded, then lets the device analyse it. */
    bool measure()
    {
        for (int block = 0; block < 400 && insert.getPingState() != HardwareInsert::PingState::analysing; ++block)
            (void) run();
        if (insert.getPingState() != HardwareInsert::PingState::analysing)
            return false;
        insert.service();
        return insert.getPingState() == HardwareInsert::PingState::idle;
    }

    HardwareInsert& insert;
    int extra;
    float gain;
    bool cable;
    std::array<std::vector<double>, 2> history;
    std::array<std::vector<float>, 4> lastSend;
};

std::unique_ptr<HardwareInsert> makeInsert(bool autoPing = false)
{
    auto insert = std::make_unique<HardwareInsert>();
    setBool(*insert->autoPing_, autoPing);
    insert->prepareToPlay(kRate, kBlock);
    return insert;
}
} // namespace

TEST_CASE("Loopback measurement: finds the ping's round trip, gain and polarity, and nothing in noise", "[devices][hwinsert]")
{
    const auto ping = devices::loopback::makePing(kRate, 0.25f);
    CHECK(ping.size() == 7200);
    CHECK_THAT(*std::max_element(ping.begin(), ping.end()), WithinAbs(0.25, 0.01));

    juce::Random random(42);
    for (const auto& [delay, gain] : std::vector<std::pair<int, float>>{{0, 1.0f}, {37, 0.5f}, {293, -1.0f}, {1234, 2.0f}, {20000, -0.25f}})
    {
        INFO("delay " << delay << ", gain " << gain);
        std::vector<float> recorded(ping.size() + 24000, 0.0f);
        for (std::size_t i = 0; i < ping.size(); ++i)
            recorded[static_cast<std::size_t>(delay) + i] += gain * ping[i];
        for (auto& v : recorded)
            v += 0.003f * (random.nextFloat() * 2.0f - 1.0f); // noise at about -50 dBFS
        const auto m = devices::loopback::measure(ping, recorded.data(), static_cast<int>(recorded.size()));
        CHECK(m.found);
        CHECK(m.latency == delay);
        CHECK_THAT(m.gainDb, WithinAbs(20.0 * std::log10(std::abs(gain)), 0.2));
        CHECK(m.inverted == (gain < 0.0f));
        CHECK(m.clarityDb > devices::loopback::kMinClarityDb);
        CHECK(m.correlation.size() == 512);
        CHECK(*std::max_element(m.correlation.begin(), m.correlation.end()) == 1.0f);
    }

    // Through a filter (a one-pole low-pass at 5 kHz): within a sample or two.
    {
        std::vector<float> recorded(ping.size() + 24000, 0.0f);
        for (std::size_t i = 0; i < ping.size(); ++i)
            recorded[700 + i] = ping[i];
        const float a = std::exp(-2.0f * juce::MathConstants<float>::pi * 5000.0f / static_cast<float>(kRate));
        float state = 0.0f;
        for (auto& v : recorded)
            v = state = (1.0f - a) * v + a * state;
        const auto m = devices::loopback::measure(ping, recorded.data(), static_cast<int>(recorded.size()));
        CHECK(m.found);
        CHECK(std::abs(m.latency - 700) <= 2);
    }

    // Nothing came back, or only noise.
    std::vector<float> silence(ping.size() + 24000, 0.0f);
    const auto none = devices::loopback::measure(ping, silence.data(), static_cast<int>(silence.size()));
    CHECK_FALSE(none.found);
    CHECK(none.gainDb <= devices::loopback::kMinGainDb);
    std::vector<float> noise(ping.size() + 24000);
    for (auto& v : noise)
        v = 0.5f * (random.nextFloat() * 2.0f - 1.0f);
    CHECK_FALSE(devices::loopback::measure(ping, noise.data(), static_cast<int>(noise.size())).found);
}

TEST_CASE("HW Insert: channel choices", "[devices][hwinsert]")
{
    const auto& choices = HardwareInsert::channelChoices();
    REQUIRE(choices.size() == 97);
    CHECK(choices[0] == "None");
    CHECK(choices[2] == "3/4");
    CHECK(choices[32] == "63/64");
    CHECK(choices[33] == "1");
    CHECK(choices[96] == "64");
    CHECK(HardwareInsert::channelsOf(0) == HardwareInsert::Channels{});
    CHECK(HardwareInsert::channelsOf(2) == HardwareInsert::Channels{2, 2});
    CHECK(HardwareInsert::channelsOf(37) == HardwareInsert::Channels{4, 1});
    for (int choice = 0; choice < choices.size(); ++choice)
        CHECK(HardwareInsert::choiceOf(HardwareInsert::channelsOf(choice)) == choice);

    // By default: out to 3/4 and back from 3/4, all of it, measured by itself.
    HardwareInsert insert;
    CHECK(insert.getSendChannels() == HardwareInsert::Channels{2, 2});
    CHECK(insert.getReturnChannels() == HardwareInsert::Channels{2, 2});
    CHECK(insert.mix_->get() == 100.0f);
    CHECK(insert.autoPing_->get());
    CHECK_FALSE(insert.send_->isAutomatable());
    CHECK_FALSE(insert.offset_->isAutomatable());
}

TEST_CASE("HW Insert: sends to the sound card, returns from it, mixes, inverts, and passes through without it", "[devices][hwinsert]")
{
    auto insert = makeInsert();
    Loopback loop(*insert, 0, 1.0f, false); // (no cable: the return is set by hand below)

    // A return of 0.25 / -0.5 on inputs 3/4 replaces the signal; the track goes out on 3/4.
    const auto runWith = [&](double left, double right, double returnLeft, double returnRight, int blocks = 1)
    {
        std::vector<double> in0(kBlock, 0.0), in1(kBlock, 0.0), in2(kBlock, returnLeft), in3(kBlock, returnRight);
        const double* inputs[4] = {in0.data(), in1.data(), in2.data(), in3.data()};
        juce::AudioBuffer<double> buffer(2, kBlock);
        std::array<std::vector<float>, 4> out;
        for (int b = 0; b < blocks; ++b)
        {
            HardwarePorts ports;
            ports.inputs = inputs;
            ports.numInputs = 4;
            ports.numOutputs = 4;
            ports.bufferSize = kBlock;
            ports.live = true;
            insert->setHardwarePorts(ports);
            for (int i = 0; i < kBlock; ++i)
            {
                buffer.setSample(0, i, left);
                buffer.setSample(1, i, right);
            }
            juce::MidiBuffer midi;
            insert->processBlock(buffer, midi);
            for (auto& channel : out)
                channel.assign(kBlock, 0.0f);
            float* outputs[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
            insert->addSends(outputs, 4, 0, kBlock);
        }
        return std::make_pair(std::make_pair(buffer.getSample(0, kBlock - 1), buffer.getSample(1, kBlock - 1)), out);
    };

    auto [output, sent] = runWith(0.1, 0.2, 0.25, -0.5);
    CHECK_THAT(output.first, WithinAbs(0.25, 1e-9));
    CHECK_THAT(output.second, WithinAbs(-0.5, 1e-9));
    CHECK_THAT(sent[2][100], WithinAbs(0.1, 1e-7));
    CHECK_THAT(sent[3][100], WithinAbs(0.2, 1e-7));
    CHECK(sent[0][100] == 0.0f);
    CHECK(sent[1][100] == 0.0f);
    // Sent once: a second call adds nothing.
    std::vector<float> extra(kBlock, 0.0f);
    float* outputs[4] = {extra.data(), extra.data(), extra.data(), extra.data()};
    insert->addSends(outputs, 4, 0, kBlock);
    CHECK(extra[100] == 0.0f);

    // Send and Return levels, Phase.
    setFloat(*insert->sendGain_, -6.0f);
    setFloat(*insert->returnGain_, 6.0f);
    setBool(*insert->phase_, true);
    std::tie(output, sent) = runWith(0.1, 0.2, 0.25, -0.5, 40);
    CHECK_THAT(sent[2][100], WithinAbs(0.1 * 0.501187, 1e-4));
    CHECK_THAT(output.first, WithinAbs(-0.25 * 1.995262, 1e-3));
    setFloat(*insert->sendGain_, 0.0f);
    setFloat(*insert->returnGain_, 0.0f);
    setBool(*insert->phase_, false);

    // Dry/Wet: half the signal as it was, half the return.
    setFloat(*insert->mix_, 50.0f);
    std::tie(output, sent) = runWith(0.1, 0.2, 0.25, -0.5, 40);
    CHECK_THAT(output.first, WithinAbs(0.5 * 0.1 + 0.5 * 0.25, 1e-3));
    setFloat(*insert->mix_, 100.0f);

    // A single channel each way: the send is the mono sum; the return feeds both sides.
    setChoice(*insert->send_, HardwareInsert::choiceOf({2, 1}));
    setChoice(*insert->return_, HardwareInsert::choiceOf({3, 1}));
    std::tie(output, sent) = runWith(0.1, 0.2, 0.25, -0.5, 40);
    CHECK_THAT(sent[2][100], WithinAbs(0.15, 1e-7));
    CHECK(sent[3][100] == 0.0f);
    CHECK_THAT(output.first, WithinAbs(-0.5, 1e-6));
    CHECK_THAT(output.second, WithinAbs(-0.5, 1e-6));

    // A return the sound card does not have: the signal passes through (the send still goes out).
    setChoice(*insert->send_, HardwareInsert::choiceOf({2, 2}));
    setChoice(*insert->return_, HardwareInsert::choiceOf({4, 2}));
    std::tie(output, sent) = runWith(0.1, 0.2, 0.25, -0.5, 4);
    CHECK_THAT(output.first, WithinAbs(0.1, 1e-9));
    CHECK_THAT(output.second, WithinAbs(0.2, 1e-9));
    CHECK_THAT(sent[2][100], WithinAbs(0.1, 1e-7));

    // No sound card (an offline render, or never given the ports): it passes through and sends nothing.
    setChoice(*insert->return_, HardwareInsert::choiceOf({2, 2}));
    juce::AudioBuffer<double> buffer(2, kBlock);
    for (int i = 0; i < kBlock; ++i)
    {
        buffer.setSample(0, i, 0.3);
        buffer.setSample(1, i, 0.4);
    }
    juce::MidiBuffer midi;
    insert->processBlock(buffer, midi);
    CHECK_THAT(buffer.getSample(0, 200), WithinAbs(0.3, 1e-9));
    std::fill(extra.begin(), extra.end(), 0.0f);
    insert->addSends(outputs, 4, 0, kBlock);
    CHECK(extra[100] == 0.0f);
}

TEST_CASE("HW Insert: PING measures the round trip, sets Phase, and Auto Ping measures by itself once", "[devices][hwinsert]")
{
    SECTION("PING")
    {
        auto insert = makeInsert();
        Loopback loop(*insert, 37, -0.5f);
        (void) loop.run(0.2);
        insert->service();
        // Not measured yet: the driver's figure, or two buffers.
        CHECK(insert->getLatencySamples() == 2 * kBlock);
        (void) loop.run(0.2, true, 300);
        insert->service();
        CHECK(insert->getLatencySamples() == 300);

        insert->ping();
        CHECK(insert->getPingState() == HardwareInsert::PingState::waiting);
        CHECK(insert->getMessage() == "Measuring the round trip...");
        // While it pings the device is silent and the sweep goes out on both sides.
        for (int block = 0; block < 20; ++block)
            (void) loop.run(0.2);
        CHECK(insert->getPingState() == HardwareInsert::PingState::running);
        const auto quiet = loop.run(0.2);
        CHECK(*std::max_element(quiet.begin(), quiet.end()) < 1e-9);
        CHECK(std::abs(loop.lastSend[2][100]) > 0.0f);
        CHECK(loop.lastSend[2][100] == loop.lastSend[3][100]);

        REQUIRE(loop.measure());
        const auto& calibration = insert->getCalibration();
        REQUIRE(calibration.valid);
        CHECK(calibration.latency == kBlock + 37);
        CHECK(calibration.inverted);
        CHECK_THAT(calibration.gainDb, WithinAbs(-6.02, 0.1));
        CHECK(calibration.bufferSize == kBlock);
        CHECK(insert->isCalibrationCurrent());
        CHECK(insert->getLatencySamples() == kBlock + 37);
        CHECK(insert->phase_->get()); // the hardware inverts: Phase turns it back
        CHECK(insert->getMessage().startsWith("Round trip 293 samples (6.10 ms)"));
        CHECK(insert->getCorrelation().size() == 512);
        CHECK(insert->getCorrelationLags() == 4 * (kBlock + 37)); // (the peak a quarter of the way in)

        // MATCH: Return makes up the loop's 6 dB.
        insert->matchLevels();
        CHECK_THAT(insert->returnGain_->get(), WithinAbs(6.02, 0.1));

        // After the ping the signal comes back (through the loop: delayed, inverted twice, matched).
        std::vector<double> out;
        for (int block = 0; block < 30; ++block)
            out = loop.run(0.2);
        CHECK_THAT(out.back(), WithinAbs(0.2, 0.01));

        // Offset nudges the delay compensation.
        setFloat(*insert->offset_, 12.0f);
        insert->service();
        CHECK(insert->getLatencySamples() == kBlock + 37 + 12);

        // Another buffer size: the measurement no longer applies; the driver's figure stands in.
        setFloat(*insert->offset_, 0.0f);
        (void) loop.run(0.2, true, 300);
        insert->service();
        CHECK(insert->getLatencySamples() == kBlock + 37);
        insert->prepareToPlay(kRate, 512);
        HardwarePorts ports;
        std::vector<double> silent(512, 0.0);
        const double* inputs[4] = {silent.data(), silent.data(), silent.data(), silent.data()};
        ports.inputs = inputs;
        ports.numInputs = ports.numOutputs = 4;
        ports.bufferSize = 512;
        ports.reportedLatency = 900;
        ports.live = true;
        insert->setHardwarePorts(ports);
        juce::AudioBuffer<double> buffer(2, 512);
        juce::MidiBuffer midi;
        insert->processBlock(buffer, midi);
        insert->service();
        CHECK_FALSE(insert->isCalibrationCurrent());
        CHECK(insert->getLatencySamples() == 900);
    }

    SECTION("The measurement is saved with the device")
    {
        auto insert = makeInsert();
        Loopback loop(*insert, 100, 1.0f);
        (void) loop.run();
        insert->ping();
        REQUIRE(loop.measure());
        CHECK(insert->getLatencySamples() == kBlock + 100);
        juce::MemoryBlock state;
        insert->getStateInformation(state);

        HardwareInsert restored;
        restored.prepareToPlay(kRate, kBlock);
        restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
        CHECK(restored.getCalibration().valid);
        CHECK(restored.getCalibration().latency == kBlock + 100);
        CHECK(restored.getLatencySamples() == kBlock + 100); // (before the sound card is seen)
    }

    SECTION("Auto Ping")
    {
        auto insert = makeInsert(true);
        Loopback loop(*insert, 10, 0.8f);
        (void) loop.run(0.1);
        insert->service(); // running, live, stopped, not measured: it pings
        CHECK(insert->getPingState() == HardwareInsert::PingState::waiting);
        REQUIRE(loop.measure());
        CHECK(insert->getCalibration().latency == kBlock + 10);
        CHECK_FALSE(insert->getCalibration().inverted);
        CHECK_FALSE(insert->phase_->get());
        (void) loop.run(0.1);
        insert->service();
        CHECK(insert->getPingState() == HardwareInsert::PingState::idle); // measured: no more pings

        // New channels: it measures again by itself.
        setChoice(*insert->return_, HardwareInsert::choiceOf({2, 1}));
        (void) loop.run(0.1);
        insert->service();
        CHECK(insert->getPingState() == HardwareInsert::PingState::waiting);
        REQUIRE(loop.measure());
        CHECK(insert->getCalibration().ret == HardwareInsert::choiceOf({2, 1}));
    }

    SECTION("Nothing comes back: said once, not retried by itself")
    {
        auto insert = makeInsert(true);
        Loopback loop(*insert, 10, 1.0f, false);
        (void) loop.run();
        insert->service();
        REQUIRE(loop.measure());
        CHECK_FALSE(insert->getCalibration().valid);
        CHECK(insert->getMessage().startsWith("Nothing came back on input 3/4"));
        for (int i = 0; i < 5; ++i)
        {
            (void) loop.run();
            insert->service();
        }
        CHECK(insert->getPingState() == HardwareInsert::PingState::idle);
        // PING asked for by hand still runs.
        insert->ping();
        CHECK(insert->getPingState() == HardwareInsert::PingState::waiting);
        // Pressing it again while it waits cancels it.
        insert->ping();
        CHECK(insert->getPingState() == HardwareInsert::PingState::idle);
    }

    SECTION("Channels the sound card lacks")
    {
        auto insert = makeInsert();
        setChoice(*insert->send_, HardwareInsert::choiceOf({6, 2}));
        Loopback loop(*insert);
        insert->ping();
        (void) loop.run();
        CHECK(insert->getPingState() == HardwareInsert::PingState::unavailable);
        insert->service();
        CHECK(insert->getPingState() == HardwareInsert::PingState::idle);
        CHECK(insert->getMessage().startsWith("Can't measure: the sound card has 4 outputs and 4 inputs"));
    }
}

TEST_CASE("HW Insert in the engine: the sound card's channels both ways, and the return in time with the rest", "[engine][hwinsert]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(220.0, kRate, 96000, 0.4f), kRate);
    engine::AudioEngine engine(2);
    session::Session session;
    session::AudioAssetCache assets;
    session::DeviceManager devices(session, [](const juce::ValueTree& d, double, int, juce::String&)
                                   { return devices::createBuiltinDevice(session::device::getType(d)); });
    session::GraphBuilder builder;
    engine.prepare(kRate, kBlock, 4, 4); // a sound card with four outputs and four inputs
    devices.setPlayHead(engine.getPlayHead());
    devices.prepareAll(kRate, kBlock);
    builder.setRenderFormat(kRate, kBlock);
    juce::String error;
    auto asset = assets.loadNow(file, error);
    REQUIRE(asset != nullptr);

    // Track A goes through the hardware; track B plays the same, upside down: in time, they cancel.
    auto a = session.addTrack();
    session.addClip(a, file, 0.0, asset->getLengthSeconds());
    auto b = session.addTrack();
    session.addClip(b, file, 0.0, asset->getLengthSeconds());
    session::track::setPhaseInverted(b, true, nullptr);
    auto device = session.addDevice(a, HardwareInsert::kTypeId, "HW Insert");
    auto node = devices.getNode(device);
    REQUIRE(node != nullptr);
    REQUIRE(node->getHardwareIo() != nullptr);
    auto& insert = dynamic_cast<HardwareInsert&>(node->getProcessor());
    setBool(*insert.autoPing_, false);

    std::uint64_t serial = 0;
    const auto swap = [&]
    {
        auto graph = builder.build(session, assets, &devices);
        CHECK(graph->hardwareIo.size() == 1);
        graph->serial = ++serial;
        engine::command::SwapGraph command;
        command.graph = graph.release();
        REQUIRE(engine.post(command));
    };
    swap();

    // The cable: outputs 3/4 back into inputs 3/4, a callback plus 37 samples later.
    constexpr int kExtra = 37;
    std::array<std::vector<float>, 2> cable;
    std::vector<float> master;
    const auto callback = [&]
    {
        const auto at = static_cast<std::int64_t>(cable[0].size());
        std::array<std::vector<float>, 4> in, out;
        for (auto& c : in)
            c.assign(kBlock, 0.0f);
        for (auto& c : out)
            c.assign(kBlock, 0.0f);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < kBlock; ++i)
                if (const auto source = at + i - kBlock - kExtra; source >= 0)
                    in[static_cast<std::size_t>(2 + c)][static_cast<std::size_t>(i)] = cable[static_cast<std::size_t>(c)][static_cast<std::size_t>(source)];
        const float* inputs[4] = {in[0].data(), in[1].data(), in[2].data(), in[3].data()};
        float* outputs[4] = {out[0].data(), out[1].data(), out[2].data(), out[3].data()};
        engine.process(inputs, 4, outputs, 4, kBlock);
        engine.releaseRetiredGraphs();
        for (int c = 0; c < 2; ++c)
            cable[static_cast<std::size_t>(c)].insert(cable[static_cast<std::size_t>(c)].end(), out[static_cast<std::size_t>(2 + c)].begin(),
                                                      out[static_cast<std::size_t>(2 + c)].end());
        master.insert(master.end(), out[0].begin(), out[0].end());
    };

    // Stopped: PING finds the round trip through the engine.
    callback();
    insert.ping();
    for (int i = 0; i < 400 && insert.getPingState() != HardwareInsert::PingState::analysing; ++i)
        callback();
    insert.service();
    REQUIRE(insert.getCalibration().valid);
    CHECK(insert.getCalibration().latency == kBlock + kExtra);
    CHECK(insert.getLatencySamples() == kBlock + kExtra);

    // The delay compensation now holds track B back by the round trip: the two cancel.
    const auto residual = [&](int offset)
    {
        setFloat(*insert.offset_, static_cast<float>(offset));
        insert.service();
        swap();
        REQUIRE(engine.post(engine::command::Locate{0}));
        REQUIRE(engine.post(engine::command::SetPlaying{true}));
        master.clear();
        for (int i = 0; i < 160; ++i)
            callback();
        REQUIRE(engine.post(engine::command::SetPlaying{false}));
        callback();
        double sum = 0.0;
        for (std::size_t i = 4800; i < 38400; ++i) // (0.1 s .. 0.8 s)
            sum += static_cast<double>(master[i]) * master[i];
        return std::sqrt(sum / 33600.0);
    };
    CHECK(residual(0) < 1.0e-3);
    CHECK(residual(24) > 0.05); // misreported by half a millisecond: they no longer cancel
}

TEST_CASE("HW Insert: the editor names the sound card's channels, warns, and pings", "[ui][hwinsert]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); });
    auto& session = project.getSession();
    ui::EditorState editorState;
    auto track = session.addTrack(session::TrackKind::audio, "Synth");
    auto device = session.addDevice(track, HardwareInsert::kTypeId, "HW Insert");
    auto node = project.getDevices().getNode(device);
    REQUIRE(node != nullptr);
    auto& insert = dynamic_cast<HardwareInsert&>(node->getProcessor());
    setBool(*insert.autoPing_, false);
    insert.prepareToPlay(kRate, kBlock);

    ui::DeviceEditorContext context{project, editorState, device, track};
    context.channelNames = [](bool outputs)
    {
        return outputs ? juce::StringArray{"Main L", "Main R", "Line 3", "Line 4"} : juce::StringArray{"Mic 1", "Mic 2", "Line In 3", "Line In 4"};
    };
    int width = 0;
    auto editor = ui::createDeviceEditor(*node, context, width);
    REQUIRE(editor != nullptr);
    CHECK(width == 740);
    editor->setSize(width, 220);

    auto* send = dynamic_cast<juce::ComboBox*>(editor->findChildWithID("send"));
    auto* ret = dynamic_cast<juce::ComboBox*>(editor->findChildWithID("return"));
    REQUIRE(send != nullptr);
    REQUIRE(ret != nullptr);
    CHECK(send->getText() == "3/4   Line 3 + Line 4");
    CHECK(ret->getText() == "3/4   Line In 3 + Line In 4");
    CHECK(send->getNumItems() == 1 + 2 + 4); // None, two pairs, four single channels
    auto* readout = dynamic_cast<juce::Label*>(editor->findChildWithID("readout"));
    auto* source = dynamic_cast<juce::Label*>(editor->findChildWithID("source"));
    REQUIRE(readout != nullptr);
    REQUIRE(source != nullptr);
    CHECK(readout->getText().endsWith("ms"));
    CHECK(source->getText().contains("not measured yet"));

    // Picking a single channel sets the parameter; one the card lacks is marked and explained.
    send->setSelectedId(HardwareInsert::choiceOf({3, 1}) + 1, juce::sendNotificationSync);
    CHECK(insert.getSendChannels() == HardwareInsert::Channels{3, 1});
    setChoice(*insert.return_, HardwareInsert::choiceOf({4, 2}));
    (void) editor->createComponentSnapshot(editor->getLocalBounds()); // (paints; the timer refreshes below)
    auto* warning = dynamic_cast<juce::Label*>(editor->findChildWithID("returnWarning"));
    REQUIRE(warning != nullptr);
    for (int i = 0; i < 2; ++i)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
    CHECK(warning->getText().startsWith("5/6 isn't on this sound card (4 inputs)"));
    CHECK(ret->getText().endsWith("(not on this sound card)"));

    // PING asks the device to measure.
    auto* ping = dynamic_cast<juce::TextButton*>(editor->findChildWithID("ping"));
    REQUIRE(ping != nullptr);
    ping->triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    CHECK(insert.getPingState() == HardwareInsert::PingState::waiting);
    CHECK(ping->getButtonText() == "CANCEL");
    (void) editor->createComponentSnapshot(editor->getLocalBounds());
}

TEST_CASE("HW Insert: projects with one bounce in real time; processing never touches the heap", "[devices][hwinsert]")
{
    session::Session session;
    auto track = session.addTrack();
    CHECK_FALSE(ui::deviceActions::usesHardwareInserts(session));
    auto device = session.addDevice(track, HardwareInsert::kTypeId, "HW Insert");
    CHECK(ui::deviceActions::usesHardwareInserts(session));
    session::device::setBypassed(device, true, nullptr);
    CHECK_FALSE(ui::deviceActions::usesHardwareInserts(session));
    session::device::setBypassed(device, false, nullptr);
    session::track::setActive(track, false, nullptr);
    CHECK_FALSE(ui::deviceActions::usesHardwareInserts(session));

    auto insert = makeInsert();
    Loopback loop(*insert, 37, 1.0f);
    (void) loop.run(0.1);
    const auto before = rt::getRealtimeViolationCount();
    {
        // (the loop's own bookkeeping allocates: only the device is checked)
        std::vector<double> in(kBlock, 0.0);
        const double* inputs[4] = {in.data(), in.data(), in.data(), in.data()};
        std::vector<float> out(kBlock, 0.0f);
        float* outputs[4] = {out.data(), out.data(), out.data(), out.data()};
        juce::AudioBuffer<double> buffer(2, kBlock);
        juce::MidiBuffer midi;
        insert->ping();
        const rt::ScopedRealtimeSection section;
        for (int block = 0; block < 200; ++block)
        {
            HardwarePorts ports;
            ports.inputs = inputs;
            ports.numInputs = ports.numOutputs = 4;
            ports.bufferSize = kBlock;
            ports.live = true;
            insert->setHardwarePorts(ports);
            insert->processBlock(buffer, midi);
            insert->addSends(outputs, 4, 0, kBlock);
        }
    }
    CHECK(rt::getRealtimeViolationCount() == before);
}

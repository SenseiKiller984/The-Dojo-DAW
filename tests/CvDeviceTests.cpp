// CV/Gate: the CV devices' voltages on the sound card's outputs (pitch, gate, velocity, mod, glide,
// priorities, delay), calibration against an oscillator, triggers, clock, utility, CV In, editors.

#include "devices/CvDevices.h"
#include "devices/DeviceRegistry.h"
#include "devices/HardwareInsert.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/Project.h"
#include "ui/EditorState.h"
#include "ui/devices/CvEditors.h"
#include "ui/devices/MoreDeviceEditors.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>

using namespace dojo;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 48000.0;
constexpr int kBlock = 256;

/** A sound card for the devices: outputs to read the voltages from, inputs to feed. */
struct Card
{
    static constexpr int kChannels = 8;
    std::vector<std::vector<float>> outputs = std::vector<std::vector<float>>(kChannels, std::vector<float>(kBlock, 0.0f));
    std::vector<std::vector<Sample>> inputs = std::vector<std::vector<Sample>>(kChannels, std::vector<Sample>(kBlock, 0.0));
    std::array<float*, kChannels> outPointers{};
    std::array<const Sample*, kChannels> inPointers{};
    bool live = true;

    /** One block through a device: its ports given, processed, its sends added to the outputs. */
    template <typename Device>
    void run(Device& device, juce::MidiBuffer midi = {}, int numSamples = kBlock)
    {
        for (int c = 0; c < kChannels; ++c)
        {
            std::fill(outputs[static_cast<std::size_t>(c)].begin(), outputs[static_cast<std::size_t>(c)].end(), 0.0f);
            outPointers[static_cast<std::size_t>(c)] = outputs[static_cast<std::size_t>(c)].data();
            inPointers[static_cast<std::size_t>(c)] = inputs[static_cast<std::size_t>(c)].data();
        }
        HardwarePorts ports;
        ports.live = live;
        ports.inputs = live ? inPointers.data() : nullptr;
        ports.numInputs = live ? kChannels : 0;
        ports.numOutputs = live ? kChannels : 0;
        ports.bufferSize = kBlock;
        device.setHardwarePorts(ports);
        juce::AudioBuffer<float> buffer(2, numSamples);
        buffer.clear();
        device.processBlock(buffer, midi);
        device.addSends(outPointers.data(), kChannels, 0, numSamples);
        audio = buffer;
    }
    [[nodiscard]] float volts(int channel, int sample, double fullScale = 10.0) const
    {
        return static_cast<float>(outputs[static_cast<std::size_t>(channel)][static_cast<std::size_t>(sample)] * fullScale);
    }
    juce::AudioBuffer<float> audio;
};

void setChoice(juce::AudioParameterChoice* p, int index) { p->setValueNotifyingHost(p->convertTo0to1(static_cast<float>(index))); }
void setFloat(juce::AudioParameterFloat* p, float value) { p->setValueNotifyingHost(p->convertTo0to1(value)); }

juce::MidiBuffer midiOf(std::initializer_list<std::pair<int, juce::MidiMessage>> events)
{
    juce::MidiBuffer m;
    for (const auto& [at, message] : events)
        m.addEvent(message, at);
    return m;
}

/** A transport for the clock. */
struct TestPlayHead final : public juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo p;
        p.setIsPlaying(playing);
        p.setPpqPosition(ppq);
        p.setBpm(bpm);
        return p;
    }
    bool playing = true;
    double ppq = 0.0;
    double bpm = 120.0;
};
} // namespace

TEST_CASE("CV Instrument: notes to 1 V/octave pitch, gate, velocity and mod voltages on the chosen outputs", "[cv]")
{
    devices::CvInstrument cv;
    cv.prepareToPlay(kRate, kBlock);
    Card card;
    // Pitch on output 3, gate on 4, velocity on 5, mod on 6.
    setChoice(cv.getPortChoice(devices::CvInstrument::pitchPort), 3);
    setChoice(cv.getPortChoice(devices::CvInstrument::gatePort), 4);
    setChoice(cv.getPortChoice(devices::CvInstrument::velocityPort), 5);
    setChoice(cv.getPortChoice(devices::CvInstrument::modPort), 6);
    REQUIRE(cv.getNumPorts() == 4);
    CHECK(cv.getPortName(0) == "Pitch");
    CHECK_THAT(cv.zeroNote_->get(), WithinAbs(36.0, 1e-6));

    // C4 (60) from sample 10: two octaves over the 0 V note (C2): 2 V; the gate opens at 5 V.
    card.run(cv, midiOf({{10, juce::MidiMessage::noteOn(1, 60, 1.0f)}, {20, juce::MidiMessage::controllerEvent(1, 1, 127)}}));
    CHECK(card.volts(3, 5) == 0.0f);
    CHECK_THAT(card.volts(2, 100), WithinAbs(2.0, 1e-5));
    CHECK_THAT(card.volts(3, 100), WithinAbs(5.0, 1e-5));
    CHECK_THAT(card.volts(4, 100), WithinAbs(5.0, 1e-4)); // velocity 1: the full 5 V
    CHECK_THAT(card.volts(5, 100), WithinAbs(5.0, 1e-4)); // mod wheel up
    CHECK(card.volts(0, 100) == 0.0f);                    // (other outputs untouched)
    CHECK_THAT(cv.getPortVolts(0), WithinAbs(2.0, 1e-5));
    CHECK(cv.getCurrentNote() == 60);

    // Off: the gate closes, the pitch stays (for the release).
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOff(1, 60)}}));
    CHECK(card.volts(3, 10) == 0.0f);
    CHECK_THAT(card.volts(2, 10), WithinAbs(2.0, 1e-5));

    // Transpose, tracking and offset trims, bend.
    setFloat(cv.transpose_, 12.0f);
    setFloat(cv.tracking_, 105.0f);
    setFloat(cv.offset_, 50.0f);
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, 60, 0.5f)}}));
    CHECK_THAT(card.volts(2, 50), WithinAbs((72.0 - 36.0) / 12.0 * 1.05 + 0.05, 1e-4));
    CHECK_THAT(card.volts(4, 50), WithinAbs(64.0 / 127.0 * 5.0, 1e-3)); // velocity 0.5 (64 in MIDI)
    setFloat(cv.transpose_, 0.0f);
    setFloat(cv.tracking_, 100.0f);
    setFloat(cv.offset_, 0.0f);
    card.run(cv, midiOf({{0, juce::MidiMessage::pitchWheel(1, 16383)}}));
    CHECK_THAT(card.volts(2, 50), WithinAbs(2.0 + 2.0 / 12.0 * 16383.0 / 16384.0 * 8192.0 / 8192.0, 2e-3)); // +2 semitones
    card.run(cv, midiOf({{0, juce::MidiMessage::pitchWheel(1, 8192)}, {1, juce::MidiMessage::noteOff(1, 60)}}));

    // Priorities: last, lowest, highest.
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, 60, 1.0f)}, {1, juce::MidiMessage::noteOn(1, 67, 1.0f)}}));
    CHECK(cv.getCurrentNote() == 67);
    setChoice(cv.priority_, 1);
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, 64, 1.0f)}}));
    CHECK(cv.getCurrentNote() == 60);
    setChoice(cv.priority_, 2);
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, 62, 1.0f)}}));
    CHECK(cv.getCurrentNote() == 67);
    // Releasing the sounding note goes back to one still held (retriggered: a 2 ms gap in the gate).
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOff(1, 67)}}));
    CHECK(cv.getCurrentNote() == 64);
    CHECK(card.volts(3, 20) == 0.0f);
    CHECK_THAT(card.volts(3, 200), WithinAbs(5.0, 1e-4));
    card.run(cv, midiOf({{0, juce::MidiMessage::allNotesOff(1)}}));
    CHECK(cv.getCurrentNote() == -1);
    setChoice(cv.priority_, 0);

    // Glide: 100 ms from C4 to C5, halfway at 50 ms.
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, 60, 1.0f)}}));
    setFloat(cv.glide_, 100.0f);
    juce::MidiBuffer up = midiOf({{0, juce::MidiMessage::noteOn(1, 72, 1.0f)}});
    std::vector<float> pitch;
    for (int b = 0; b < 30; ++b)
    {
        card.run(cv, b == 0 ? up : juce::MidiBuffer{});
        for (int i = 0; i < kBlock; ++i)
            pitch.push_back(card.volts(2, i));
    }
    CHECK(pitch[static_cast<std::size_t>(0.05 * kRate)] > 2.4f);
    CHECK(pitch[static_cast<std::size_t>(0.05 * kRate)] < 2.6f);
    CHECK_THAT(pitch[static_cast<std::size_t>(0.12 * kRate)], WithinAbs(3.0, 1e-4));
    setFloat(cv.glide_, 0.0f);
    card.run(cv, midiOf({{0, juce::MidiMessage::allNotesOff(1)}}));

    // Delay: the voltages 10 ms later.
    setFloat(cv.delay_, 10.0f);
    std::vector<float> gate;
    for (int b = 0; b < 4; ++b)
    {
        card.run(cv, b == 0 ? midiOf({{0, juce::MidiMessage::noteOn(1, 48, 1.0f)}}) : juce::MidiBuffer{});
        for (int i = 0; i < kBlock; ++i)
            gate.push_back(card.volts(3, i));
    }
    CHECK(gate[470] == 0.0f);
    CHECK_THAT(gate[490], WithinAbs(5.0, 1e-4));
    setFloat(cv.delay_, 0.0f);

    // Full scale: a 5 V interface puts 2 V at 0.4.
    setFloat(cv.fullScale_, 5.0f);
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, 60, 1.0f)}}));
    CHECK_THAT(card.outputs[2][100], WithinAbs(0.4, 1e-5));
    setFloat(cv.fullScale_, 10.0f);

    // Offline (no sound card): nothing goes out; the status says so.
    card.live = false;
    card.run(cv);
    CHECK(card.outputs[2][10] == 0.0f);
    CHECK_FALSE(cv.isLive());
    card.live = true;
    card.run(cv);
    CHECK(cv.isLive());
    CHECK(cv.getSeenOutputs() == Card::kChannels);
    CHECK(cv.getProblems().isEmpty());
    setChoice(cv.getPortChoice(devices::CvInstrument::modPort), 20); // (output 20: not on this card)
    CHECK(cv.getProblems().contains("Mod (20)"));

    // Audio In: the modular's audio comes back into the track, at the input's gain.
    setChoice(cv.audioIn_, 1); // "1/2"
    setFloat(cv.inputGain_, -6.0f);
    for (int i = 0; i < kBlock; ++i)
    {
        card.inputs[0][static_cast<std::size_t>(i)] = 0.5;
        card.inputs[1][static_cast<std::size_t>(i)] = -0.25;
    }
    card.run(cv);
    CHECK_THAT(card.audio.getSample(0, 10), WithinAbs(0.5 * 0.501187, 1e-4));
    CHECK_THAT(card.audio.getSample(1, 10), WithinAbs(-0.25 * 0.501187, 1e-4));
}

TEST_CASE("CV Instrument calibration: steps out, the oscillator's pitch measured, tracking and offset set", "[cv][calibration]")
{
    devices::CvInstrument cv;
    cv.prepareToPlay(kRate, kBlock);
    Card card;
    setChoice(cv.getPortChoice(devices::CvInstrument::pitchPort), 1);
    setChoice(cv.audioIn_, 34); // single channel "2"
    REQUIRE(devices::HardwareInsert::channelsOf(34).first == 1);

    // Without a return there is nothing to calibrate against.
    devices::CvInstrument unpatched;
    unpatched.prepareToPlay(kRate, kBlock);
    unpatched.calibrate();
    CHECK(unpatched.getCalibration() == devices::CvInstrument::Calibration::failed);
    CHECK(unpatched.getCalibrationMessage().contains("Audio In"));

    // A VCO that tracks 2 % flat, C3 at 0 V: it plays what the pitch output sent in the block before.
    const auto vco = [](double volts) { return 130.8128 * std::exp2(volts * 0.98); };
    double phase = 0.0;
    float lastVolts = 0.0f;
    const auto runLoop = [&](int blocks)
    {
        for (int b = 0; b < blocks; ++b)
        {
            const double hz = vco(lastVolts);
            for (int i = 0; i < kBlock; ++i)
            {
                phase += hz / kRate;
                card.inputs[1][static_cast<std::size_t>(i)] = 0.4 * std::sin(2.0 * std::numbers::pi * phase);
            }
            card.run(cv);
            lastVolts = card.volts(0, kBlock - 1);
        }
    };
    cv.calibrate();
    CHECK(cv.getCalibration() == devices::CvInstrument::Calibration::waiting);
    runLoop(2);
    CHECK(cv.getCalibration() == devices::CvInstrument::Calibration::running);
    CHECK_THAT(card.volts(0, 10), WithinAbs(0.0, 1e-6)); // the first step: 0 V, the gate open
    runLoop(static_cast<int>(devices::CvInstrument::kCalibrationSteps * devices::CvInstrument::kStepSeconds * kRate / kBlock) + 2);
    CHECK(cv.getCalibration() == devices::CvInstrument::Calibration::analysing);
    cv.service();
    INFO(cv.getCalibrationMessage().toStdString());
    REQUIRE(cv.getCalibration() == devices::CvInstrument::Calibration::done);
    const auto measured = cv.getMeasuredPitches();
    REQUIRE(measured.size() == 4);
    CHECK_THAT(measured[0], WithinAbs(130.81, 0.3));
    CHECK_THAT(measured[2], WithinAbs(vco(2.0), 0.6));
    CHECK_THAT(cv.tracking_->get(), WithinAbs(100.0 / 0.98, 0.05));
    CHECK_THAT(cv.offset_->get(), WithinAbs((36.0 - 48.0) / 12.0 / 0.98 * 1000.0, 3.0));
    CHECK(cv.getCalibrationMessage().startsWith("Calibrated"));

    // Calibrated, every note plays in tune on that VCO: C4 and A4.
    for (const int note : {60, 69})
    {
        card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, note, 1.0f)}}));
        const double hz = vco(card.volts(0, 100));
        CHECK_THAT(69.0 + 12.0 * std::log2(hz / 440.0), WithinAbs(static_cast<double>(note), 0.03));
        card.run(cv, midiOf({{0, juce::MidiMessage::allNotesOff(1)}}));
    }
    // Nothing in tune comes back: it says so.
    CHECK_FALSE(cv.applyCalibration({0.0, 0.0, 0.0, 0.0}));
    CHECK_FALSE(cv.applyCalibration({100.0, 150.0, 225.0, 337.5})); // (7 semitones a volt)
    CHECK(devices::cv::measurePitch(std::vector<float>(4800, 0.0f).data(), 4800, kRate) == 0.0);
}

TEST_CASE("CV Triggers: a trigger (or gate) for each note; CV Clock: pulses at the tempo, run and reset", "[cv]")
{
    devices::CvTriggers triggers;
    triggers.prepareToPlay(kRate, kBlock);
    Card card;
    setChoice(triggers.getPortChoice(0), 1);
    setChoice(triggers.getPortChoice(1), 2);
    REQUIRE(triggers.getNumPorts() == devices::CvTriggers::kSlots);
    // Kick (36) and snare (38): 10 ms triggers at 5 V.
    card.run(triggers, midiOf({{0, juce::MidiMessage::noteOn(1, 36, 1.0f)}, {100, juce::MidiMessage::noteOn(1, 37, 1.0f)}}));
    CHECK_THAT(card.volts(0, 10), WithinAbs(5.0, 1e-5));
    CHECK_THAT(card.volts(1, 110), WithinAbs(5.0, 1e-5)); // (the second slot's note is 37, on output 2)
    CHECK(card.volts(1, 90) == 0.0f);
    std::vector<float> kick;
    for (int b = 0; b < 3; ++b)
    {
        if (b > 0)
            card.run(triggers);
        for (int i = 0; i < kBlock; ++i)
            kick.push_back(card.volts(0, i));
    }
    CHECK_THAT(kick[static_cast<std::size_t>(0.009 * kRate)], WithinAbs(5.0, 1e-5));
    CHECK(kick[static_cast<std::size_t>(0.011 * kRate)] == 0.0f);
    // Gates: held while the note is; velocity sets the level.
    setChoice(triggers.mode_, 1);
    triggers.velocity_->setValueNotifyingHost(1.0f);
    card.run(triggers, midiOf({{0, juce::MidiMessage::noteOn(1, 37, 0.5f)}}));
    for (int b = 0; b < 4; ++b)
        card.run(triggers);
    CHECK_THAT(card.volts(1, 100), WithinAbs(64.0 / 127.0 * 5.0, 1e-3));
    card.run(triggers, midiOf({{50, juce::MidiMessage::noteOff(1, 37)}}));
    CHECK(card.volts(1, 100) == 0.0f);

    // The clock: 4 pulses a quarter at 120 BPM: 8 a second, run high, a reset as it starts.
    devices::CvClock clock;
    TestPlayHead head;
    clock.setPlayHead(&head);
    clock.prepareToPlay(kRate, kBlock);
    setChoice(clock.getPortChoice(devices::CvClock::clockPort), 1);
    setChoice(clock.getPortChoice(devices::CvClock::runPort), 2);
    setChoice(clock.getPortChoice(devices::CvClock::resetPort), 3);
    std::vector<float> pulses, run, reset;
    for (int b = 0; b < static_cast<int>(kRate / kBlock); ++b)
    {
        card.run(clock);
        head.ppq += kBlock * head.bpm / 60.0 / kRate;
        for (int i = 0; i < kBlock; ++i)
        {
            pulses.push_back(card.volts(0, i));
            run.push_back(card.volts(1, i));
            reset.push_back(card.volts(2, i));
        }
    }
    int rises = 0;
    for (std::size_t i = 1; i < pulses.size(); ++i)
        rises += pulses[i] > 2.5f && pulses[i - 1] <= 2.5f ? 1 : 0;
    CHECK(rises + (pulses[0] > 2.5f ? 1 : 0) == 8);
    CHECK(clock.getPulses() == 8);
    CHECK_THAT(run[1000], WithinAbs(5.0, 1e-5));
    CHECK_THAT(reset[100], WithinAbs(5.0, 1e-5));
    CHECK(reset[1000] == 0.0f);
    // Half-width pulses: high for the first half of each 125 ms.
    CHECK(pulses[static_cast<std::size_t>(0.04 * kRate)] > 2.5f);
    CHECK(pulses[static_cast<std::size_t>(0.08 * kRate)] < 2.5f);
    // Swing: every second pulse later (50 %: half a pulse late).
    setFloat(clock.swing_, 50.0f);
    head.ppq = 0.0;
    card.run(clock);
    std::vector<float> swung;
    for (int b = 0; b < 40; ++b)
    {
        head.ppq = b * kBlock * head.bpm / 60.0 / kRate;
        card.run(clock);
        for (int i = 0; i < kBlock; ++i)
            swung.push_back(card.volts(0, i));
    }
    // The second pulse starts at 1.5 pulses (187.5 ms), not 1 (125 ms).
    CHECK(swung[static_cast<std::size_t>(0.140 * kRate)] < 2.5f);
    CHECK(swung[static_cast<std::size_t>(0.195 * kRate)] > 2.5f);
    // Stopped: no clock, run low.
    head.playing = false;
    card.run(clock);
    CHECK(card.volts(0, 10) == 0.0f);
    CHECK(card.volts(1, 10) == 0.0f);
}

TEST_CASE("CV Utility: a voltage from a value, with an LFO and slew; CV In: an input's voltage as modulation", "[cv]")
{
    devices::CvUtility utility;
    utility.prepareToPlay(kRate, kBlock);
    Card card;
    setChoice(utility.getPortChoice(0), 1);
    setFloat(utility.maximum_, 10.0f);
    setFloat(utility.value_, 50.0f);
    card.run(utility);
    CHECK_THAT(card.volts(0, 100), WithinAbs(5.0, 1e-4));
    // An LFO of 1 V at 10 Hz around it.
    setChoice(utility.shape_, 1);
    setFloat(utility.rate_, 10.0f);
    setFloat(utility.depth_, 1.0f);
    float lo = 100.0f, hi = -100.0f;
    for (int b = 0; b < 40; ++b)
    {
        card.run(utility);
        for (int i = 0; i < kBlock; ++i)
        {
            lo = std::min(lo, card.volts(0, i));
            hi = std::max(hi, card.volts(0, i));
        }
    }
    CHECK_THAT(lo, WithinAbs(4.0, 0.01));
    CHECK_THAT(hi, WithinAbs(6.0, 0.01));
    // Slew: a jump to 0 V takes its time.
    setChoice(utility.shape_, 0);
    setFloat(utility.slew_, 200.0f);
    card.run(utility);
    setFloat(utility.value_, 0.0f);
    card.run(utility);
    CHECK(card.volts(0, kBlock - 1) > 3.0f);
    for (int b = 0; b < 200; ++b)
        card.run(utility);
    CHECK_THAT(card.volts(0, kBlock - 1), WithinAbs(0.0, 0.01));
    // Bipolar: -5..5 V.
    setFloat(utility.minimum_, -5.0f);
    setFloat(utility.maximum_, 5.0f);
    setFloat(utility.slew_, 0.0f);
    card.run(utility);
    CHECK_THAT(card.volts(0, 100), WithinAbs(-5.0, 1e-4));

    // CV In: 2.5 V on input 1 is halfway between 0 and 5 V; inverted, too; smoothed toward it.
    devices::CvIn in;
    in.prepareToPlay(kRate, kBlock);
    for (auto& v : card.inputs[0])
        v = 0.25;
    setFloat(in.smooth_, 0.0f);
    card.run(in);
    CHECK_THAT(in.advanceModulation(kBlock, kRate, 120.0, 0.0, true), WithinAbs(0.5, 1e-6));
    CHECK_THAT(in.getVolts(), WithinAbs(2.5, 1e-5));
    in.invert_->setValueNotifyingHost(1.0f);
    CHECK_THAT(in.advanceModulation(kBlock, kRate, 120.0, 0.0, true), WithinAbs(0.5, 1e-6));
    setFloat(in.maximum_, 10.0f);
    in.invert_->setValueNotifyingHost(0.0f);
    CHECK_THAT(in.advanceModulation(kBlock, kRate, 120.0, 0.0, true), WithinAbs(0.25, 1e-6));
    setFloat(in.smooth_, 100.0f);
    for (auto& v : card.inputs[0])
        v = 1.0;
    card.run(in);
    const double first = in.advanceModulation(kBlock, kRate, 120.0, 0.0, true);
    CHECK(first > 0.25);
    CHECK(first < 0.5);
    // (the track's audio passes through)
    CHECK(card.audio.getSample(0, 0) == 0.0f);
}

TEST_CASE("CV devices in the browser and their editors: ports, meters, calibration; CV In maps like the LFO", "[cv][ui]")
{
    for (const auto* type : {devices::CvInstrument::kTypeId, devices::CvTriggers::kTypeId, devices::CvClock::kTypeId, devices::CvUtility::kTypeId,
                             devices::CvIn::kTypeId})
    {
        INFO(type);
        const auto* info = devices::findBuiltinDevice(type);
        REQUIRE(info != nullptr);
        CHECK(info->category == "CV");
        CHECK(devices::createBuiltinDevice(type) != nullptr);
    }
    CHECK(devices::findBuiltinDevice(devices::CvInstrument::kTypeId)->isInstrument);
    CHECK(devices::findBuiltinDevice(devices::CvTriggers::kTypeId)->isInstrument);
    CHECK_FALSE(devices::findBuiltinDevice(devices::CvClock::kTypeId)->isInstrument);

    struct MessageThread
    {
        MessageThread() { juce::MessageManager::getInstance(); }
    } messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host(engine);
    engine::EngineController controller(engine, host);
    session::Project project([&controller](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    ui::EditorState editor;
    auto& session = project.getSession();
    auto track = session.addTrack(session::TrackKind::instrument, "Modular");
    auto device = session.addDevice(track, devices::CvInstrument::kTypeId, "CV Instrument");
    devices::CvInstrument cv;
    cv.prepareToPlay(kRate, kBlock);
    ui::DeviceEditorContext context{project, editor, device, track};
    int width = 0;
    auto component = ui::createCvEditor(cv, context, width);
    REQUIRE(component != nullptr);
    CHECK(width > 400);
    auto* cvEditor = dynamic_cast<ui::CvEditor*>(component.get());
    REQUIRE(cvEditor != nullptr);
    cvEditor->setSize(width, 260);
    CHECK(cvEditor->getStatus().contains("No sound card"));
    // Calibrate without the patch: it says what is missing.
    cvEditor->getCalibrateButton().triggerClick();
    juce::MessageManager::getInstance()->runDispatchLoopUntil(60);
    CHECK(cvEditor->getStatus().contains("Audio In"));
    Card card;
    setChoice(cv.getPortChoice(0), 1);
    card.run(cv, midiOf({{0, juce::MidiMessage::noteOn(1, 60, 1.0f)}}));
    cvEditor->refresh();
    juce::Image image(juce::Image::RGB, width, 260, true);
    {
        juce::Graphics g(image);
        cvEditor->paintEntireComponent(g, false);
    }

    // CV In: the LFO's mapping list, with its own display.
    auto modulator = session.addDevice(track, devices::CvIn::kTypeId, "CV In");
    devices::CvIn in;
    ui::DeviceEditorContext inContext{project, editor, modulator, track};
    auto inEditor = ui::createCvEditor(in, inContext, width);
    auto* mapper = dynamic_cast<ui::LfoEditor*>(inEditor.get());
    REQUIRE(mapper != nullptr);
    mapper->setSize(width, 220);
    const auto candidates = mapper->getCandidates();
    REQUIRE_FALSE(candidates.empty());
    REQUIRE(mapper->map(candidates.front()));
    CHECK(session::Session::getModulations(modulator).size() == 1);
    {
        juce::Graphics g(image);
        mapper->paintEntireComponent(g, false);
    }
}

#include "core/rt/RealtimeGuard.h"
#include "devices/DeviceRegistry.h"
#include "devices/Dynamics.h"
#include "devices/Equaliser.h"
#include "devices/Instruments.h"
#include "devices/TimeEffects.h"
#include "engine/ProcessorNode.h"
#include "TestAudio.h"
#include "TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::devices;
using dojo::engine::ProcessorNode;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kRate = 48'000.0;
constexpr int kBlock = 256;

struct Stereo
{
    explicit Stereo(std::size_t n = 0)
        : left(n, 0.0)
        , right(n, 0.0)
    {
    }
    std::vector<double> left, right;
};

Stereo fromMono(const std::vector<float>& mono)
{
    Stereo s(mono.size());
    for (std::size_t i = 0; i < mono.size(); ++i)
        s.left[i] = s.right[i] = mono[i];
    return s;
}

struct TimedMidi
{
    int sample;
    juce::MidiMessage message;
};

/** Runs `node` over `input` in blocks, with optional sidechain and MIDI. */
Stereo run(ProcessorNode& node, const Stereo& input, const Stereo* sidechain = nullptr, std::vector<TimedMidi> midiEvents = {})
{
    const auto total = input.left.size();
    Stereo out(total);
    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::AudioBuffer<double> side(2, kBlock);
    juce::MidiBuffer midi;

    for (std::size_t offset = 0; offset < total; offset += kBlock)
    {
        const int n = static_cast<int>(std::min<std::size_t>(kBlock, total - offset));
        for (int i = 0; i < n; ++i)
        {
            buffer.setSample(0, i, input.left[offset + static_cast<std::size_t>(i)]);
            buffer.setSample(1, i, input.right[offset + static_cast<std::size_t>(i)]);
            if (sidechain != nullptr)
            {
                side.setSample(0, i, sidechain->left[offset + static_cast<std::size_t>(i)]);
                side.setSample(1, i, sidechain->right[offset + static_cast<std::size_t>(i)]);
            }
        }

        midi.clear();
        for (const auto& e : midiEvents)
            if (e.sample >= static_cast<int>(offset) && e.sample < static_cast<int>(offset) + n)
                midi.addEvent(e.message, e.sample - static_cast<int>(offset));

        node.process(buffer, n, midi, sidechain != nullptr ? &side : nullptr);

        for (int i = 0; i < n; ++i)
        {
            out.left[offset + static_cast<std::size_t>(i)] = buffer.getSample(0, i);
            out.right[offset + static_cast<std::size_t>(i)] = buffer.getSample(1, i);
        }
    }
    return out;
}

void setParameter(BuiltinProcessor& processor, const juce::String& id, float value)
{
    auto* parameter = processor.getParameterById(id);
    REQUIRE(parameter != nullptr);
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
}

template <typename T>
std::pair<std::unique_ptr<ProcessorNode>, T*> makeNode()
{
    auto processor = std::make_unique<T>();
    auto* raw = processor.get();
    auto node = std::make_unique<ProcessorNode>(std::move(processor));
    node->prepare(kRate, kBlock, nullptr);
    return {std::move(node), raw};
}
} // namespace

TEST_CASE("Every native device can be created by type and saves its state", "[devices]")
{
    for (const auto& info : getBuiltinDevices())
    {
        auto processor = createBuiltinDevice(info.type);
        REQUIRE(processor != nullptr);
        CHECK(processor->acceptsMidi() == info.isInstrument);

        juce::MemoryBlock state;
        processor->getStateInformation(state);
        CHECK(state.getSize() > 0);

        auto copy = createBuiltinDevice(info.type);
        copy->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }
    CHECK(createBuiltinDevice("builtin:nope") == nullptr);
}

TEST_CASE("EQ peak band boosts its frequency and the response curve agrees", "[devices][eq]")
{
    auto [node, eq] = makeNode<Equaliser>();
    setParameter(*eq, "band2Freq", 1000.0f);
    setParameter(*eq, "band2Gain", 12.0f);
    setParameter(*eq, "band2Q", 1.0f);
    node->prepare(kRate, kBlock, nullptr); // snap the smoothed values

    const auto input = fromMono(test::sine(1000.0, kRate, 24000, 0.1f));
    const auto output = run(*node, input);
    const double gain = test::rms(output.left, 12000, 24000) / test::rms(input.left, 12000, 24000);

    CHECK_THAT(gain, WithinRel(std::pow(10.0, 12.0 / 20.0), 0.05));
    CHECK_THAT(eq->getMagnitudeAt(1000.0), WithinRel(std::pow(10.0, 12.0 / 20.0), 0.01));
    CHECK_THAT(eq->getMagnitudeAt(50.0), WithinAbs(1.0, 0.05));

    std::vector<float> analyser(1024);
    eq->copyAnalyserSamples(analyser.data(), 1024);
    CHECK(test::peak(analyser) > 0.2);
}

TEST_CASE("EQ low cut removes rumble", "[devices][eq]")
{
    auto [node, eq] = makeNode<Equaliser>();
    setParameter(*eq, "band1Shape", 0.0f); // low cut
    setParameter(*eq, "band1Freq", 400.0f);
    node->prepare(kRate, kBlock, nullptr);

    const auto output = run(*node, fromMono(test::sine(40.0, kRate, 48000, 0.5f)));
    CHECK(test::rms(output.left, 24000, 48000) < 0.5 * 0.7071 * 0.02); // > 34 dB down
}

TEST_CASE("Compressor reduces level above the threshold by the ratio", "[devices][dynamics]")
{
    auto [node, comp] = makeNode<Compressor>();
    setParameter(*comp, "threshold", -20.0f);
    setParameter(*comp, "ratio", 4.0f);
    setParameter(*comp, "knee", 0.0f);
    setParameter(*comp, "attack", 0.1f);
    setParameter(*comp, "release", 50.0f);

    // A 0 dBFS square-ish signal (constant magnitude) makes the static curve easy to check:
    // 20 dB over the threshold at 4:1 -> 5 dB over -> -15 dBFS.
    Stereo input(48000);
    for (std::size_t i = 0; i < input.left.size(); ++i)
        input.left[i] = input.right[i] = (i / 24) % 2 == 0 ? 1.0 : -1.0;

    const auto output = run(*node, input);
    CHECK_THAT(test::peak(output.left, 40000, 48000), WithinAbs(std::pow(10.0, -15.0 / 20.0), 0.01));
    CHECK(comp->takeGainReductionDb() > 14.0f);
    CHECK_THAT(comp->getOutputLevelDb(0.0), WithinAbs(-15.0, 1e-9));
}

TEST_CASE("Compressor follows an external sidechain when asked to", "[devices][dynamics]")
{
    auto [node, comp] = makeNode<Compressor>();
    REQUIRE(node->hasSidechainInput());
    setParameter(*comp, "threshold", -30.0f);
    setParameter(*comp, "ratio", 20.0f);
    setParameter(*comp, "attack", 0.1f);

    const auto music = fromMono(test::sine(220.0, kRate, 24000, 0.25f));
    const auto kick = fromMono(test::sine(60.0, kRate, 24000, 1.0f));

    SECTION("internal detector ignores the sidechain bus")
    {
        Stereo silence(24000);
        const auto out = run(*node, music, &silence);
        CHECK(test::rms(out.left, 12000, 24000) < test::rms(music.left, 12000, 24000)); // music itself is loud enough
    }

    SECTION("external detector ducks the music under the kick")
    {
        setParameter(*comp, "threshold", -20.0f); // the kick peaks 20 dB over it: ~19 dB of ducking
        setParameter(*comp, "sidechain", 1.0f);
        Stereo silence(24000);
        const auto undisturbed = run(*node, music, &silence);
        CHECK_THAT(test::rms(undisturbed.left, 12000, 24000), WithinRel(test::rms(music.left, 12000, 24000), 0.01));

        const auto ducked = run(*node, music, &kick);
        CHECK(test::rms(ducked.left, 12000, 24000) < 0.25 * test::rms(music.left, 12000, 24000));
    }
}

TEST_CASE("Limiter holds the ceiling with lookahead and reports its latency", "[devices][dynamics]")
{
    auto [node, limiter] = makeNode<Limiter>();
    setParameter(*limiter, "ceiling", -1.0f);
    setParameter(*limiter, "lookahead", 5.0f);
    node->prepare(kRate, kBlock, nullptr);
    CHECK(node->getLatencySamples() == 240);

    const auto loud = fromMono(test::sine(100.0, kRate, 24000, 2.0f)); // +6 dBFS
    const auto output = run(*node, loud);
    const double ceiling = std::pow(10.0, -1.0 / 20.0);
    CHECK(test::peak(output.left) <= ceiling + 1e-9);
    CHECK(test::peak(output.left, 12000, 24000) > ceiling * 0.95); // limited, not just attenuated
    CHECK(limiter->takeGainReductionDb() > 6.0f);

    // Quiet material passes untouched, delayed by exactly the lookahead.
    node->prepare(kRate, kBlock, nullptr);
    Stereo impulse(4800);
    impulse.left[1000] = impulse.right[1000] = 0.5;
    const auto delayed = run(*node, impulse);
    CHECK(test::indexOfPeak(delayed.left) == 1240);
    CHECK_THAT(delayed.left[1240], WithinAbs(0.5, 1e-9));

    setParameter(*limiter, "lookahead", 2.0f);
    CHECK(node->getLatencySamples() == 96);
}

TEST_CASE("Reverb produces a decaying tail and passes dry signal at zero mix", "[devices][space]")
{
    auto [node, reverb] = makeNode<Reverb>();
    setParameter(*reverb, "decay", 1.0f);
    setParameter(*reverb, "mix", 100.0f);

    Stereo impulse(96000);
    impulse.left[0] = impulse.right[0] = 1.0;
    const auto output = run(*node, impulse);

    const double early = test::rms(output.left, 4800, 14400);
    const double late = test::rms(output.left, 72000, 96000);
    CHECK(early > 1e-3);
    CHECK(late < early * 0.01); // > 40 dB down after 1.5 s with a 1 s RT60
    CHECK(test::rms(output.left, 4800, 14400) != test::rms(output.right, 4800, 14400)); // stereo

    setParameter(*reverb, "mix", 0.0f);
    const auto music = fromMono(test::sine(440.0, kRate, 4800, 0.3f));
    const auto dry = run(*node, music);
    CHECK_THAT(dry.left[2000], WithinAbs(music.left[2000], 1e-12));
}

TEST_CASE("Delay repeats at the set time and ping-pongs", "[devices][space]")
{
    auto [node, delay] = makeNode<Delay>();
    setParameter(*delay, "sync", 0.0f);
    setParameter(*delay, "time", 100.0f);
    setParameter(*delay, "feedback", 50.0f);
    setParameter(*delay, "mix", 100.0f);
    setParameter(*delay, "lowcut", 20.0f);
    setParameter(*delay, "highcut", 20000.0f);
    node->prepare(kRate, kBlock, nullptr);

    Stereo impulse(24000);
    impulse.left[100] = impulse.right[100] = 1.0;
    const auto output = run(*node, impulse);

    // First echo after 100 ms on the left (mono sum in), the second one 100 ms later on the right.
    CHECK(test::peak(output.left, 4880, 4920) > 0.4);
    CHECK(test::peak(output.right, 4880, 4920) < 0.01);
    CHECK(test::peak(output.right, 9680, 9720) > 0.15);

    CHECK_THAT(Delay::beatsForDivision(8), WithinAbs(1.0, 1e-12)); // 1/4
    CHECK_THAT(Delay::beatsForDivision(6), WithinAbs(0.75, 1e-12)); // 1/8 dotted
}

TEST_CASE("Pitch shifter transposes by an octave", "[devices][pitch]")
{
    auto [node, shifter] = makeNode<PitchShifter>();
    setParameter(*shifter, "semitones", 12.0f);
    CHECK(node->getLatencySamples() > 0);

    const auto output = run(*node, fromMono(test::sine(440.0, kRate, 48000, 0.4f)));
    CHECK_THAT(test::estimateFrequency(output.left, 24000, 48000, kRate), WithinRel(880.0, 0.03));
}

TEST_CASE("Sampler plays its sample chromatically with an envelope", "[devices][instrument]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("a440.wav"), test::sine(440.0, kRate, 96000, 0.5f), kRate);

    auto [node, sampler] = makeNode<Sampler>();
    REQUIRE(node->isInstrument());
    juce::String error;
    REQUIRE(sampler->loadSample(file, error));
    setParameter(*sampler, "root", 69.0f);
    setParameter(*sampler, "release", 10.0f);

    const auto output = run(*node, Stereo(48000), nullptr,
                            {{4800, juce::MidiMessage::noteOn(1, 81, 1.0f)}, {24000, juce::MidiMessage::noteOff(1, 81)}});

    CHECK(test::peak(output.left, 0, 4800) == 0.0);
    CHECK_THAT(test::estimateFrequency(output.left, 6000, 20000, kRate), WithinRel(880.0, 0.01)); // an octave up
    CHECK(test::peak(output.left, 26000, 48000) < 1e-6); // released after 10 ms
    CHECK(sampler->getActiveVoiceCount() == 0);

    // State brings back the sample and the parameters.
    juce::MemoryBlock state;
    sampler->getStateInformation(state);
    Sampler restored;
    restored.setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    REQUIRE(restored.getSample() != nullptr);
    CHECK(restored.getSample()->file == file);
    CHECK_THAT(restored.root_->get(), WithinAbs(69.0, 1e-6));
}

TEST_CASE("Drum Rack pads play on their notes, route to aux outputs and choke", "[devices][instrument]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("hit.wav"), std::vector<float>(4800, 0.5f), kRate);

    auto processor = std::make_unique<DrumRack>();
    auto* rack = processor.get();
    juce::String error;
    REQUIRE(rack->loadPadSample(0, file, error));
    REQUIRE(rack->loadPadSample(1, file, error));

    ProcessorNode node(std::move(processor));
    node.prepare(kRate, kBlock, nullptr);
    REQUIRE(node.getNumAuxOutputChannels() == 32);

    SECTION("main output")
    {
        const auto out = run(node, Stereo(2400), nullptr, {{100, juce::MidiMessage::noteOn(1, 36, 1.0f)}});
        CHECK(test::peak(out.left, 0, 100) == 0.0);
        CHECK(test::peak(out.left, 200, 2400) > 0.4);
    }

    SECTION("aux output")
    {
        setParameter(*rack, "pad2Output", 3.0f); // "Out 3"
        juce::AudioBuffer<double> buffer(2, kBlock);
        buffer.clear();
        juce::MidiBuffer midi;
        midi.addEvent(juce::MidiMessage::noteOn(1, 37, 1.0f), 0);
        node.process(buffer, kBlock, midi, nullptr);

        CHECK(buffer.getMagnitude(0, 0, kBlock) == 0.0); // main stays silent
        const double* aux = node.getAuxOutput(4); // Out 3 = aux channels 4/5
        REQUIRE(aux != nullptr);
        CHECK(std::abs(aux[kBlock - 1]) > 0.4);
    }

    SECTION("choke group")
    {
        setParameter(*rack, "pad1Choke", 1.0f);
        setParameter(*rack, "pad2Choke", 1.0f);
        const auto out = run(node, Stereo(2400), nullptr,
                             {{0, juce::MidiMessage::noteOn(1, 36, 1.0f)}, {1000, juce::MidiMessage::noteOn(1, 37, 1.0f)}});
        // Pad 1 fades out within 5 ms of pad 2's hit; only pad 2 remains (0.5 x velocity gain).
        CHECK(test::peak(out.left, 1300, 2400) < 0.55);
        CHECK(test::peak(out.left, 500, 1000) > 0.4);
    }

    SECTION("audition from the UI")
    {
        rack->triggerPad(0, 1.0f);
        const auto out = run(node, Stereo(512));
        CHECK(test::peak(out.left) > 0.4);
        CHECK(rack->takePadActivity(0) > 0.4f);
    }
}

TEST_CASE("Native devices never touch the heap while processing", "[devices][rt]")
{
    DOJO_REQUIRE_ALLOCATION_TRAP();

    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("s.wav"), test::sine(220.0, kRate, 9600, 0.5f), kRate);

    std::vector<std::unique_ptr<ProcessorNode>> nodes;
    for (const auto& info : getBuiltinDevices())
    {
        auto processor = createBuiltinDevice(info.type);
        juce::String error;
        if (auto* sampler = dynamic_cast<Sampler*>(processor.get()))
            REQUIRE(sampler->loadSample(file, error));
        if (auto* rack = dynamic_cast<DrumRack*>(processor.get()))
            REQUIRE(rack->loadPadSample(0, file, error));
        nodes.push_back(std::make_unique<ProcessorNode>(std::move(processor)));
        nodes.back()->prepare(kRate, kBlock, nullptr);
    }

    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::AudioBuffer<double> side(2, kBlock);
    juce::MidiBuffer midi;
    midi.ensureSize(4096);
    const auto music = test::sine(330.0, kRate, kBlock, 0.8f);

    const auto before = rt::getRealtimeViolationCount();
    {
        const rt::ScopedRealtimeSection section;
        for (int block = 0; block < 200; ++block)
        {
            for (auto& node : nodes)
            {
                for (int i = 0; i < kBlock; ++i)
                {
                    buffer.setSample(0, i, music[static_cast<std::size_t>(i)]);
                    buffer.setSample(1, i, music[static_cast<std::size_t>(i)]);
                }
                midi.clear();
                if (block % 20 == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 36 + block % 24, 0.9f), 10);
                if (block % 20 == 10)
                    midi.addEvent(juce::MidiMessage::noteOff(1, 36 + (block - 10) % 24), 10);
                node->process(buffer, kBlock, midi, &side);
            }
        }
    }
    CHECK(rt::getRealtimeViolationCount() == before);
}

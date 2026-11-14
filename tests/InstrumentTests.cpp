#include "core/rt/RealtimeGuard.h"
#include "devices/Impulse.h"
#include "devices/Operator.h"
#include "devices/WavetableSynth.h"
#include "DeviceTestSupport.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kPi = 3.14159265358979323846;

double frequencyOf(double note)
{
    return 440.0 * std::pow(2.0, (note - 69.0) / 12.0);
}

double amplitudeAt(const std::vector<double>& x, double frequency, std::size_t begin, std::size_t end)
{
    const double w = 2.0 * kPi * frequency / kRate;
    const double coefficient = 2.0 * std::cos(w);
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = begin; i < end; ++i)
    {
        const double s0 = x[i] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    return 2.0 * std::sqrt(std::max(0.0, s1 * s1 + s2 * s2 - coefficient * s1 * s2)) / static_cast<double>(end - begin);
}

double rmsOf(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    double sum = 0.0;
    for (std::size_t i = begin; i < end; ++i)
        sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(std::max<std::size_t>(1, end - begin)));
}

double frequencyEstimate(const std::vector<double>& x, std::size_t begin, std::size_t end)
{
    return test::estimateFrequency(x, begin, end, kRate);
}

std::vector<TimedMidi> note(int number, int on, int off = -1, float velocity = 1.0f)
{
    std::vector<TimedMidi> events{{on, juce::MidiMessage::noteOn(1, number, velocity)}};
    if (off >= 0)
        events.push_back({off, juce::MidiMessage::noteOff(1, number)});
    return events;
}

Stereo play(ProcessorNode& node, std::size_t frames, std::vector<TimedMidi> events)
{
    return run(node, Stereo(frames), nullptr, std::move(events));
}

/** Wavetable reduced to one plain oscillator through an open filter, no modulation. */
void makePlain(WavetableSynth& synth)
{
    setParameter(synth, "o2on", 0.0f);
    setParameter(synth, "o1pos", 0.0f); // Basic Shapes, first frame: sine
    setParameter(synth, "cutoff", 20000.0f);
    setParameter(synth, "keytrack", 0.0f);
    for (int m = 1; m <= WavetableSynth::kMatrixSlots; ++m)
        setParameter(synth, "m" + juce::String(m) + "amt", 0.0f);
}
} // namespace

//==================================================================================================
// Wavetable

TEST_CASE("Wavetable plays the note's pitch and morphs its waveform", "[devices][wavetable]")
{
    auto [node, synth] = makeNode<WavetableSynth>();
    makePlain(*synth);
    node->prepare(kRate, kBlock, nullptr);

    auto out = play(*node, 24'000, note(60, 0));
    CHECK_THAT(frequencyEstimate(out.left, 4800, 24'000), WithinAbs(frequencyOf(60), 0.5));
    const double sineThird = amplitudeAt(out.left, 3.0 * frequencyOf(60), 4800, 24'000) / amplitudeAt(out.left, frequencyOf(60), 4800, 24'000);
    CHECK(sineThird < 0.01);
    CHECK(synth->getActiveVoiceCount() == 1);

    // Position 100 % of Basic Shapes is a square: strong odd harmonics (1/3 of the fundamental).
    setParameter(*synth, "o1pos", 100.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 24'000, note(48, 0));
    const double squareThird = amplitudeAt(out.left, 3.0 * frequencyOf(48), 4800, 24'000) / amplitudeAt(out.left, frequencyOf(48), 4800, 24'000);
    CHECK_THAT(squareThird, WithinAbs(1.0 / 3.0, 0.05));
}

TEST_CASE("Wavetable oscillators are band-limited", "[devices][wavetable]")
{
    auto [node, synth] = makeNode<WavetableSynth>();
    makePlain(*synth);
    setParameter(*synth, "o1pos", 66.7f); // the saw frame
    node->prepare(kRate, kBlock, nullptr);

    // C9 (8372 Hz): the 3rd harmonic (25.1 kHz) would alias to 22.9 kHz.
    const auto out = play(*node, 24'000, note(120, 0));
    const double fundamental = amplitudeAt(out.left, frequencyOf(120), 4800, 24'000);
    const double alias = amplitudeAt(out.left, kRate - 3.0 * frequencyOf(120), 4800, 24'000);
    CHECK(fundamental > 0.05);
    CHECK(alias < fundamental * 0.01);
}

TEST_CASE("Wavetable modulation matrix, unison and filter", "[devices][wavetable]")
{
    auto [node, synth] = makeNode<WavetableSynth>();
    makePlain(*synth);

    // Mod wheel -> Osc 1 Pitch at +50 % = +12 semitones at full wheel.
    setParameter(*synth, "m1src", static_cast<float>(WavetableSynth::srcModWheel));
    setParameter(*synth, "m1dst", static_cast<float>(WavetableSynth::dstOsc1Pitch));
    setParameter(*synth, "m1amt", 50.0f);
    node->prepare(kRate, kBlock, nullptr);
    auto events = note(57, 0);
    events.insert(events.begin(), {0, juce::MidiMessage::controllerEvent(1, 1, 127)});
    auto out = play(*node, 24'000, events);
    CHECK_THAT(frequencyEstimate(out.left, 4800, 24'000), WithinAbs(2.0 * frequencyOf(57), 1.0));

    // Unison spreads the voices across the stereo field.
    setParameter(*synth, "m1amt", 0.0f);
    setParameter(*synth, "unison", 5.0f); // 8 voices
    setParameter(*synth, "unidetune", 60.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 24'000, note(57, 0));
    double difference = 0.0;
    for (std::size_t i = 4800; i < 24'000; ++i)
        difference += std::abs(out.left[i] - out.right[i]);
    CHECK(difference / 19'200.0 > 0.01);

    // A low cutoff removes the upper harmonics of a saw.
    setParameter(*synth, "unison", 0.0f);
    setParameter(*synth, "o1pos", 66.7f);
    const auto brightness = [&](float cutoff)
    {
        setParameter(*synth, "cutoff", cutoff);
        node->prepare(kRate, kBlock, nullptr);
        const auto o = play(*node, 24'000, note(45, 0));
        return amplitudeAt(o.left, 10.0 * frequencyOf(45), 4800, 24'000) / amplitudeAt(o.left, frequencyOf(45), 4800, 24'000);
    };
    CHECK(brightness(300.0f) < brightness(20'000.0f) * 0.05);
}

TEST_CASE("Wavetable envelopes, release and voice stealing", "[devices][wavetable]")
{
    auto [node, synth] = makeNode<WavetableSynth>();
    makePlain(*synth);
    setParameter(*synth, "r1", 100.0f);
    node->prepare(kRate, kBlock, nullptr);

    const auto violationsBefore = rt::getRealtimeViolationCount();
    auto out = play(*node, 48'000, note(60, 0, 9600));
    CHECK(rmsOf(out.left, 4800, 9600) > 0.05);
    CHECK(rmsOf(out.left, 9600 + 9600, 48'000) < 1.0e-4); // released within ~100 ms
    CHECK(synth->getActiveVoiceCount() == 0);

    std::vector<TimedMidi> chord;
    for (int n = 0; n < 20; ++n)
        chord.push_back({n * 10, juce::MidiMessage::noteOn(1, 40 + n, 0.8f)});
    out = play(*node, 4800, chord);
    CHECK(synth->getActiveVoiceCount() == WavetableSynth::kVoices);
    if (rt::isAllocationTrapInstalled())
        CHECK(rt::getRealtimeViolationCount() == violationsBefore);

    // All notes off.
    out = play(*node, 48'000, {{0, juce::MidiMessage::allNotesOff(1)}});
    CHECK(synth->getActiveVoiceCount() == 0);
}

//==================================================================================================
// Operator

TEST_CASE("Operator: a lone carrier is a sine at the note, modulators add sidebands", "[devices][operator]")
{
    auto [node, fm] = makeNode<Operator>();
    setParameter(*fm, "blevel", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    auto out = play(*node, 24'000, note(57, 0));
    const double fundamental = amplitudeAt(out.left, 220.0, 4800, 24'000);
    CHECK_THAT(frequencyEstimate(out.left, 4800, 24'000), WithinAbs(220.0, 0.5));
    CHECK(amplitudeAt(out.left, 440.0, 4800, 24'000) < fundamental * 0.01);
    CHECK_THAT(out.left[10'000], WithinAbs(out.right[10'000], 1.0e-12));

    // B modulates A (algorithm 1) at a 1:1 ratio: harmonics appear.
    setParameter(*fm, "blevel", 60.0f);
    setParameter(*fm, "bcoarse", 1.0f);
    setParameter(*fm, "bs", 100.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 24'000, note(57, 0));
    CHECK(amplitudeAt(out.left, 440.0, 4800, 24'000) > amplitudeAt(out.left, 220.0, 4800, 24'000) * 0.1);
}

TEST_CASE("Operator ratios, fixed frequency, algorithms and pitch envelope", "[devices][operator]")
{
    CHECK_THAT(Operator::ratioFor(0.0f, 0.0f), WithinAbs(0.5, 1.0e-12));
    CHECK_THAT(Operator::ratioFor(3.0f, 0.0f), WithinAbs(3.0, 1.0e-12));
    CHECK_THAT(Operator::ratioFor(2.0f, 500.0f), WithinAbs(3.0, 1.0e-12));

    auto [node, fm] = makeNode<Operator>();
    setParameter(*fm, "blevel", 0.0f);
    setParameter(*fm, "acoarse", 2.0f);
    node->prepare(kRate, kBlock, nullptr);
    auto out = play(*node, 24'000, note(57, 0));
    CHECK_THAT(frequencyEstimate(out.left, 4800, 24'000), WithinAbs(440.0, 0.5));

    setParameter(*fm, "afixed", 1.0f);
    setParameter(*fm, "afreq", 1000.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 24'000, note(40, 0));
    CHECK_THAT(frequencyEstimate(out.left, 4800, 24'000), WithinAbs(1000.0, 1.0));

    // Algorithm 10 is additive: every operator is heard at its own ratio.
    setParameter(*fm, "afixed", 0.0f);
    setParameter(*fm, "acoarse", 1.0f);
    setParameter(*fm, "algo", 9.0f);
    for (const auto* op : {"b", "c", "d"})
    {
        setParameter(*fm, juce::String(op) + "level", 60.0f);
        setParameter(*fm, juce::String(op) + "s", 100.0f);
    }
    setParameter(*fm, "bcoarse", 2.0f);
    setParameter(*fm, "ccoarse", 3.0f);
    setParameter(*fm, "dcoarse", 5.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 24'000, note(45, 0));
    for (const double ratio : {1.0, 2.0, 3.0, 5.0})
        CHECK(amplitudeAt(out.left, 110.0 * ratio, 4800, 24'000) > 0.02);
    CHECK(amplitudeAt(out.left, 110.0 * 4.0, 4800, 24'000) < 0.002);
    CHECK(Operator::getAlgorithmNames().size() == Operator::kAlgorithms);

    // Pitch envelope: starts an octave up, settles on the note.
    setParameter(*fm, "algo", 0.0f);
    for (const auto* op : {"b", "c", "d"})
        setParameter(*fm, juce::String(op) + "level", 0.0f);
    setParameter(*fm, "penv", 12.0f);
    setParameter(*fm, "pdecay", 300.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 48'000, note(57, 0));
    CHECK(frequencyEstimate(out.left, 0, 960) > 380.0);
    CHECK_THAT(frequencyEstimate(out.left, 38'400, 48'000), WithinAbs(220.0, 1.0));
}

TEST_CASE("Operator feedback, filter and release", "[devices][operator]")
{
    auto [node, fm] = makeNode<Operator>();
    setParameter(*fm, "algo", 9.0f); // additive: D is heard
    setParameter(*fm, "alevel", 0.0f);
    setParameter(*fm, "blevel", 0.0f);
    setParameter(*fm, "dlevel", 80.0f);
    setParameter(*fm, "ds", 100.0f);
    node->prepare(kRate, kBlock, nullptr);
    auto out = play(*node, 24'000, note(57, 0));
    const double pure = amplitudeAt(out.left, 440.0, 4800, 24'000);

    setParameter(*fm, "feedback", 80.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 24'000, note(57, 0));
    CHECK(amplitudeAt(out.left, 440.0, 4800, 24'000) > pure + 0.01); // feedback adds harmonics

    setParameter(*fm, "fon", 1.0f);
    setParameter(*fm, "cutoff", 300.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 24'000, note(57, 0));
    CHECK(amplitudeAt(out.left, 880.0, 4800, 24'000) < 0.01);

    setParameter(*fm, "dr", 50.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 48'000, note(57, 0, 4800));
    CHECK(rmsOf(out.left, 24'000, 48'000) < 1.0e-5);
    CHECK(fm->getActiveVoiceCount() == 0);
}

//==================================================================================================
// Impulse

TEST_CASE("Impulse plays each slot on its note, with transpose, start and decay", "[devices][impulse]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(1000.0, kRate, 9600, 0.5f), kRate);

    auto [node, impulse] = makeNode<Impulse>();
    juce::String error;
    REQUIRE(impulse->loadSlotSample(2, file, error));
    CHECK(impulse->getSlotSample(2) != nullptr);
    node->prepare(kRate, kBlock, nullptr);

    auto out = play(*node, 19'200, note(Impulse::kFirstNote + 2, 0));
    CHECK_THAT(frequencyEstimate(out.left, 100, 9000), WithinAbs(1000.0, 2.0));
    CHECK(rmsOf(out.left, 9700, 19'200) == 0.0); // the sample is over
    CHECK(impulse->takeSlotActivity(2) > 0.1f);

    out = play(*node, 4800, note(Impulse::kFirstNote, 0)); // slot 1 is empty
    CHECK(rmsOf(out.left, 0, 4800) == 0.0);

    setParameter(*impulse, "s3transpose", 12.0f);
    out = play(*node, 19'200, note(Impulse::kFirstNote + 2, 0));
    CHECK_THAT(frequencyEstimate(out.left, 100, 4500), WithinAbs(2000.0, 4.0));
    CHECK(rmsOf(out.left, 4850, 19'200) == 0.0); // twice as fast: half as long

    setParameter(*impulse, "s3transpose", 0.0f);
    setParameter(*impulse, "s3start", 50.0f);
    out = play(*node, 19'200, note(Impulse::kFirstNote + 2, 0));
    CHECK(rmsOf(out.left, 0, 4500) > 0.1);
    CHECK(rmsOf(out.left, 4850, 19'200) == 0.0);

    setParameter(*impulse, "s3start", 0.0f);
    setParameter(*impulse, "s3decay", 30.0f);
    out = play(*node, 9600, note(Impulse::kFirstNote + 2, 0));
    CHECK(rmsOf(out.left, 0, 480) > 0.1);
    CHECK(rmsOf(out.left, 4800, 9600) < 1.0e-3);
}

TEST_CASE("Impulse pan, filter, transient, drive and the 7/8 link", "[devices][impulse]")
{
    test::TempFolder temp;
    const auto tone = test::writeWav(temp.dir.getChildFile("tone.wav"), test::sine(1000.0, kRate, 24'000, 0.5f), kRate);
    const auto low = test::writeWav(temp.dir.getChildFile("low.wav"), test::sine(200.0, kRate, 24'000, 0.5f), kRate);

    auto [node, impulse] = makeNode<Impulse>();
    juce::String error;
    REQUIRE(impulse->loadSlotSample(0, tone, error));
    REQUIRE(impulse->loadSlotSample(6, tone, error));
    REQUIRE(impulse->loadSlotSample(7, low, error));
    // Every hit starts from silence (earlier hits may still be sounding).
    const auto hit = [&, n = node.get()](std::size_t frames, std::vector<TimedMidi> events)
    {
        n->prepare(kRate, kBlock, nullptr);
        return play(*n, frames, std::move(events));
    };

    setParameter(*impulse, "s1pan", -100.0f);
    auto out = hit(4800, note(Impulse::kFirstNote, 0));
    CHECK(rmsOf(out.right, 0, 4800) == 0.0);
    CHECK(rmsOf(out.left, 0, 4800) > 0.1);
    setParameter(*impulse, "s1pan", 0.0f);

    const double open = rmsOf(hit(9600, note(Impulse::kFirstNote, 0)).left, 2400, 9600);
    setParameter(*impulse, "s1filter", 1.0f);
    setParameter(*impulse, "s1cutoff", 150.0f);
    const double filtered = rmsOf(hit(9600, note(Impulse::kFirstNote, 0)).left, 2400, 9600);
    CHECK(filtered < open * 0.1);
    setParameter(*impulse, "s1filter", 0.0f);

    setParameter(*impulse, "s1transient", 100.0f);
    const double punchy = rmsOf(hit(9600, note(Impulse::kFirstNote, 0)).left, 0, 240);
    setParameter(*impulse, "s1transient", -100.0f);
    const double soft = rmsOf(hit(9600, note(Impulse::kFirstNote, 0)).left, 0, 240);
    CHECK(punchy > open * 1.5);
    CHECK(soft < open * 0.5);
    setParameter(*impulse, "s1transient", 0.0f);

    setParameter(*impulse, "s1drive", 100.0f);
    out = hit(9600, note(Impulse::kFirstNote, 0));
    CHECK(amplitudeAt(out.left, 3000.0, 1000, 9000) > 0.05); // odd harmonics from saturation
    CHECK(test::peak(out.left) <= 1.0001);

    // Slot 7 (open hat) is choked by slot 8 (closed hat) when linked.
    auto events = note(Impulse::kFirstNote + 6, 0);
    events.push_back({4800, juce::MidiMessage::noteOn(1, Impulse::kFirstNote + 7, 1.0f)});
    out = hit(14'400, events);
    CHECK(amplitudeAt(out.left, 1000.0, 7200, 14'400) < 0.001);
    CHECK(amplitudeAt(out.left, 200.0, 7200, 14'400) > 0.1);

    setParameter(*impulse, "link78", 0.0f);
    out = hit(14'400, events);
    CHECK(amplitudeAt(out.left, 1000.0, 7200, 14'400) > 0.1);
}

TEST_CASE("Impulse saves its samples and auditions slots", "[devices][impulse]")
{
    test::TempFolder temp;
    const auto file = test::writeWav(temp.dir.getChildFile("kick.wav"), test::sine(60.0, kRate, 4800, 0.8f), kRate);

    Impulse original;
    juce::String error;
    REQUIRE(original.loadSlotSample(4, file, error));
    juce::MemoryBlock state;
    original.getStateInformation(state);

    auto [node, copy] = makeNode<Impulse>();
    copy->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    REQUIRE(copy->getSlotSample(4) != nullptr);
    CHECK(copy->getSlotSample(4)->file == file);
    CHECK(copy->getSlotSample(0) == nullptr);

    copy->triggerSlot(4, 1.0f);
    const auto out = play(*node, 2400, {});
    CHECK(rmsOf(out.left, 0, 2400) > 0.1);
    CHECK(Impulse::getSlotNoteName(0) == juce::MidiMessage::getMidiNoteName(36, true, true, 3));
}

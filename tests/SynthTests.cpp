#include "devices/AnalogSynth.h"
#include "devices/DriftSynth.h"
#include "devices/Meld.h"
#include "EffectTestHelpers.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
double frequencyOf(double note)
{
    return 440.0 * std::exp2((note - 69.0) / 12.0);
}

std::vector<TimedMidi> note(int number, int on, int off = -1, float velocity = 1.0f, int channel = 1)
{
    std::vector<TimedMidi> events{{on, juce::MidiMessage::noteOn(channel, number, velocity)}};
    if (off >= 0)
        events.push_back({off, juce::MidiMessage::noteOff(channel, number)});
    return events;
}

std::vector<TimedMidi> operator+(std::vector<TimedMidi> a, const std::vector<TimedMidi>& b)
{
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

/** Plays `events` on a fresh instrument (no notes left over from before). */
Stereo play(ProcessorNode& node, double seconds, std::vector<TimedMidi> events)
{
    node.prepare(kRate, kBlock, nullptr);
    return run(node, Stereo(at(seconds)), nullptr, std::move(events));
}

int sampleAt(double seconds)
{
    return static_cast<int>(at(seconds));
}

double estimate(const std::vector<double>& x, double from, double to)
{
    return test::estimateFrequency(x, at(from), at(to), kRate);
}

/** Pitch by autocorrelation (for noisy, decaying tones), 50 Hz .. 1 kHz. */
double autocorrelationPitch(const std::vector<double>& x, double from, double to)
{
    const auto begin = at(from), end = at(to);
    const auto score = [&](std::size_t lag)
    {
        double ab = 0.0, aa = 0.0, bb = 0.0;
        for (std::size_t i = begin; i + lag < end; ++i)
        {
            ab += x[i] * x[i + lag];
            aa += x[i] * x[i];
            bb += x[i + lag] * x[i + lag];
        }
        return ab / std::sqrt(std::max(1.0e-30, aa * bb));
    };
    std::size_t best = 48;
    double bestScore = -2.0;
    for (std::size_t lag = 48; lag <= 960; ++lag)
        if (const double s = score(lag); s > bestScore)
        {
            bestScore = s;
            best = lag;
        }
    // The first peak nearly as good as the best (multiples of the period score almost the same).
    for (std::size_t lag = 49; lag < best; ++lag)
        if (const double s = score(lag); s > 0.9 * bestScore && s >= score(lag - 1) && s >= score(lag + 1))
        {
            best = lag;
            bestScore = s;
            break;
        }
    // Parabolic interpolation around the best lag.
    const double a = score(best - 1), b = bestScore, c = score(best + 1);
    const double curvature = a - 2.0 * b + c; // (negative at a peak)
    const double offset = curvature < -1.0e-12 ? 0.5 * (a - c) / curvature : 0.0;
    return kRate / (static_cast<double>(best) + offset);
}
} // namespace

//==================================================================================================
// Analog

TEST_CASE("Analog plays the note's pitch with band-limited oscillators", "[devices][analog]")
{
    auto [node, synth] = makeNode<AnalogSynth>();
    setParameter(*synth, "drift", 0.0f);
    setParameter(*synth, "o2on", 0.0f);
    setParameter(*synth, "f1cutoff", 20000.0f);
    setParameter(*synth, "f1env", 0.0f);
    setParameter(*synth, "f1key", 0.0f);
    setParameter(*synth, "f1res", 0.0f);

    auto out = play(*node, 0.5, note(69, 0));
    CHECK_THAT(estimate(out.left, 0.1, 0.5), WithinRel(440.0, 0.003));
    out = play(*node, 0.5, note(57, 0));
    CHECK_THAT(estimate(out.left, 0.1, 0.5), WithinRel(220.0, 0.003));

    // A high saw keeps its aliases (here harmonic 20 folded back to 4.74 kHz) far down.
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 0.5, note(100, 0));
    const double fundamental = amplitudeAt(out.left, frequencyOf(100), at(0.1), at(0.5));
    const double alias = amplitudeAt(out.left, 20.0 * frequencyOf(100) - kRate, at(0.1), at(0.5));
    CHECK(fundamental > 0.05);
    CHECK(toDb(alias / fundamental) < -40.0);
}

TEST_CASE("Analog filters, envelopes and voices", "[devices][analog]")
{
    auto [node, synth] = makeNode<AnalogSynth>();
    setParameter(*synth, "drift", 0.0f);
    setParameter(*synth, "o2on", 0.0f);
    setParameter(*synth, "f1env", 0.0f);
    setParameter(*synth, "f1key", 0.0f);

    // The 24 dB ladder at 300 Hz keeps the 110 Hz fundamental and takes the 10th harmonic away.
    setParameter(*synth, "f1cutoff", 300.0f);
    auto out = play(*node, 0.5, note(45, 0));
    const double h1 = amplitudeAt(out.left, 110.0, at(0.1), at(0.5));
    const double h10 = amplitudeAt(out.left, 1100.0, at(0.1), at(0.5));
    CHECK(toDb(h10 / h1) < -50.0);

    // The filter envelope opens it at the start of the note.
    setParameter(*synth, "f1env", 80.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 1.0, note(45, 0));
    CHECK(amplitudeAt(out.left, 1100.0, at(0.0), at(0.05)) > amplitudeAt(out.left, 1100.0, at(0.6), at(1.0)) * 4.0);

    // The amplifier's release ends the voice.
    setParameter(*synth, "ae1r", 50.0f);
    out = play(*node, 1.0, note(57, 0, sampleAt(0.3)));
    CHECK(rmsOf(out.left, at(0.1), at(0.3)) > 0.01);
    CHECK(rmsOf(out.left, at(0.6), at(1.0)) < 1.0e-6);
    CHECK(synth->getActiveVoiceCount() == 0);

    // Polyphony: a chord holds three voices.
    play(*node, 0.2, note(60, 0) + note(64, 0) + note(67, 0));
    CHECK(synth->getActiveVoiceCount() == 3);
}

TEST_CASE("Analog hard sync, mono legato with glide, and drift", "[devices][analog]")
{
    auto [node, synth] = makeNode<AnalogSynth>();
    setParameter(*synth, "drift", 0.0f);
    setParameter(*synth, "f1cutoff", 20000.0f);
    setParameter(*synth, "f1env", 0.0f);
    setParameter(*synth, "f1key", 0.0f);

    // Oscillator 2 a fifth up but synced to oscillator 1 repeats at oscillator 1's period.
    setParameter(*synth, "o1level", 0.0f);
    setParameter(*synth, "o2semi", 7.0f);
    setParameter(*synth, "o2detune", 0.0f);
    setParameter(*synth, "o2shape", 1.0f); // saw
    setParameter(*synth, "o2sync", 1.0f);
    auto out = play(*node, 0.5, note(57, 0));
    CHECK(amplitudeAt(out.left, 220.0, at(0.1), at(0.5)) > 0.02);
    CHECK(amplitudeAt(out.left, frequencyOf(64), at(0.1), at(0.5)) < amplitudeAt(out.left, 220.0, at(0.1), at(0.5)) * 0.5);
    setParameter(*synth, "o2sync", 0.0f);
    setParameter(*synth, "o2semi", 0.0f);
    setParameter(*synth, "o1level", 80.0f);
    setParameter(*synth, "o2on", 0.0f);

    // Mono with legato: the second key glides there without a new attack; letting it go returns
    // to the first key (last-note priority).
    setParameter(*synth, "voices", 0.0f);
    setParameter(*synth, "legato", 1.0f);
    setParameter(*synth, "glide", 50.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 1.2, note(57, 0, sampleAt(1.1)) + note(69, sampleAt(0.3), sampleAt(0.7)));
    CHECK(synth->getActiveVoiceCount() == 1);
    CHECK_THAT(estimate(out.left, 0.1, 0.3), WithinRel(220.0, 0.005));
    CHECK_THAT(estimate(out.left, 0.45, 0.7), WithinRel(440.0, 0.005));
    CHECK_THAT(estimate(out.left, 0.85, 1.1), WithinRel(220.0, 0.005));

    // Drift: the same note on new voices is never quite the same.
    setParameter(*synth, "voices", 3.0f);
    setParameter(*synth, "drift", 100.0f);
    setParameter(*synth, "glide", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    std::vector<double> pitches;
    for (int k = 0; k < 3; ++k)
    {
        out = play(*node, 0.6, note(69, 0, sampleAt(0.45)));
        pitches.push_back(estimate(out.left, 0.05, 0.4));
        CHECK_THAT(pitches.back(), WithinRel(440.0, 0.01));
    }
    CHECK(std::abs(pitches[0] - pitches[1]) + std::abs(pitches[1] - pitches[2]) > 0.2);
}

//==================================================================================================
// Drift

TEST_CASE("Drift: every oscillator shape, the Shape control and voice modes", "[devices][drift]")
{
    auto [node, synth] = makeNode<DriftSynth>();
    setParameter(*synth, "drift", 0.0f);
    setParameter(*synth, "o2level", 0.0f);
    setParameter(*synth, "freq", 20000.0f);
    setParameter(*synth, "fenv", 0.0f);
    setParameter(*synth, "key", 0.0f);
    setParameter(*synth, "m1amt", 0.0f);

    for (int shape = 0; shape < 6; ++shape)
    {
        setParameter(*synth, "o1shape", static_cast<float>(shape));
        const auto out = play(*node, 0.4, note(69, 0));
        INFO("shape " << shape);
        REQUIRE(allFinite(out));
        CHECK_THAT(estimate(out.left, 0.1, 0.4), WithinRel(440.0, 0.005));
    }

    // Pulse: a square at 0 (no even harmonics), narrow pulses with them.
    setParameter(*synth, "o1shape", 5.0f);
    auto out = play(*node, 0.4, note(57, 0));
    const double square = toDb(amplitudeAt(out.left, 440.0, at(0.1), at(0.4)) / amplitudeAt(out.left, 220.0, at(0.1), at(0.4)));
    setParameter(*synth, "o1amt", 80.0f);
    out = play(*node, 0.4, note(57, 0));
    const double narrow = toDb(amplitudeAt(out.left, 440.0, at(0.1), at(0.4)) / amplitudeAt(out.left, 220.0, at(0.1), at(0.4)));
    CHECK(square < -30.0);
    CHECK(narrow > -10.0);

    // Poly is centred; Stereo spreads two voices apart.
    setParameter(*synth, "o1amt", 0.0f);
    out = play(*node, 0.4, note(57, 0));
    double difference = 0.0;
    for (std::size_t i = 0; i < out.left.size(); ++i)
        difference = std::max(difference, std::abs(out.left[i] - out.right[i]));
    CHECK(difference < 1.0e-12);
    setParameter(*synth, "mode", 2.0f);
    setParameter(*synth, "spread", 100.0f);
    out = play(*node, 0.4, note(57, 0));
    difference = 0.0;
    for (std::size_t i = at(0.1); i < out.left.size(); ++i)
        difference = std::max(difference, std::abs(out.left[i] - out.right[i]));
    CHECK(difference > 0.01);
    setParameter(*synth, "mode", 3.0f); // unison
    REQUIRE(allFinite(play(*node, 0.3, note(57, 0))));
}

TEST_CASE("Drift MPE: each note bends on its own channel", "[devices][drift][mpe]")
{
    auto [node, synth] = makeNode<DriftSynth>();
    setParameter(*synth, "drift", 0.0f);
    setParameter(*synth, "o2level", 0.0f);
    setParameter(*synth, "o1shape", 0.0f); // sine
    setParameter(*synth, "freq", 20000.0f);
    setParameter(*synth, "fenv", 0.0f);
    for (int m = 1; m <= DriftSynth::kMatrixSlots; ++m)
        setParameter(*synth, "m" + juce::String(m) + "amt", 0.0f);

    // Two notes on member channels 2 and 3; channel 2 bends +2 semitones (of 48).
    const int bend = 8192 + static_cast<int>(std::lround(8192.0 * 2.0 / 48.0));
    auto events = note(60, 0, -1, 1.0f, 2) + note(72, 0, -1, 1.0f, 3);
    events.push_back({sampleAt(0.2), juce::MidiMessage::pitchWheel(2, bend)});
    auto out = play(*node, 0.6, events);
    CHECK(amplitudeAt(out.left, frequencyOf(62), at(0.3), at(0.6)) > 0.03);
    CHECK(amplitudeAt(out.left, frequencyOf(60), at(0.3), at(0.6)) < 0.005);
    CHECK(amplitudeAt(out.left, frequencyOf(72), at(0.3), at(0.6)) > 0.03);
    CHECK(amplitudeAt(out.left, frequencyOf(74), at(0.3), at(0.6)) < 0.005);

    // MPE off: the wheel moves every note (+-2 semitones range: the same value is +1/12 st).
    setParameter(*synth, "mpe", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    events = note(60, 0, -1, 1.0f, 1) + note(72, 0, -1, 1.0f, 1);
    events.push_back({sampleAt(0.2), juce::MidiMessage::pitchWheel(1, 8192 + 4096)}); // +1 semitone
    out = play(*node, 0.6, events);
    CHECK(amplitudeAt(out.left, frequencyOf(61), at(0.3), at(0.6)) > 0.03);
    CHECK(amplitudeAt(out.left, frequencyOf(73), at(0.3), at(0.6)) > 0.03);
}

TEST_CASE("Drift's cycling envelope loops while the note is held", "[devices][drift]")
{
    auto [node, synth] = makeNode<DriftSynth>();
    setParameter(*synth, "drift", 0.0f);
    setParameter(*synth, "e2cycle", 1.0f);
    setParameter(*synth, "e2a", 50.0f);
    setParameter(*synth, "e2d", 50.0f);
    setParameter(*synth, "fenv", 100.0f);
    setParameter(*synth, "freq", 300.0f);
    setParameter(*synth, "key", 0.0f);
    for (int m = 1; m <= DriftSynth::kMatrixSlots; ++m)
        setParameter(*synth, "m" + juce::String(m) + "amt", 0.0f);

    // Sample the filter frequency every block for a second: ten cycles.
    std::vector<float> cutoffs;
    auto events = note(57, 0);
    for (int block = 0; block < static_cast<int>(kRate) / kBlock; ++block)
    {
        run(*node, Stereo(kBlock), nullptr, block == 0 ? events : std::vector<TimedMidi>{});
        cutoffs.push_back(synth->getDisplayCutoff());
    }
    int peaks = 0;
    for (std::size_t i = 1; i + 1 < cutoffs.size(); ++i)
        if (cutoffs[i] > cutoffs[i - 1] && cutoffs[i] >= cutoffs[i + 1] && cutoffs[i] > 3000.0f)
            ++peaks;
    CHECK(peaks >= 9);
    CHECK(peaks <= 11);
}

//==================================================================================================
// Meld

TEST_CASE("Meld: every macro oscillator plays in both engines", "[devices][meld]")
{
    auto [node, synth] = makeNode<Meld>();
    const int count = Meld::getOscillatorNames().size();
    CHECK(count == 13);
    for (int osc = 0; osc < count; ++osc)
    {
        setParameter(*synth, "aosc", static_cast<float>(osc));
        setParameter(*synth, "bosc", static_cast<float>((osc + 5) % count));
        node->prepare(kRate, kBlock, nullptr);
        const auto out = play(*node, 0.6, note(57, 0, sampleAt(0.4), 0.9f) + note(64, sampleAt(0.05), sampleAt(0.4), 0.7f));
        INFO("oscillator " << Meld::getOscillatorNames()[osc]);
        REQUIRE(allFinite(out));
        CHECK(rmsOf(out.left, at(0.1), at(0.4)) > 0.002);
        CHECK(peakOf(out.left) < 2.0);
        const auto names = Meld::getMacroNames(static_cast<Meld::Oscillator>(osc));
        CHECK(names.first.isNotEmpty());
        CHECK(names.second.isNotEmpty());
    }
}

TEST_CASE("Meld pitch, harmonic FM and the pluck's decay", "[devices][meld]")
{
    auto [node, synth] = makeNode<Meld>();
    for (int m = 1; m <= Meld::kMatrixSlots; ++m)
        setParameter(*synth, "m" + juce::String(m) + "amt", 0.0f);
    setParameter(*synth, "bon", 0.0f);
    setParameter(*synth, "aosc", 0.0f); // Basic Shapes: sine
    setParameter(*synth, "am1", 0.0f);
    setParameter(*synth, "am2", 0.0f);
    setParameter(*synth, "afine", 0.0f);
    setParameter(*synth, "acutoff", 20000.0f);
    setParameter(*synth, "akey", 0.0f);
    auto out = play(*node, 0.5, note(69, 0));
    CHECK_THAT(estimate(out.left, 0.1, 0.5), WithinRel(440.0, 0.003));
    CHECK(amplitudeAt(out.left, 880.0, at(0.1), at(0.5)) < amplitudeAt(out.left, 440.0, at(0.1), at(0.5)) * 0.01);

    // Harmonic FM at ratio 2 with some index: sidebands at the odd harmonics.
    setParameter(*synth, "aosc", 2.0f);
    setParameter(*synth, "am1", 100.0f / 7.0f + 0.5f); // ratio round(1 + m1 x 7) = 2
    setParameter(*synth, "am2", 30.0f);
    out = play(*node, 0.5, note(57, 0));
    CHECK(amplitudeAt(out.left, 660.0, at(0.1), at(0.5)) > 0.01);
    CHECK_THAT(estimate(out.left, 0.1, 0.5), WithinRel(220.0, 0.01));

    // Pluck: a decaying string; Decay lets it ring longer.
    setParameter(*synth, "aosc", 6.0f);
    setParameter(*synth, "am1", 60.0f);
    setParameter(*synth, "am2", 0.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 1.5, note(57, 0));
    const double early = rmsOf(out.left, at(0.005), at(0.03));
    CHECK(early > 0.01);
    CHECK(rmsOf(out.left, at(0.5), at(1.0)) < early * 0.001); // Decay 0: about 0.15 s
    setParameter(*synth, "am2", 100.0f);
    out = play(*node, 1.5, note(57, 0));
    CHECK(rmsOf(out.left, at(1.0), at(1.5)) > rmsOf(out.left, at(0.005), at(0.03)) * 0.05); // ~10 s
    CHECK_THAT(autocorrelationPitch(out.left, 0.2, 0.6), WithinRel(220.0, 0.01));
}

TEST_CASE("Meld is bi-timbral: layer both engines or split the keyboard between them", "[devices][meld]")
{
    auto [node, synth] = makeNode<Meld>();
    for (int m = 1; m <= Meld::kMatrixSlots; ++m)
        setParameter(*synth, "m" + juce::String(m) + "amt", 0.0f);
    for (const auto* engine : {"a", "b"})
    {
        const juce::String e = engine;
        setParameter(*synth, e + "osc", 0.0f);
        setParameter(*synth, e + "m1", 0.0f);
        setParameter(*synth, e + "m2", 0.0f);
        setParameter(*synth, e + "fine", 0.0f);
        setParameter(*synth, e + "cutoff", 20000.0f);
        setParameter(*synth, e + "pan", 0.0f);
    }
    setParameter(*synth, "boct", 1.0f); // B an octave up

    // Layer: both on every note.
    auto out = play(*node, 0.5, note(57, 0));
    CHECK(amplitudeAt(out.left, 220.0, at(0.1), at(0.5)) > 0.02);
    CHECK(amplitudeAt(out.left, 440.0, at(0.1), at(0.5)) > 0.02);

    // Split at middle C: below it only A, from it up only B.
    setParameter(*synth, "mode", 1.0f);
    setParameter(*synth, "split", 60.0f);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 0.5, note(57, 0));
    CHECK(amplitudeAt(out.left, 220.0, at(0.1), at(0.5)) > 0.02);
    CHECK(amplitudeAt(out.left, 440.0, at(0.1), at(0.5)) < 0.001);
    node->prepare(kRate, kBlock, nullptr);
    out = play(*node, 0.5, note(64, 0));
    CHECK(amplitudeAt(out.left, frequencyOf(64), at(0.1), at(0.5)) < 0.001);
    CHECK(amplitudeAt(out.left, frequencyOf(76), at(0.1), at(0.5)) > 0.02);
}

TEST_CASE("Meld MPE: slide and pressure of a note drive its macros", "[devices][meld][mpe]")
{
    auto [node, synth] = makeNode<Meld>();
    for (int m = 1; m <= Meld::kMatrixSlots; ++m)
        setParameter(*synth, "m" + juce::String(m) + "amt", 0.0f);
    setParameter(*synth, "m1src", static_cast<float>(Meld::srcSlide));
    setParameter(*synth, "m1dst", static_cast<float>(Meld::destination(1, Meld::edMacro1)));
    setParameter(*synth, "m1amt", 100.0f);
    setParameter(*synth, "m2src", static_cast<float>(Meld::srcPressure));
    setParameter(*synth, "m2dst", static_cast<float>(Meld::destination(0, Meld::edMacro2)));
    setParameter(*synth, "m2amt", 50.0f);
    setParameter(*synth, "bm1", 0.0f);
    setParameter(*synth, "am2", 0.0f);

    auto events = note(60, 0, -1, 1.0f, 2);
    events.push_back({sampleAt(0.1), juce::MidiMessage::controllerEvent(2, 74, 127)});
    events.push_back({sampleAt(0.1), juce::MidiMessage::channelPressureChange(2, 127)});
    play(*node, 0.2, events);
    CHECK_THAT(synth->getDisplayMacro(1, 0), WithinAbs(1.0, 1e-6));
    CHECK_THAT(synth->getDisplayMacro(0, 1), WithinAbs(0.5, 1e-6));

    // Another channel's slide does not touch this note in MPE mode (the note still held).
    run(*node, Stereo(at(0.1)), nullptr, {{0, juce::MidiMessage::controllerEvent(3, 74, 0)}});
    CHECK_THAT(synth->getDisplayMacro(1, 0), WithinAbs(1.0, 1e-6));
}

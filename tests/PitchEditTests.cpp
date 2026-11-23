// Native pitch editing: analysis (melodic and polyphonic), resynthesis (pitch, drift, timing, level,
// formants), the session's pitch edits and the pitch editor.

#include "session/audioedit/PitchAnalysis.h"
#include "session/audioedit/PitchRender.h"
#include "session/audioedit/Stft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <numbers>
#include <vector>

using namespace dojo;
using namespace dojo::audioedit;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 44100.0;

/** A harmonic tone added to `x` (8 harmonics falling as 1/h, 10 ms attack, 20 ms release), with
    optional vibrato (cents, 5.5 Hz) and an amplitude envelope. */
void addTone(std::vector<float>& x, double start, double end, double midi, double amplitude = 0.3, double vibratoCents = 0.0,
             const std::function<double(double)>& envelope = {}, int harmonics = 8, const std::function<double(double)>& partialGain = {})
{
    double phase = 0.0;
    const auto first = static_cast<std::size_t>(start * kRate);
    const auto last = std::min(x.size(), static_cast<std::size_t>(end * kRate));
    for (std::size_t i = first; i < last; ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        const double f = midiToHz(midi + vibratoCents / 100.0 * std::sin(2.0 * std::numbers::pi * 5.5 * (t - start)));
        phase += 2.0 * std::numbers::pi * f / kRate;
        double v = 0.0;
        for (int h = 1; h <= harmonics; ++h)
            if (h * f < kRate * 0.45)
                v += (partialGain ? partialGain(h * f) : 1.0 / h) * std::sin(h * phase);
        const double attack = std::min(1.0, (t - start) / 0.01);
        const double release = std::min(1.0, (end - t) / 0.02);
        x[i] += static_cast<float>(amplitude * v * attack * release * (envelope ? envelope(t) : 1.0));
    }
}

/** The median pitch (MIDI) of [from, to) of mono audio, by YIN. */
double measuredPitch(const std::vector<float>& x, double from, double to)
{
    const auto first = static_cast<int>(from * kRate);
    const auto count = static_cast<int>((to - from) * kRate);
    const auto frames = pitch::trackPitch(x.data() + first, count, kRate);
    std::vector<double> midi;
    for (const auto& f : frames)
        if (f.f0 > 0.0f)
            midi.push_back(hzToMidi(f.f0));
    if (midi.empty())
        return 0.0;
    std::sort(midi.begin(), midi.end());
    return midi[midi.size() / 2];
}

double pitchSpread(const std::vector<float>& x, double from, double to)
{
    const auto frames = pitch::trackPitch(x.data() + static_cast<int>(from * kRate), static_cast<int>((to - from) * kRate), kRate);
    double lo = 200.0, hi = 0.0;
    for (const auto& f : frames)
        if (f.f0 > 0.0f)
        {
            lo = std::min(lo, hzToMidi(f.f0));
            hi = std::max(hi, hzToMidi(f.f0));
        }
    return hi - lo;
}

double rmsDb(const float* x, double from, double to)
{
    double sum = 0.0;
    const auto a = static_cast<std::size_t>(from * kRate), b = static_cast<std::size_t>(to * kRate);
    for (std::size_t i = a; i < b; ++i)
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return 10.0 * std::log10(sum / static_cast<double>(b - a) + 1.0e-20);
}

juce::AudioBuffer<float> toBuffer(const std::vector<float>& x)
{
    juce::AudioBuffer<float> b(1, static_cast<int>(x.size()));
    std::copy(x.begin(), x.end(), b.getWritePointer(0));
    return b;
}

std::vector<float> toVector(const juce::AudioBuffer<float>& b)
{
    return {b.getReadPointer(0), b.getReadPointer(0) + b.getNumSamples()};
}

std::vector<pitch::Edit> editsOf(const std::vector<pitch::Note>& notes)
{
    std::vector<pitch::Edit> edits;
    for (const auto& n : notes)
        edits.push_back(pitch::Edit::of(n));
    return edits;
}
} // namespace

TEST_CASE("Pitch analysis: YIN tracks a voice's f0 to a cent; silence has none", "[pitchedit][analysis]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 1.0), 0.0f);
    addTone(x, 0.2, 0.8, hzToMidi(220.0));
    const auto frames = pitch::trackPitch(x.data(), static_cast<int>(x.size()), kRate);
    REQUIRE(frames.size() == 200);
    CHECK(frames[10].f0 == 0.0f); // (0.05 s: silence)
    for (int i = 60; i < 140; ++i)
    {
        INFO("frame " << i);
        REQUIRE(frames[static_cast<std::size_t>(i)].f0 > 0.0f);
        CHECK(std::abs(hzToMidi(frames[static_cast<std::size_t>(i)].f0) - hzToMidi(220.0)) < 0.01);
        CHECK(frames[static_cast<std::size_t>(i)].clarity > 0.9f);
    }
    CHECK(frames[190].f0 == 0.0f);
    // Low and high voices.
    for (const double hz : {82.4, 659.3})
    {
        std::vector<float> y(static_cast<std::size_t>(kRate * 0.5), 0.0f);
        addTone(y, 0.0, 0.5, hzToMidi(hz));
        CHECK_THAT(measuredPitch(y, 0.1, 0.4), WithinAbs(hzToMidi(hz), 0.02));
    }
}

TEST_CASE("Pitch analysis, melodic: notes at pitch changes and new attacks, not at vibrato", "[pitchedit][analysis]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 3.2), 0.0f);
    // C4 then E4, legato (one continuous sound); a gap; G4 sung twice (a dip and a new attack);
    // A4 with a wide vibrato.
    addTone(x, 0.10, 0.62, 60.0);
    addTone(x, 0.60, 1.10, 64.0);
    addTone(x, 1.20, 2.00, 67.0, 0.3, 0.0, [](double t) { return t > 1.55 && t < 1.62 ? 0.05 : 1.0; });
    addTone(x, 2.10, 3.00, 69.0, 0.3, 60.0);
    const auto notes = pitch::analyse(x.data(), static_cast<int>(x.size()), kRate, pitch::Mode::melodic);
    REQUIRE(notes.size() == 5);
    const double expected[] = {60.0, 64.0, 67.0, 67.0, 69.0};
    const double starts[] = {0.10, 0.61, 1.20, 1.60, 2.10};
    for (std::size_t i = 0; i < 5; ++i)
    {
        INFO("note " << i);
        CHECK_THAT(notes[i].pitch, WithinAbs(expected[i], 0.1));
        CHECK_THAT(notes[i].start, WithinAbs(starts[i], 0.04));
        CHECK(notes[i].end > notes[i].start + 0.2);
        CHECK(notes[i].drift.size() == notes[i].level.size());
    }
    // The vibrato is the A4's drift (+-0.6 semitones), not more notes.
    double lo = 0.0, hi = 0.0;
    for (const float d : notes[4].drift)
    {
        lo = std::min(lo, static_cast<double>(d));
        hi = std::max(hi, static_cast<double>(d));
    }
    CHECK_THAT(hi, WithinAbs(0.6, 0.12));
    CHECK_THAT(lo, WithinAbs(-0.6, 0.12));
    CHECK(notes[4].levelAt(2.5) > 0.5);
    CHECK_THAT(notes[4].driftAt(2.1 + 1.0 / 22.0), WithinAbs(0.6, 0.15)); // (a quarter vibrato cycle in)
}

TEST_CASE("Pitch analysis, polyphonic: the notes of chords", "[pitchedit][analysis]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 2.2), 0.0f);
    for (const double m : {60.0, 64.0, 67.0})
        addTone(x, 0.1, 1.0, m, 0.2);
    for (const double m : {65.0, 69.0, 72.0})
        addTone(x, 1.1, 2.0, m, 0.2);
    addTone(x, 0.1, 2.0, 48.0, 0.2);
    const auto notes = pitch::analyse(x.data(), static_cast<int>(x.size()), kRate, pitch::Mode::polyphonic);
    std::vector<int> first, second, whole;
    for (const auto& n : notes)
    {
        INFO("note " << n.pitch << " " << n.start << "-" << n.end);
        CHECK(std::abs(n.pitch - std::round(n.pitch)) < 0.15);
        const int m = static_cast<int>(std::lround(n.pitch));
        if (n.end - n.start > 1.5)
            whole.push_back(m);
        else if (n.start < 0.6)
            first.push_back(m);
        else
            second.push_back(m);
    }
    std::sort(first.begin(), first.end());
    std::sort(second.begin(), second.end());
    CHECK(whole == std::vector<int>{48});
    CHECK(first == std::vector<int>{60, 64, 67});
    CHECK(second == std::vector<int>{65, 69, 72});
    for (const auto& n : notes)
        if (n.end - n.start < 1.5)
        {
            CHECK_THAT(n.start, WithinAbs(n.start < 0.6 ? 0.1 : 1.1, 0.06));
            CHECK_THAT(n.end, WithinAbs(n.start < 0.6 ? 1.0 : 2.0, 0.08));
        }
}

TEST_CASE("Pitch rendering, melodic: a note moved, its drift flattened, its level and timing changed; the rest untouched", "[pitchedit][render]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 2.4), 0.0f);
    addTone(x, 0.20, 1.00, 69.0, 0.3, 50.0);
    addTone(x, 1.30, 2.10, 76.0);
    const auto notes = pitch::analyse(x.data(), static_cast<int>(x.size()), kRate, pitch::Mode::melodic);
    REQUIRE(notes.size() == 2);
    const auto input = toBuffer(x);
    pitch::Renderer renderer;
    pitch::RenderOptions options;
    juce::AudioBuffer<float> output;

    // Nothing edited: the input, sample for sample.
    auto edits = editsOf(notes);
    REQUIRE(renderer.render(input, kRate, edits, options, output));
    CHECK(renderer.getSpansRendered() == 0);
    for (int i = 0; i < input.getNumSamples(); ++i)
        REQUIRE(output.getSample(0, i) == input.getSample(0, i));

    // Up a whole tone: B4, vibrato and level kept; the E5 untouched.
    edits[0].newPitch = notes[0].pitch + 2.0;
    REQUIRE(renderer.render(input, kRate, edits, options, output));
    CHECK(renderer.getSpansRendered() == 1);
    auto y = toVector(output);
    CHECK_THAT(measuredPitch(y, 0.35, 0.9), WithinAbs(71.0, 0.12));
    CHECK_THAT(pitchSpread(y, 0.35, 0.9), WithinAbs(pitchSpread(x, 0.35, 0.9), 0.2));
    CHECK_THAT(rmsDb(y.data(), 0.35, 0.9), WithinAbs(rmsDb(x.data(), 0.35, 0.9), 1.5));
    for (int i = static_cast<int>(1.2 * kRate); i < input.getNumSamples(); ++i)
        REQUIRE(output.getSample(0, i) == input.getSample(0, i));
    CHECK_THAT(measuredPitch(y, 1.4, 2.0), WithinAbs(76.0, 0.05));

    // Rendered again: from the cache.
    REQUIRE(renderer.render(input, kRate, edits, options, output));
    CHECK(renderer.getSpansRendered() == 0);
    CHECK_THAT(measuredPitch(toVector(output), 0.35, 0.9), WithinAbs(71.0, 0.12));

    // Drift flattened: the vibrato gone, the centre kept.
    edits[0].drift = 0.0;
    REQUIRE(renderer.render(input, kRate, edits, options, output));
    y = toVector(output);
    CHECK(pitchSpread(y, 0.35, 0.9) < 0.15);
    CHECK(pitchSpread(x, 0.35, 0.9) > 0.9);
    CHECK_THAT(measuredPitch(y, 0.35, 0.9), WithinAbs(71.0, 0.08));

    // Level: -6 dB.
    edits = editsOf(notes);
    edits[1].gainDb = -6.0;
    REQUIRE(renderer.render(input, kRate, edits, options, output));
    y = toVector(output);
    CHECK_THAT(rmsDb(y.data(), 1.45, 2.0) - rmsDb(x.data(), 1.45, 2.0), WithinAbs(-6.0, 0.6));
    CHECK_THAT(measuredPitch(y, 1.45, 2.0), WithinAbs(76.0, 0.05));

    // Timing: the E5 starts 100 ms later (the gap before it stretched), ends where it did.
    edits = editsOf(notes);
    edits[1].newStart = notes[1].start + 0.1;
    REQUIRE(renderer.render(input, kRate, edits, options, output));
    y = toVector(output);
    CHECK(rmsDb(y.data(), 1.32, 1.38) < rmsDb(x.data(), 1.32, 1.38) - 20.0); // (still the gap)
    CHECK_THAT(rmsDb(y.data(), 1.5, 2.0), WithinAbs(rmsDb(x.data(), 1.5, 2.0), 1.5));
    CHECK_THAT(measuredPitch(y, 1.5, 2.0), WithinAbs(76.0, 0.05));
}

TEST_CASE("Pitch rendering keeps a voice's formants where they were", "[pitchedit][render]")
{
    // A 150 Hz voice with a resonance at 1 kHz, moved up five semitones.
    const auto formant = [](double hz) { return std::exp(-std::pow((hz - 1000.0) / 250.0, 2.0)) + 0.03; };
    std::vector<float> x(static_cast<std::size_t>(kRate * 1.2), 0.0f);
    addTone(x, 0.1, 1.1, hzToMidi(150.0), 0.3, 0.0, {}, 26, formant);
    const auto notes = pitch::analyse(x.data(), static_cast<int>(x.size()), kRate, pitch::Mode::melodic);
    REQUIRE(notes.size() == 1);
    auto edits = editsOf(notes);
    edits[0].newPitch = notes[0].pitch + 5.0;

    const auto strongestPartial = [](const juce::AudioBuffer<float>& b)
    {
        RealFft fft(15);
        std::vector<float> frame(static_cast<std::size_t>(fft.size()), 0.0f);
        const auto window = hannWindow(fft.size());
        const int first = static_cast<int>(0.3 * kRate);
        for (int i = 0; i < fft.size() && first + i < b.getNumSamples(); ++i)
            frame[static_cast<std::size_t>(i)] = b.getSample(0, first + i) * window[static_cast<std::size_t>(i)];
        std::vector<std::complex<float>> spectrum(static_cast<std::size_t>(fft.bins()));
        fft.forward(frame.data(), spectrum.data());
        int best = 1;
        for (int k = 1; k < fft.bins(); ++k)
            if (std::abs(spectrum[static_cast<std::size_t>(k)]) > std::abs(spectrum[static_cast<std::size_t>(best)]))
                best = k;
        return best * kRate / fft.size();
    };
    pitch::Renderer renderer;
    juce::AudioBuffer<float> kept, moved;
    pitch::RenderOptions options;
    REQUIRE(renderer.render(toBuffer(x), kRate, edits, options, kept));
    options.preserveFormants = false;
    REQUIRE(renderer.render(toBuffer(x), kRate, edits, options, moved));
    CHECK_THAT(measuredPitch(toVector(kept), 0.3, 1.0), WithinAbs(hzToMidi(150.0) + 5.0, 0.1));
    CHECK_THAT(measuredPitch(toVector(moved), 0.3, 1.0), WithinAbs(hzToMidi(150.0) + 5.0, 0.1));
    CHECK(std::abs(strongestPartial(kept) - 1000.0) < 120.0);         // the resonance stayed
    CHECK(std::abs(strongestPartial(moved) - 1000.0 * 1.335) < 160.0); // (it moved with the pitch)
}

TEST_CASE("Pitch rendering, polyphonic: one note of a chord moved alone", "[pitchedit][render]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 1.4), 0.0f);
    for (const double m : {60.0, 64.0, 67.0})
        addTone(x, 0.1, 1.3, m, 0.2);
    const auto notes = pitch::analyse(x.data(), static_cast<int>(x.size()), kRate, pitch::Mode::polyphonic);
    REQUIRE(notes.size() == 3);
    auto edits = editsOf(notes);
    for (auto& e : edits)
        if (std::lround(e.note.pitch) == 64)
            e.newPitch = e.note.pitch + 1.0; // E4 -> F4
    pitch::Renderer renderer;
    pitch::RenderOptions options;
    options.mode = pitch::Mode::polyphonic;
    juce::AudioBuffer<float> output;
    REQUIRE(renderer.render(toBuffer(x), kRate, edits, options, output));
    const auto y = toVector(output);
    const auto again = pitch::analyse(y.data(), static_cast<int>(y.size()), kRate, pitch::Mode::polyphonic);
    std::vector<int> pitches;
    for (const auto& n : again)
        if (n.end - n.start > 0.5)
            pitches.push_back(static_cast<int>(std::lround(n.pitch)));
    std::sort(pitches.begin(), pitches.end());
    CHECK(pitches == std::vector<int>{60, 65, 67});
    CHECK_THAT(rmsDb(y.data(), 0.3, 1.1), WithinAbs(rmsDb(x.data(), 0.3, 1.1), 1.5));
}

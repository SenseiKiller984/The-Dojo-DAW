// Native pitch editing: analysis (melodic and polyphonic), resynthesis (pitch, drift, timing, level,
// formants), the session's pitch edits and the pitch editor.

#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/AudioEdits.h"
#include "session/Project.h"
#include "session/audioedit/PitchAnalysis.h"
#include "session/audioedit/PitchRender.h"
#include "session/audioedit/Stft.h"
#include "ui/EditorState.h"
#include "ui/components/ClipEditor.h"
#include "ui/components/PitchEditor.h"
#include "TestAudio.h"

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

namespace
{
/** A project with the engine, for the session and UI tests. */
struct PitchApp
{
    struct MessageThread
    {
        MessageThread() { juce::MessageManager::getInstance(); }
    } messageThread;
    engine::AudioEngine engine;
    engine::AudioDeviceHost host{engine};
    engine::EngineController controller{engine, host};
    session::Project project{[this](std::unique_ptr<engine::RenderGraph> graph) { controller.publishGraph(std::move(graph)); },
                             [](const juce::ValueTree& d, double, int, juce::String&) { return devices::createBuiltinDevice(session::device::getType(d)); }};
};

std::vector<float> readFile(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
        return {};
    juce::AudioBuffer<float> b(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&b, 0, b.getNumSamples(), 0, true, true);
    return toVector(b);
}

/** A vocal-like take: A4 (with vibrato) then E5. */
juce::File writeTake(const juce::File& file)
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 2.4), 0.0f);
    addTone(x, 0.20, 1.00, 69.0, 0.3, 40.0);
    addTone(x, 1.30, 2.10, 76.0);
    return test::writeWav(file, x, kRate);
}
} // namespace

TEST_CASE("Pitch edits in the session: analysed, edited, rendered and played; undone, bypassed, reopened, removed", "[pitchedit][session]")
{
    test::TempFolder temp;
    PitchApp app;
    auto& session = app.project.getSession();
    auto& edits = app.project.getAudioEdits();
    auto& undo = session.getUndoManager();
    edits.setRenderFolder(temp.dir.getChildFile("Edits"));
    const auto source = writeTake(temp.dir.getChildFile("take.wav"));
    auto track = session.addTrack(session::TrackKind::audio, "Vocal");
    auto clip = session.addClip(track, source, 1.0, 2.4);

    juce::String error;
    REQUIRE(edits.analysePitch(clip, pitch::Mode::melodic, error));
    CHECK(edits.getState(clip).status == session::AudioEdits::Status::analysing);
    REQUIRE(edits.waitUntilIdle(30'000));
    auto notes = session::audioedits::getPitchNotes(clip);
    REQUIRE(notes.size() == 2);
    CHECK(undo.getUndoDescription() == "Analyse Pitch");
    CHECK(session::clip::getFile(clip) == source); // (nothing edited: the source plays)
    CHECK(session::audioedits::getSource(clip) == source);
    CHECK(session::audioedits::getPitchMode(clip) == pitch::Mode::melodic);
    const auto first = session::audioedits::toEdit(notes[0]);
    CHECK_THAT(first.note.pitch, WithinAbs(69.0, 0.1));
    CHECK_THAT(first.note.start, WithinAbs(0.2, 0.03)); // (seconds of the source from `from`, here 0)
    CHECK_THAT(session::audioedits::clipTimeOf(clip, first.note.start), WithinAbs(0.2, 0.03));
    CHECK(first.note.drift.size() > 100);

    // An edit renders: the clip plays a new file, the A4 now a B4; the rest as it was.
    session.beginTransaction("Change Pitch");
    session::audioedits::setNotePitch(notes[0], first.note.pitch + 2.0, &undo);
    CHECK(edits.getState(clip).status == session::AudioEdits::Status::rendering);
    REQUIRE(edits.waitUntilIdle(30'000));
    const auto rendered = session::clip::getFile(clip);
    CHECK(rendered != source);
    CHECK(rendered.isAChildOf(temp.dir.getChildFile("Edits")));
    CHECK(session::audioedits::getEdit(clip).getProperty(session::ids::rendered).toString() == rendered.getFullPathName());
    auto y = readFile(rendered);
    const auto x = readFile(source);
    REQUIRE(y.size() == x.size());
    CHECK_THAT(measuredPitch(y, 0.35, 0.9), WithinAbs(71.0, 0.15));
    CHECK_THAT(measuredPitch(y, 1.4, 2.0), WithinAbs(76.0, 0.05));
    CHECK(app.project.getAssets().find(rendered) != nullptr); // (loaded before it was played)

    // Undo: the source plays again, the render is deleted; redo renders again.
    REQUIRE(undo.undo());
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) == source);
    CHECK_FALSE(rendered.existsAsFile());
    REQUIRE(undo.redo());
    REQUIRE(edits.waitUntilIdle(30'000));
    const auto again = session::clip::getFile(clip);
    CHECK(again != source);
    CHECK_THAT(measuredPitch(readFile(again), 0.35, 0.9), WithinAbs(71.0, 0.15));

    // Original (A/B): the source while bypassed.
    edits.setBypassed(clip, true);
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) == source);
    edits.setBypassed(clip, false);
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) != source);

    // Saved and opened again: with its render gone, the clip plays its source and renders again.
    const auto song = temp.dir.getChildFile("Song.dojo");
    REQUIRE(app.project.saveAs(song).wasOk());
    CHECK(edits.getRenderFolder() == temp.dir.getChildFile("Song Edits"));
    const auto saved = session::clip::getFile(clip);
    REQUIRE(saved.deleteFile());
    REQUIRE(app.project.open(song).wasOk());
    auto reopened = session.findClip(session::clip::getId(clip));
    REQUIRE(reopened.isValid());
    CHECK(session::clip::getFile(reopened) == source);
    CHECK_FALSE(app.project.hasUnsavedChanges());
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(reopened) != source);
    CHECK(session::clip::getFile(reopened).isAChildOf(temp.dir.getChildFile("Song Edits")));
    CHECK_THAT(measuredPitch(readFile(session::clip::getFile(reopened)), 0.35, 0.9), WithinAbs(71.0, 0.15));

    // Removed (one undo step): the source plays; undone, the edits are back and render.
    edits.removePitch(reopened);
    CHECK(undo.getUndoDescription() == "Remove Pitch Edits");
    CHECK(session::clip::getFile(reopened) == source);
    CHECK_FALSE(session::audioedits::getEdit(reopened).isValid());
    REQUIRE(edits.waitUntilIdle(30'000));
    REQUIRE(undo.undo());
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::audioedits::hasPitch(reopened));
    CHECK(session::clip::getFile(reopened) != source);
    CHECK(session::clip::getFile(reopened).existsAsFile());
}

TEST_CASE("Pitch edit model: timing keeps notes apart (and touching ones touching), scales, correction", "[pitchedit][model]")
{
    juce::ValueTree edit(session::ids::audioEdit);
    edit.setProperty(session::ids::from, 0.0, nullptr);
    edit.setProperty(session::ids::to, 4.0, nullptr);
    juce::ValueTree p(session::ids::pitchEdit);
    edit.appendChild(p, nullptr);
    const auto add = [&p](double start, double end, double midi)
    {
        pitch::Note n;
        n.start = start;
        n.end = end;
        n.pitch = midi;
        n.drift = {0.3f, -0.3f};
        n.level = {1.0f, 1.0f};
        auto tree = session::audioedits::makeNote(n);
        p.appendChild(tree, nullptr);
        return tree;
    };
    auto n0 = add(0.0, 1.0, 60.2);
    auto n1 = add(1.0, 2.0, 63.6); // touches n0
    auto n2 = add(2.5, 3.0, 67.0);
    juce::UndoManager um;

    // Moved later: n0's end follows (they touch); stopped at n2.
    session::audioedits::setNoteTiming(n1, 1.3, 2.3, &um);
    auto e1 = session::audioedits::toEdit(n1);
    CHECK_THAT(e1.newStart, WithinAbs(1.3, 1e-9));
    CHECK_THAT(e1.newEnd, WithinAbs(2.3, 1e-9));
    CHECK_THAT(session::audioedits::toEdit(n0).newEnd, WithinAbs(1.3, 1e-9));
    session::audioedits::setNoteTiming(n1, 2.0, 3.0, &um);
    e1 = session::audioedits::toEdit(n1);
    CHECK_THAT(e1.newStart, WithinAbs(1.5, 1e-9)); // (its length kept, up to n2)
    CHECK_THAT(e1.newEnd, WithinAbs(2.5, 1e-9));
    // An end dragged: not past n2, not shorter than 20 ms.
    session::audioedits::setNoteTiming(n1, 1.5, 2.9, &um);
    CHECK_THAT(session::audioedits::toEdit(n1).newEnd, WithinAbs(2.5, 1e-9));
    session::audioedits::setNoteTiming(n1, 1.5, 1.0, &um);
    CHECK_THAT(session::audioedits::toEdit(n1).newEnd, WithinAbs(1.52, 1e-9));
    // A start dragged before n0's own start is stopped 20 ms after it.
    session::audioedits::setNoteTiming(n1, -1.0, 2.0, &um);
    CHECK_THAT(session::audioedits::toEdit(n1).newStart, WithinAbs(0.02, 1e-9));
    CHECK_THAT(session::audioedits::toEdit(n0).newEnd, WithinAbs(0.02, 1e-9));
    session::audioedits::resetNote(n1, &um);
    CHECK_FALSE(session::audioedits::toEdit(n1).timeEdited());

    // Scales: the nearest note (ties go down).
    using session::audioedits::ScaleKind;
    CHECK(session::audioedits::nearestInScale(61.0, {0, ScaleKind::major}) == 60);
    CHECK(session::audioedits::nearestInScale(61.4, {0, ScaleKind::chromatic}) == 61);
    CHECK(session::audioedits::nearestInScale(65.9, {0, ScaleKind::major}) == 65);
    CHECK(session::audioedits::nearestInScale(66.6, {0, ScaleKind::major}) == 67);
    CHECK(session::audioedits::nearestInScale(63.0, {9, ScaleKind::naturalMinor}) == 62); // A minor: D (ties down), not D#
    CHECK(session::audioedits::nearestInScale(68.0, {9, ScaleKind::harmonicMinor}) == 68);  // G# in A harmonic minor
    CHECK(session::audioedits::nearestInScale(65.0, {0, ScaleKind::majorPentatonic}) == 64);

    // Correction: halfway to the scale, the drift halved; then all the way, flat.
    session::audioedits::correctPitch({n0, n1}, 0.5, 0.5, {0, ScaleKind::major}, &um);
    CHECK_THAT(session::audioedits::toEdit(n0).newPitch, WithinAbs(60.1, 1e-9));
    CHECK_THAT(session::audioedits::toEdit(n1).newPitch, WithinAbs(63.8, 1e-9)); // (towards E)
    CHECK_THAT(session::audioedits::toEdit(n0).drift, WithinAbs(0.5, 1e-9));
    session::audioedits::correctPitch({n0, n1}, 1.0, 1.0, {0, ScaleKind::major}, &um);
    CHECK_THAT(session::audioedits::toEdit(n0).newPitch, WithinAbs(60.0, 1e-9));
    CHECK_THAT(session::audioedits::toEdit(n1).newPitch, WithinAbs(64.0, 1e-9));
    CHECK_THAT(session::audioedits::toEdit(n1).drift, WithinAbs(0.0, 1e-9));
    CHECK(session::audioedits::toEdit(n0).pitchEdited());
    CHECK_THAT(session::audioedits::toEdit(n0).offsetAt(0.0), WithinAbs(-0.2 - 0.3, 1e-6)); // (to C, the drift flattened)
}

TEST_CASE("The pitch editor: blobs dragged in pitch and time, edges, keys, snapping, correction; the clip editor's PITCH view", "[pitchedit][ui]")
{
    test::TempFolder temp;
    PitchApp app;
    auto& session = app.project.getSession();
    auto& edits = app.project.getAudioEdits();
    auto& undo = session.getUndoManager();
    edits.setRenderFolder(temp.dir.getChildFile("Edits"));
    const auto source = writeTake(temp.dir.getChildFile("take.wav"));
    auto track = session.addTrack(session::TrackKind::audio, "Vocal");
    auto clip = session.addClip(track, source, 0.0, 2.4);

    ui::EditorState editor;
    ui::ClipEditor clipEditor(app.project, editor);
    clipEditor.setSize(1400, 420);
    editor.setEditedClip(session::clip::getId(clip));
    auto* pitchButton = [&]() -> juce::Button*
    {
        for (auto* child : clipEditor.getChildren())
            if (auto* b = dynamic_cast<juce::Button*>(child); b != nullptr && b->getButtonText() == "PITCH")
                return b;
        return nullptr;
    }();
    REQUIRE(pitchButton != nullptr);
    CHECK(pitchButton->isVisible());
    CHECK_FALSE(clipEditor.getPitchEditor().isVisible());
    const auto click = [](juce::Button& b)
    {
        b.triggerClick(); // (asynchronous)
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
    };
    click(*pitchButton);
    auto& view = clipEditor.getPitchEditor();
    CHECK(view.isVisible());
    CHECK(view.getClip() == clip);
    const auto paint = [&clipEditor]
    {
        juce::Image image(juce::Image::RGB, clipEditor.getWidth(), clipEditor.getHeight(), true);
        juce::Graphics g(image);
        clipEditor.paintEntireComponent(g, false);
        return image;
    };
    paint();

    // Analyse from the editor.
    click(view.getAnalyseButton());
    REQUIRE(edits.waitUntilIdle(30'000));
    const auto notes = session::audioedits::getPitchNotes(clip);
    REQUIRE(notes.size() == 2);
    paint();

    const auto event = [&view](juce::Point<float> p, juce::Point<float> down, juce::ModifierKeys extra = {}, int clicks = 1)
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p,
                                juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | extra.getRawFlags()), juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &view, &view, now, down, now, clicks,
                                p != down);
    };
    const auto drag = [&](juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
    {
        view.mouseDown(event(from, from, mods));
        view.mouseDrag(event(from + (to - from) * 0.5f, from, mods));
        view.mouseDrag(event(to, from, mods));
        view.mouseUp(event(to, from, mods));
    };

    // The blob is where the note is heard.
    auto a4 = notes[0];
    const auto e0 = session::audioedits::toEdit(a4);
    auto bounds = view.getNoteBounds(a4);
    CHECK_THAT(static_cast<double>(bounds.getX()), WithinAbs(static_cast<double>(view.timeToX(e0.note.start)), 0.5));
    CHECK_THAT(view.yToPitch(bounds.getCentreY()), WithinAbs(e0.note.pitch, 0.01));
    CHECK(view.noteAt(bounds.getCentre()) == a4);

    // Up two semitones (one step), selected.
    const auto centre = bounds.getCentre();
    drag(centre, centre.translated(0.0f, -2.0f * (view.pitchToY(60.0) - view.pitchToY(61.0))));
    CHECK_THAT(session::audioedits::toEdit(a4).newPitch, WithinAbs(e0.note.pitch + 2.0, 1e-9));
    CHECK(undo.getUndoDescription() == "Change Pitch");
    CHECK(view.getSelection() == std::vector<juce::ValueTree>{a4});
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) != source);
    // DOJO_PITCH_SNAPSHOT=<file.png>: the editor with the moved note, for a look.
    if (const auto path = juce::SystemStats::getEnvironmentVariable("DOJO_PITCH_SNAPSHOT", {}); path.isNotEmpty())
    {
        view.setPlayhead(0.6);
        const auto image = paint();
        const juce::File file(path);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(image, out);
    }
    paint();

    // Sideways: later in time (melodic), one step.
    bounds = view.getNoteBounds(a4);
    drag(bounds.getCentre(), bounds.getCentre().translated(static_cast<float>(view.timeToX(0.1) - view.timeToX(0.0)), 0.0f));
    auto moved = session::audioedits::toEdit(a4);
    CHECK_THAT(moved.newStart, WithinAbs(e0.note.start + 0.1, 0.002));
    CHECK_THAT(moved.newEnd, WithinAbs(e0.note.end + 0.1, 0.002));
    CHECK(undo.getUndoDescription() == "Move Notes");
    undo.undo();

    // Its end dragged earlier.
    bounds = view.getNoteBounds(a4);
    drag({bounds.getRight() - 2.0f, bounds.getCentreY()}, {bounds.getRight() - 2.0f - static_cast<float>(view.timeToX(0.2) - view.timeToX(0.0)), bounds.getCentreY()});
    CHECK_THAT(session::audioedits::toEdit(a4).newEnd, WithinAbs(e0.note.end - 0.2, 0.002));
    CHECK(undo.getUndoDescription() == "Change Note End");

    // Keys: Up a semitone, Shift+Down 10 cents; Delete resets.
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::upKey)));
    CHECK_THAT(session::audioedits::toEdit(a4).newPitch, WithinAbs(e0.note.pitch + 3.0, 1e-9));
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::downKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK_THAT(session::audioedits::toEdit(a4).newPitch, WithinAbs(e0.note.pitch + 2.9, 1e-9));
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK_FALSE(session::audioedits::toEdit(a4).edited());
    CHECK(undo.getUndoDescription() == "Reset Note");

    // Double-click: snapped to the nearest note of the scale.
    bounds = view.getNoteBounds(a4);
    view.mouseDoubleClick(event(bounds.getCentre(), bounds.getCentre(), {}, 2));
    CHECK_THAT(session::audioedits::toEdit(a4).newPitch, WithinAbs(std::round(e0.note.pitch), 1e-9));
    CHECK(undo.getUndoDescription() == "Snap Pitch");

    // Lasso both; Correct (to D major: the E5 stays, the A4 too); drift and level of the selection.
    const auto area = view.getNoteArea().toFloat();
    drag(area.getTopLeft().translated(2.0f, 2.0f), area.getBottomRight().translated(-2.0f, -2.0f));
    CHECK(view.getSelection().size() == 2);
    view.setScale({2, session::audioedits::ScaleKind::major});
    view.correct(1.0, 1.0);
    CHECK(undo.getUndoDescription() == "Correct Pitch");
    for (const auto& n : notes)
    {
        const auto e = session::audioedits::toEdit(n);
        CHECK_THAT(e.newPitch, WithinAbs(std::round(e.note.pitch), 1e-9));
        CHECK_THAT(e.drift, WithinAbs(0.0, 1e-9));
    }
    session.beginTransaction("Change Note Level");
    view.setGainOfSelection(-6.0);
    CHECK_THAT(session::audioedits::toEdit(notes[1]).gainDb, WithinAbs(-6.0, 1e-9));
    REQUIRE(edits.waitUntilIdle(30'000));
    const auto y = readFile(session::clip::getFile(clip));
    CHECK(pitchSpread(y, 0.35, 0.9) < 0.15); // (the vibrato straightened)
    paint();

    // Formants and Original toggle the clip's settings.
    click(view.getFormantsButton());
    CHECK_FALSE(session::audioedits::keepsFormants(clip));
    CHECK(undo.getUndoDescription() == "Keep Formants");
    click(view.getOriginalButton());
    CHECK(session::AudioEdits::isBypassed(clip));
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) == source);
    click(view.getOriginalButton());
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) != source);

    // CLIP: the inspector again.
    for (auto* child : clipEditor.getChildren())
        if (auto* b = dynamic_cast<juce::Button*>(child); b != nullptr && b->getButtonText() == "CLIP")
            click(*b);
    CHECK_FALSE(view.isVisible());
}

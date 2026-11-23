// Spectral editing: region gain and erase, repair, de-noise, de-hum, the spectrogram; the session's
// spectral edits and the spectral editor.

#include "devices/DeviceRegistry.h"
#include "engine/AudioEngine.h"
#include "engine/EngineController.h"
#include "engine/io/AudioDeviceHost.h"
#include "session/AudioEdits.h"
#include "session/Project.h"
#include "session/audioedit/PitchAnalysis.h"
#include "session/audioedit/Spectral.h"
#include "session/audioedit/Stft.h"
#include "ui/EditorState.h"
#include "ui/components/ClipEditor.h"
#include "ui/components/SpectralEditor.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <numbers>
#include <random>
#include <vector>

using namespace dojo;
using namespace dojo::audioedit;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kRate = 44100.0;

void addSine(std::vector<float>& x, double hz, double amplitude, double from = 0.0, double to = 1.0e9)
{
    const auto a = static_cast<std::size_t>(from * kRate);
    const auto b = std::min(x.size(), static_cast<std::size_t>(to * kRate));
    for (std::size_t i = a; i < b; ++i)
        x[i] += static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
}

/** The level (dB) of one frequency in [from, to) (Goertzel, Hann-windowed). */
double levelDb(const float* x, double hz, double from, double to)
{
    const auto a = static_cast<std::size_t>(from * kRate);
    const auto n = static_cast<std::size_t>((to - from) * kRate);
    const double w = 2.0 * std::numbers::pi * hz / kRate;
    double s1 = 0.0, s2 = 0.0, windowSum = 0.0;
    for (std::size_t i = 0; i < n; ++i)
    {
        const double win = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * static_cast<double>(i) / static_cast<double>(n));
        windowSum += win;
        const double s0 = static_cast<double>(x[a + i]) * win + 2.0 * std::cos(w) * s1 - s2;
        s2 = s1;
        s1 = s0;
    }
    const double power = s1 * s1 + s2 * s2 - 2.0 * std::cos(w) * s1 * s2;
    return 20.0 * std::log10(2.0 * std::sqrt(std::max(0.0, power)) / windowSum + 1.0e-12);
}

double rmsDb(const float* x, double from, double to)
{
    double sum = 0.0;
    const auto a = static_cast<std::size_t>(from * kRate), b = static_cast<std::size_t>(to * kRate);
    for (std::size_t i = a; i < b; ++i)
        sum += static_cast<double>(x[i]) * static_cast<double>(x[i]);
    return 10.0 * std::log10(sum / static_cast<double>(b - a) + 1.0e-20);
}

juce::AudioBuffer<float> toBuffer(const std::vector<float>& x, int channels = 1)
{
    juce::AudioBuffer<float> b(channels, static_cast<int>(x.size()));
    for (int ch = 0; ch < channels; ++ch)
        std::copy(x.begin(), x.end(), b.getWritePointer(ch));
    return b;
}

spectral::Operation op(spectral::Kind kind, double start, double end, double low = 0.0, double high = 1.0e6)
{
    spectral::Operation o;
    o.kind = kind;
    o.start = start;
    o.end = end;
    o.low = low;
    o.high = high;
    return o;
}
} // namespace

TEST_CASE("Spectral editing: a region's gain and erasure, in its band and time only; the rest untouched", "[spectral][dsp]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 2.0), 0.0f);
    addSine(x, 1000.0, 0.3);
    addSine(x, 3000.0, 0.3);
    const auto input = toBuffer(x, 2);
    auto gain = op(spectral::Kind::gain, 0.6, 1.2, 800.0, 1200.0);
    gain.gainDb = -20.0;
    const auto out = spectral::apply(input, kRate, {gain});
    REQUIRE(out.getNumSamples() == input.getNumSamples());
    REQUIRE(out.getNumChannels() == 2);
    const float* y = out.getReadPointer(1);
    CHECK_THAT(levelDb(y, 1000.0, 0.7, 1.1) - levelDb(x.data(), 1000.0, 0.7, 1.1), WithinAbs(-20.0, 1.0));
    CHECK_THAT(levelDb(y, 3000.0, 0.7, 1.1) - levelDb(x.data(), 3000.0, 0.7, 1.1), WithinAbs(0.0, 0.3));
    CHECK_THAT(levelDb(y, 1000.0, 1.4, 1.9) - levelDb(x.data(), 1000.0, 1.4, 1.9), WithinAbs(0.0, 0.1));
    for (int i = 0; i < input.getNumSamples(); ++i)
        if (i < static_cast<int>(0.54 * kRate) || i > static_cast<int>(1.26 * kRate))
            REQUIRE(out.getSample(0, i) == input.getSample(0, i));

    const auto erased = spectral::apply(input, kRate, {op(spectral::Kind::erase, 0.5, 1.5, 2500.0, 3500.0)});
    CHECK(levelDb(erased.getReadPointer(0), 3000.0, 0.7, 1.3) < levelDb(x.data(), 3000.0, 0.7, 1.3) - 40.0);
    CHECK_THAT(levelDb(erased.getReadPointer(0), 1000.0, 0.7, 1.3) - levelDb(x.data(), 1000.0, 0.7, 1.3), WithinAbs(0.0, 0.3));

    // Disabled operations do nothing; the key follows what counts.
    auto disabled = gain;
    disabled.enabled = false;
    CHECK(spectral::keyOf({disabled}) == spectral::keyOf({}));
    CHECK(spectral::keyOf({gain}) != spectral::keyOf({}));
    auto louder = gain;
    louder.gainDb = -6.0;
    CHECK(spectral::keyOf({gain}) != spectral::keyOf({louder}));
}

TEST_CASE("Spectral repair rebuilds a click out of a tone from what surrounds it", "[spectral][dsp]")
{
    std::vector<float> clean(static_cast<std::size_t>(kRate * 2.0), 0.0f);
    addSine(clean, 440.0, 0.3);
    auto damaged = clean;
    std::mt19937 random(7);
    std::uniform_real_distribution<float> noise(-0.8f, 0.8f);
    for (std::size_t i = static_cast<std::size_t>(1.0 * kRate); i < static_cast<std::size_t>(1.004 * kRate); ++i)
        damaged[i] += noise(random);
    const auto repaired = spectral::apply(toBuffer(damaged), kRate, {op(spectral::Kind::repair, 0.998, 1.006)});
    double before = 0.0, after = 0.0;
    for (std::size_t i = static_cast<std::size_t>(0.9 * kRate); i < static_cast<std::size_t>(1.1 * kRate); ++i)
    {
        before = std::max(before, std::abs(static_cast<double>(damaged[i] - clean[i])));
        after = std::max(after, std::abs(static_cast<double>(repaired.getSample(0, static_cast<int>(i)) - clean[i])));
    }
    CHECK(before > 0.5);
    CHECK(after < 0.06);
    CHECK_THAT(levelDb(repaired.getReadPointer(0), 440.0, 0.95, 1.05), WithinAbs(levelDb(clean.data(), 440.0, 0.95, 1.05), 0.5));
}

TEST_CASE("De-noise from a learnt profile; de-hum at the mains frequency it finds", "[spectral][dsp]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 3.0), 0.0f);
    std::mt19937 random(3);
    std::normal_distribution<float> noise(0.0f, 0.02f);
    for (auto& v : x)
        v = noise(random);
    addSine(x, 1000.0, 0.3, 1.0);
    const auto input = toBuffer(x);
    auto denoise = op(spectral::Kind::denoise, 0.0, 3.0);
    denoise.profile = spectral::learnProfile(input, kRate, 0.1, 0.9);
    REQUIRE(static_cast<int>(denoise.profile.size()) == spectral::fftSizeFor(kRate) / 2 + 1);
    denoise.reductionDb = 24.0;
    const auto clean = spectral::apply(input, kRate, {denoise});
    CHECK(rmsDb(clean.getReadPointer(0), 0.2, 0.8) < rmsDb(x.data(), 0.2, 0.8) - 12.0);
    CHECK_THAT(levelDb(clean.getReadPointer(0), 1000.0, 1.5, 2.5) - levelDb(x.data(), 1000.0, 1.5, 2.5), WithinAbs(0.0, 1.0));

    for (const double mains : {60.0, 50.0})
    {
        std::vector<float> h(static_cast<std::size_t>(kRate * 2.0), 0.0f);
        addSine(h, 440.0, 0.3);
        addSine(h, mains, 0.1);
        addSine(h, 2.0 * mains, 0.05);
        addSine(h, 3.0 * mains, 0.03);
        const auto humming = toBuffer(h);
        CHECK_THAT(spectral::detectHum(humming, kRate), WithinAbs(mains, 0.1));
        const auto dehummed = spectral::apply(humming, kRate, {op(spectral::Kind::dehum, 0.0, 2.0)});
        for (int k = 1; k <= 3; ++k)
            CHECK(levelDb(dehummed.getReadPointer(0), k * mains, 0.5, 1.5) < levelDb(h.data(), k * mains, 0.5, 1.5) - 20.0);
        CHECK_THAT(levelDb(dehummed.getReadPointer(0), 440.0, 0.5, 1.5) - levelDb(h.data(), 440.0, 0.5, 1.5), WithinAbs(0.0, 0.5));
    }
}

TEST_CASE("The spectrogram shows a tone in its row, at its level", "[spectral][dsp]")
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 1.0), 0.0f);
    addSine(x, 1000.0, 0.3, 0.25, 0.75);
    const auto s = spectral::analyse(toBuffer(x), kRate, 384);
    REQUIRE(s.frames > 50);
    REQUIRE(s.rows == 384);
    CHECK_THAT(s.duration, WithinAbs(1.0, 1e-9));
    const int middle = static_cast<int>(0.5 / s.hop);
    int brightest = 0;
    for (int r = 0; r < s.rows; ++r)
        if (s.at(middle, r) > s.at(middle, brightest))
            brightest = r;
    CHECK_THAT(s.rowHz(brightest), WithinAbs(1000.0, 30.0));
    CHECK_THAT(s.hzRow(s.rowHz(100.0)), WithinAbs(100.0, 1e-9));
    const double db = s.at(middle, brightest) / 255.0 * 110.0 - 110.0;
    CHECK_THAT(db, WithinAbs(20.0 * std::log10(0.3), 1.5));
    CHECK(s.at(static_cast<int>(0.1 / s.hop), brightest) < 40); // (silence before)
}

namespace
{
struct SpectralApp
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

std::vector<float> readMono(const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr)
        return {};
    juce::AudioBuffer<float> b(static_cast<int>(reader->numChannels), static_cast<int>(reader->lengthInSamples));
    reader->read(&b, 0, b.getNumSamples(), 0, true, true);
    return {b.getReadPointer(0), b.getReadPointer(0) + b.getNumSamples()};
}

/** Two tones (1 and 3 kHz) for 2 s, with a quiet noise floor. */
juce::File writeTones(const juce::File& file)
{
    std::vector<float> x(static_cast<std::size_t>(kRate * 2.0), 0.0f);
    std::mt19937 random(11);
    std::normal_distribution<float> noise(0.0f, 0.003f);
    for (auto& v : x)
        v = noise(random);
    addSine(x, 1000.0, 0.3);
    addSine(x, 3000.0, 0.3);
    return test::writeWav(file, x, kRate);
}
} // namespace

TEST_CASE("Spectral edits in the session: added, rendered, shown, learnt from, bypassed, removed; pitch analysis hears them", "[spectral][session]")
{
    test::TempFolder temp;
    SpectralApp app;
    auto& session = app.project.getSession();
    auto& edits = app.project.getAudioEdits();
    auto& undo = session.getUndoManager();
    edits.setRenderFolder(temp.dir.getChildFile("Edits"));
    const auto source = writeTones(temp.dir.getChildFile("tones.wav"));
    auto track = session.addTrack(session::TrackKind::audio, "Tones");
    auto clip = session.addClip(track, source, 0.0, 2.0);

    // The spectrogram of the clip as it is, made in the background.
    CHECK(edits.getSpectrogram(clip) == nullptr);
    REQUIRE(edits.waitUntilIdle(30'000));
    auto before = edits.getSpectrogram(clip);
    REQUIRE(before != nullptr);

    // Attenuate the 1 kHz tone from 0.6 to 1.2 s: one undo step; the clip plays the render.
    spectral::Operation gain;
    gain.kind = spectral::Kind::gain;
    gain.start = 0.6;
    gain.end = 1.2;
    gain.low = 800.0;
    gain.high = 1200.0;
    gain.gainDb = -20.0;
    auto op = edits.addSpectralOperation(clip, gain, "Attenuate");
    REQUIRE(op.isValid());
    CHECK(undo.getUndoDescription() == "Attenuate");
    CHECK(session::audioedits::getSource(clip) == source);
    CHECK_THAT(static_cast<double>(op.getProperty(session::ids::start)), WithinAbs(0.6, 1e-9)); // (seconds of the source)
    CHECK(session::audioedits::describe(op).startsWith("Attenuate -20.0 dB"));
    REQUIRE(edits.waitUntilIdle(30'000));
    const auto rendered = session::clip::getFile(clip);
    CHECK(rendered != source);
    const auto y = readMono(rendered);
    const auto x = readMono(source);
    REQUIRE(y.size() == x.size());
    CHECK_THAT(levelDb(y.data(), 1000.0, 0.7, 1.1) - levelDb(x.data(), 1000.0, 0.7, 1.1), WithinAbs(-20.0, 1.0));
    CHECK_THAT(levelDb(y.data(), 3000.0, 0.7, 1.1) - levelDb(x.data(), 3000.0, 0.7, 1.1), WithinAbs(0.0, 0.3));

    // The spectrogram shows it.
    auto after = edits.getSpectrogram(clip);
    REQUIRE(edits.waitUntilIdle(30'000));
    after = edits.getSpectrogram(clip);
    REQUIRE(after != nullptr);
    CHECK(after != before);
    const int row = static_cast<int>(std::lround(after->hzRow(1000.0)));
    const auto levelAt = [&](double t) { return static_cast<int>(after->at(static_cast<int>(t / after->hop), row)); };
    CHECK(levelAt(0.3) - levelAt(0.9) > 30); // (about 20 dB: 46 levels)

    // A noise profile from the clip.
    juce::String error;
    const auto profile = edits.learnNoise(clip, 0.1, 0.5, error);
    CHECK(static_cast<int>(profile.size()) == spectral::fftSizeFor(kRate) / 2 + 1);
    CHECK(edits.learnNoise(clip, 0.1, 0.105, error).empty());
    CHECK(error.isNotEmpty());

    // Bypassed (its tick): the clip plays its source; enabled again, the render.
    session.beginTransaction("Bypass Spectral Edit");
    session::audioedits::setOperationEnabled(op, false, &undo);
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) == source);
    undo.undo();
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::clip::getFile(clip) != source);

    // Removed (the last edit: the whole edit goes); undone, back.
    edits.removeSpectralOperation(op);
    CHECK(undo.getUndoDescription() == "Remove Spectral Edit");
    CHECK_FALSE(session::audioedits::getEdit(clip).isValid());
    CHECK(session::clip::getFile(clip) == source);
    REQUIRE(edits.waitUntilIdle(30'000));
    undo.undo();
    REQUIRE(edits.waitUntilIdle(30'000));
    CHECK(session::audioedits::getSpectralOperations(clip).size() == 1);
    CHECK(session::clip::getFile(clip) != source);

    // Pitch analysis hears the spectral edits: a take whose second note is erased has one note.
    std::vector<float> take(static_cast<std::size_t>(kRate * 2.4), 0.0f);
    addSine(take, 440.0, 0.3, 0.2, 1.0);
    addSine(take, 659.26, 0.3, 1.3, 2.1);
    const auto takeFile = test::writeWav(temp.dir.getChildFile("take.wav"), take, kRate);
    auto vocal = session.addClip(track, takeFile, 3.0, 2.4);
    edits.addSpectralOperation(vocal, [] { spectral::Operation e; e.kind = spectral::Kind::erase; e.start = 1.25; e.end = 2.2; return e; }(), "Erase");
    REQUIRE(edits.analysePitch(vocal, pitch::Mode::melodic, error));
    REQUIRE(edits.waitUntilIdle(30'000));
    REQUIRE(session::audioedits::getPitchNotes(vocal).size() == 1);
    CHECK_THAT(session::audioedits::toEdit(session::audioedits::getPitchNotes(vocal)[0]).note.pitch, WithinAbs(69.0, 0.1));
    // ...and the pitch edits play on top of them.
    session.beginTransaction("Change Pitch");
    session::audioedits::setNotePitch(session::audioedits::getPitchNotes(vocal)[0], 71.0, &undo);
    REQUIRE(edits.waitUntilIdle(30'000));
    const auto both = readMono(session::clip::getFile(vocal));
    CHECK(levelDb(both.data(), 659.26, 1.4, 2.0) < -60.0);
    const auto frames = pitch::trackPitch(both.data() + static_cast<int>(0.4 * kRate), static_cast<int>(0.4 * kRate), kRate);
    CHECK_THAT(hzToMidi(frames[40].f0), WithinAbs(71.0, 0.1));
    // Removing the pitch edits keeps the spectral ones.
    edits.removePitch(vocal);
    CHECK(session::audioedits::getEdit(vocal).isValid());
    CHECK_FALSE(session::audioedits::hasPitch(vocal));
    CHECK(session::audioedits::getSpectralOperations(vocal).size() == 1);
}

TEST_CASE("The spectral editor: selections on the spectrogram become edits; the list bypasses and removes them", "[spectral][ui]")
{
    test::TempFolder temp;
    SpectralApp app;
    auto& session = app.project.getSession();
    auto& edits = app.project.getAudioEdits();
    auto& undo = session.getUndoManager();
    edits.setRenderFolder(temp.dir.getChildFile("Edits"));
    const auto source = writeTones(temp.dir.getChildFile("tones.wav"));
    auto track = session.addTrack(session::TrackKind::audio, "Tones");
    auto clip = session.addClip(track, source, 0.0, 2.0);

    ui::EditorState editor;
    ui::ClipEditor clipEditor(app.project, editor);
    clipEditor.setSize(1500, 420);
    editor.setEditedClip(session::clip::getId(clip));
    const auto click = [](juce::Button& b)
    {
        b.triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil(80);
    };
    juce::Button* spectralButton = nullptr;
    for (auto* child : clipEditor.getChildren())
        if (auto* b = dynamic_cast<juce::Button*>(child); b != nullptr && b->getButtonText() == "SPECTRAL")
            spectralButton = b;
    REQUIRE(spectralButton != nullptr);
    click(*spectralButton);
    auto& view = clipEditor.getSpectralEditor();
    REQUIRE(view.isVisible());
    CHECK(view.getClip() == clip);
    REQUIRE(edits.waitUntilIdle(30'000));
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    REQUIRE(view.getShownSpectrogram() != nullptr);
    const auto paint = [&clipEditor]
    {
        juce::Image image(juce::Image::RGB, clipEditor.getWidth(), clipEditor.getHeight(), true);
        juce::Graphics g(image);
        clipEditor.paintEntireComponent(g, false);
        return image;
    };
    paint();

    const auto event = [&view](juce::Point<float> p, juce::Point<float> down, juce::ModifierKeys extra = {})
    {
        const auto now = juce::Time::getCurrentTime();
        return juce::MouseEvent(juce::Desktop::getInstance().getMainMouseSource(), p,
                                juce::ModifierKeys(juce::ModifierKeys::leftButtonModifier | extra.getRawFlags()), juce::MouseInputSource::defaultPressure,
                                juce::MouseInputSource::defaultOrientation, juce::MouseInputSource::defaultRotation,
                                juce::MouseInputSource::defaultTiltX, juce::MouseInputSource::defaultTiltY, &view, &view, now, down, now, 1, p != down);
    };
    const auto drag = [&](juce::Point<float> from, juce::Point<float> to, juce::ModifierKeys mods = {})
    {
        view.mouseDown(event(from, from, mods));
        view.mouseDrag(event(to, from, mods));
        view.mouseUp(event(to, from, mods));
    };

    // A box around the 1 kHz tone from 0.6 to 1.2 s, attenuated.
    drag({view.timeToX(0.6), view.hzToY(1200.0)}, {view.timeToX(1.2), view.hzToY(800.0)});
    const auto selection = view.getSelection();
    CHECK_THAT(selection.start, WithinAbs(0.6, 0.02));
    CHECK_THAT(selection.end, WithinAbs(1.2, 0.02));
    CHECK_THAT(selection.low, WithinAbs(800.0, 25.0));
    CHECK_THAT(selection.high, WithinAbs(1200.0, 35.0));
    CHECK(view.getStatus().contains("0.60"));
    view.applyGain(-20.0);
    CHECK(undo.getUndoDescription() == "Attenuate");
    auto ops = session::audioedits::getSpectralOperations(clip);
    REQUIRE(ops.size() == 1);
    CHECK_THAT(static_cast<double>(ops[0].getProperty(session::ids::low)), WithinAbs(800.0, 25.0));
    REQUIRE(edits.waitUntilIdle(30'000));
    const auto y = readMono(session::clip::getFile(clip));
    CHECK(levelDb(y.data(), 1000.0, 0.75, 1.05) < levelDb(y.data(), 1000.0, 0.2, 0.5) - 15.0);

    // Shift: every frequency; Erase and Repair; the whole band is open-ended.
    drag({view.timeToX(1.5), view.hzToY(3000.0)}, {view.timeToX(1.6), view.hzToY(2000.0)}, juce::ModifierKeys::shiftModifier);
    CHECK(view.getSelection().low <= 20.01);
    view.erase();
    ops = session::audioedits::getSpectralOperations(clip);
    REQUIRE(ops.size() == 2);
    CHECK(ops[1].getProperty(session::ids::kind).toString() == "erase");
    CHECK_THAT(static_cast<double>(ops[1].getProperty(session::ids::low)), WithinAbs(0.0, 1e-9));
    CHECK(static_cast<double>(ops[1].getProperty(session::ids::high)) > 30000.0);
    view.repair();
    CHECK(undo.getUndoDescription() == "Repair");

    // Learn Noise and De-noise; De-hum without a selection covers the clip.
    drag({view.timeToX(0.1), view.hzToY(100.0)}, {view.timeToX(0.4), view.hzToY(10000.0)});
    CHECK(view.learnNoise());
    CHECK(view.hasNoiseProfile());
    view.denoise(12.0);
    CHECK(undo.getUndoDescription() == "De-noise");
    view.setSelection({});
    view.dehum(60.0);
    ops = session::audioedits::getSpectralOperations(clip);
    REQUIRE(ops.size() == 5);
    CHECK(ops[4].getProperty(session::ids::kind).toString() == "dehum");
    CHECK_THAT(static_cast<double>(ops[4].getProperty(session::ids::end)), WithinAbs(2.0, 1e-6));
    REQUIRE(edits.waitUntilIdle(60'000));
    juce::MessageManager::getInstance()->runDispatchLoopUntil(50);
    // DOJO_SPECTRAL_SNAPSHOT=<file.png>: the editor with its edits, for a look.
    if (const auto path = juce::SystemStats::getEnvironmentVariable("DOJO_SPECTRAL_SNAPSHOT", {}); path.isNotEmpty())
    {
        view.setSelection({0.6, 1.2, 800.0, 1200.0});
        const auto image = paint();
        const juce::File file(path);
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat().writeImageToStream(image, out);
    }
    paint();

    // The list: the tick bypasses (one step), the cross removes, Delete removes the chosen one.
    const auto tick = view.getOperationTick(0).getCentre();
    view.mouseDown(event(tick.toFloat(), tick.toFloat()));
    view.mouseUp(event(tick.toFloat(), tick.toFloat()));
    CHECK_FALSE(static_cast<bool>(ops[0].getProperty(session::ids::enabled, true)));
    CHECK(undo.getUndoDescription() == "Bypass Spectral Edit");
    const auto cross = view.getOperationCross(1).getCentre();
    view.mouseDown(event(cross.toFloat(), cross.toFloat()));
    CHECK(session::audioedits::getSpectralOperations(clip).size() == 4);
    const auto row = view.getOperationTick(0).getCentre().translated(80, 0);
    view.mouseDown(event(row.toFloat(), row.toFloat()));
    CHECK(view.operationRowAt(row) == 0);
    CHECK(view.keyPressed(juce::KeyPress(juce::KeyPress::deleteKey)));
    CHECK(session::audioedits::getSpectralOperations(clip).size() == 3);
    REQUIRE(edits.waitUntilIdle(60'000));
}

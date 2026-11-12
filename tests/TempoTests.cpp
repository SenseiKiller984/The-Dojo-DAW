#include "session/TempoDetector.h"
#include "TestAudio.h"
#include "ui/EditorState.h"
#include "ui/devices/TempoDetectPanel.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <random>

using namespace dojo;
using Catch::Matchers::WithinAbs;
namespace tempo = session::tempo;

namespace
{
constexpr double kRate = 44100.0;

enum class Voice
{
    kick,
    snare,
    hat,
};

/** One drum hit added at `time` (seconds): a pitched-down sine (kick), a noise burst with a tone
    (snare) or a short high noise burst (hat). */
void addHit(std::vector<float>& out, double time, Voice voice, float gain, std::mt19937& random)
{
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    const auto start = static_cast<std::size_t>(time * kRate);
    const double length = voice == Voice::kick ? 0.35 : voice == Voice::snare ? 0.2 : 0.06;
    double phase = 0.0;
    float previous = 0.0f;
    for (std::size_t i = 0; i < static_cast<std::size_t>(length * kRate) && start + i < out.size(); ++i)
    {
        const double t = static_cast<double>(i) / kRate;
        float v = 0.0f;
        switch (voice)
        {
        case Voice::kick:
            phase += 2.0 * test::kPi * (50.0 + 100.0 * std::exp(-t * 30.0)) / kRate;
            v = static_cast<float>(std::sin(phase) * std::exp(-t * 9.0));
            break;
        case Voice::snare:
            v = static_cast<float>((0.6 * noise(random) + 0.4 * std::sin(2.0 * test::kPi * 190.0 * t)) * std::exp(-t * 20.0));
            break;
        case Voice::hat:
        {
            const float n = noise(random);
            v = static_cast<float>((n - previous) * 0.5 * std::exp(-t * 60.0)); // (high-passed)
            previous = n;
            break;
        }
        }
        out[start + i] += gain * v;
    }
}

/** A drum part at `bpm`: kick on 1 and 3, snare on 2 and 4, eighth-note hats, with a little timing
    jitter (`jitterMs`) and background noise. Which voices play: `kick`, `snare`, `hats`. */
std::vector<float> drums(double bpm, double seconds, bool kick, bool snare, bool hats, double jitterMs = 0.0, unsigned seed = 1)
{
    std::mt19937 random(seed);
    std::normal_distribution<double> jitter(0.0, jitterMs / 1000.0);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    std::vector<float> out(static_cast<std::size_t>(seconds * kRate));
    for (auto& s : out)
        s = 0.003f * noise(random);
    const double beat = 60.0 / bpm;
    for (int eighth = 0; eighth * beat / 2.0 < seconds - 0.4; ++eighth)
    {
        const double t = 0.25 + eighth * beat / 2.0 + (jitterMs > 0.0 ? jitter(random) : 0.0);
        const int beatInBar = (eighth / 2) % 4;
        const bool onBeat = eighth % 2 == 0;
        if (kick && onBeat && (beatInBar == 0 || beatInBar == 2))
            addHit(out, t, Voice::kick, 0.8f, random);
        if (snare && onBeat && (beatInBar == 1 || beatInBar == 3))
            addHit(out, t, Voice::snare, 0.6f, random);
        if (hats)
            addHit(out, t, Voice::hat, onBeat ? 0.35f : 0.25f, random);
    }
    return out;
}
} // namespace

TEST_CASE("Tempo detection finds the tempo of a drum part to two decimal places", "[tempo]")
{
    for (const double bpm : {120.0, 121.12, 93.37, 140.5, 77.77})
    {
        const auto audio = drums(bpm, 20.0, true, true, true);
        const auto result = tempo::estimate(audio.data(), static_cast<int>(audio.size()), kRate);
        INFO("true tempo " << bpm << ", found " << result.bpm << " (" << result.problem << ")");
        REQUIRE(result.valid);
        CHECK_THAT(result.bpm, WithinAbs(bpm, 0.01));
        CHECK(result.confidence > 0.5);
    }
}

TEST_CASE("Tempo detection: the range decides between half and double time", "[tempo]")
{
    // A drum and bass part reads as half time in the default range, as itself in 100-200.
    const auto audio = drums(174.0, 20.0, true, true, true);
    const auto standard = tempo::estimate(audio.data(), static_cast<int>(audio.size()), kRate);
    REQUIRE(standard.valid);
    const bool octave = std::abs(standard.bpm - 174.0) < 0.01 || std::abs(standard.bpm - 87.0) < 0.01;
    CHECK(octave);
    const auto fast = tempo::estimate(audio.data(), static_cast<int>(audio.size()), kRate, 100.0, 200.0);
    REQUIRE(fast.valid);
    CHECK_THAT(fast.bpm, WithinAbs(174.0, 0.01));
}

TEST_CASE("Tempo detection works on single drums and played (not quantised) parts", "[tempo]")
{
    struct Case
    {
        const char* name;
        bool kick, snare, hats;
        double jitterMs;
        double tolerance;
    };
    for (const auto& c : {Case{"kick", true, false, false, 0.0, 0.01}, Case{"snare", false, true, false, 0.0, 0.01},
                          Case{"hats", false, false, true, 0.0, 0.01}, Case{"played", true, true, true, 4.0, 0.05}})
    {
        const auto audio = drums(121.12, 30.0, c.kick, c.snare, c.hats, c.jitterMs, 7);
        const auto result = tempo::estimate(audio.data(), static_cast<int>(audio.size()), kRate);
        INFO(c.name << ": found " << result.bpm << " (" << result.problem << ")");
        REQUIRE(result.valid);
        CHECK_THAT(result.bpm, WithinAbs(121.12, c.tolerance));
    }

    // Other sample rates.
    const auto audio = drums(98.6, 20.0, true, true, true);
    std::vector<float> at48(static_cast<std::size_t>(static_cast<double>(audio.size()) * 48000.0 / kRate));
    for (std::size_t i = 0; i < at48.size(); ++i)
        at48[i] = audio[std::min(audio.size() - 1, static_cast<std::size_t>(static_cast<double>(i) * kRate / 48000.0))];
    const auto result = tempo::estimate(at48.data(), static_cast<int>(at48.size()), 48000.0);
    CHECK_THAT(result.bpm, WithinAbs(98.6, 0.01));

    // Silence and noise have no tempo.
    std::vector<float> quiet(static_cast<std::size_t>(10.0 * kRate), 0.0f);
    CHECK_FALSE(tempo::estimate(quiet.data(), static_cast<int>(quiet.size()), kRate).valid);
    std::vector<float> shortClip(static_cast<std::size_t>(kRate), 0.1f);
    CHECK_FALSE(tempo::estimate(shortClip.data(), static_cast<int>(shortClip.size()), kRate).valid);
}

TEST_CASE("Tempo detection gives noise no confident tempo", "[tempo]")
{
    std::mt19937 random(3);
    std::uniform_real_distribution<float> noise(-0.5f, 0.5f);
    std::vector<float> audio(static_cast<std::size_t>(15.0 * kRate));
    for (auto& s : audio)
        s = noise(random);
    const auto result = tempo::estimate(audio.data(), static_cast<int>(audio.size()), kRate);
    CHECK((!result.valid || result.confidence < 0.3));
}

TEST_CASE("Several tracks: each estimated, octaves folded together, averaged", "[tempo]")
{
    std::vector<tempo::TrackResult> results(4);
    results[0].name = "Kick";
    results[0].estimate = {true, 121.12, 0.9, 40, {}};
    results[1].name = "Snare";
    results[1].estimate = {true, 121.48, 0.8, 30, {}};
    results[2].name = "Overheads";
    results[2].estimate = {true, 60.65, 0.6, 50, {}}; // half time
    results[3].name = "Room";
    results[3].estimate.problem = "No clear beat";
    const auto combined = tempo::combine(results);
    REQUIRE(combined.valid);
    CHECK(combined.counted == 3);
    CHECK_THAT(combined.tracks[2].folded, WithinAbs(121.30, 1e-9));
    CHECK_THAT(combined.average, WithinAbs((121.12 + 121.48 + 121.30) / 3.0, 1e-9));
    CHECK(tempo::roundBpm(combined.average) == 121.3);
    CHECK_FALSE(combined.tracks[3].estimate.valid);

    CHECK_FALSE(tempo::combine({}).valid);
}

TEST_CASE("Tempo detection of tracks: their clips as placed, stretched and looped", "[tempo]")
{
    test::TempFolder temp;
    const auto kickFile = test::writeWav(temp.dir.getChildFile("kick.wav"), drums(120.0, 12.0, true, false, false), kRate);
    const auto loopFile = test::writeWav(temp.dir.getChildFile("loop.wav"), drums(100.0, 2.4 * 2.0 + 0.5, true, true, true), kRate);
    const auto hatsFile = test::writeWav(temp.dir.getChildFile("hats.wav"), drums(120.0, 25.0, false, false, true), kRate);

    session::Session session;
    // Kick: two clips one after the other, the second from later in the file.
    auto kick = session.addTrack(session::TrackKind::audio, "Kick");
    session.addClip(kick, kickFile, 1.0, 10.0, 0.25);
    // Loop: two bars at 100 BPM, slowed down and looped.
    auto loop = session.addTrack(session::TrackKind::audio, "Loop");
    auto loopClip = session.addClip(loop, loopFile, 0.0, 4.8 * 1.25, 0.25); // two bars of the file, stretched:
    session::clip::setStretch(loopClip, 1.25, nullptr);                     // 100 / 1.25 = 80 BPM
    session.loopClip(loopClip, 4.0);                                        // four times
    // Hats: placed later in the session.
    auto hats = session.addTrack(session::TrackKind::audio, "Hats");
    session.addClip(hats, hatsFile, 30.0, 25.0);
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    session.addMidiClip(keys, 0.0, 4.0);
    auto empty = session.addTrack(session::TrackKind::audio, "Empty");

    std::vector<tempo::TrackMaterial> materials;
    for (const auto& t : {kick, loop, hats, keys, empty})
        materials.push_back(tempo::takeMaterial(session, t));
    CHECK(materials[3].problem.isNotEmpty()); // MIDI
    CHECK(materials[4].problem == "No audio clips");

    const auto combined = tempo::analyse(materials);
    REQUIRE(combined.tracks.size() == 5);
    INFO("kick " << combined.tracks[0].estimate.bpm << ", loop " << combined.tracks[1].estimate.bpm << ", hats "
                 << combined.tracks[2].estimate.bpm);
    CHECK_THAT(combined.tracks[0].estimate.bpm, WithinAbs(120.0, 0.01));
    CHECK_THAT(combined.tracks[1].estimate.bpm, WithinAbs(80.0, 0.02));
    CHECK_THAT(combined.tracks[2].estimate.bpm, WithinAbs(120.0, 0.01));
    CHECK_FALSE(combined.tracks[3].estimate.valid);
    CHECK(combined.counted == 3);
}

TEST_CASE("The Click's tempo detection: the selected tracks, their average, and the project tempo", "[tempo][ui]")
{
    juce::MessageManager::getInstance();
    test::TempFolder temp;
    const auto kick = test::writeWav(temp.dir.getChildFile("kick.wav"), drums(121.12, 20.0, true, false, false, 0.0, 1), kRate);
    const auto snare = test::writeWav(temp.dir.getChildFile("snare.wav"), drums(121.48, 20.0, false, true, false, 0.0, 2), kRate);
    const auto overheads = test::writeWav(temp.dir.getChildFile("oh.wav"), drums(121.30, 20.0, false, false, true, 0.0, 3), kRate);

    session::Session session;
    ui::EditorState editor;
    std::set<std::uint32_t> ids;
    for (const auto& [name, file] : {std::pair{"Kick", kick}, std::pair{"Snare", snare}, std::pair{"Overheads", overheads}})
    {
        auto t = session.addTrack(session::TrackKind::audio, name);
        session.addClip(t, file, 0.0, 20.0);
        ids.insert(session::track::getId(t));
    }
    auto keys = session.addTrack(session::TrackKind::instrument, "Keys");
    REQUIRE(session.getTempo() == 120.0);

    ui::TempoDetectPanel panel(session, editor);
    panel.setSize(290, 300);
    double askedDetected = 0.0;
    double askedProject = 0.0;
    panel.onDifferentTempo = [&](double detected, double project)
    {
        askedDetected = detected;
        askedProject = project;
    };

    // Nothing selected: nothing to do.
    editor.setSelectedTracks({}, 0);
    panel.detect();
    CHECK_FALSE(panel.isAnalysing());

    // The audio tracks are listed (not the instrument track); selecting tracks in the arranger
    // ticks them.
    CHECK(panel.getAudioTracks().size() == 3);
    ids.insert(session::track::getId(keys));
    editor.setSelectedTracks(ids, *ids.begin());
    for (const auto& t : panel.getAudioTracks())
        CHECK(panel.isTicked(session::track::getId(t)));
    // Selecting a non-audio track (the Click's own, say) leaves the ticks alone.
    editor.selectTrack(session::track::getId(keys));
    CHECK(panel.isTicked(*ids.begin()));

    panel.detect();
    CHECK(panel.isAnalysing());
    REQUIRE(panel.waitUntilDone(120'000));
    const auto& result = panel.getResult();
    REQUIRE(result.tracks.size() == 3);
    CHECK(result.tracks[0].name == "Kick");
    CHECK_THAT(result.tracks[0].estimate.bpm, WithinAbs(121.12, 0.01));
    CHECK_THAT(result.tracks[1].estimate.bpm, WithinAbs(121.48, 0.01));
    CHECK_THAT(result.tracks[2].estimate.bpm, WithinAbs(121.30, 0.01));
    CHECK(result.counted == 3);
    CHECK_THAT(panel.getDetectedTempo(), WithinAbs(121.30, 0.011));
    CHECK(panel.getDetectedTempo() == tempo::roundBpm(panel.getDetectedTempo())); // (two decimals)

    // It asked, because the project is at 120.
    CHECK(askedDetected == panel.getDetectedTempo());
    CHECK(askedProject == 120.0);

    // Half time, double time, then set it.
    panel.setOctave(0.5);
    CHECK_THAT(panel.getDetectedTempo(), WithinAbs(60.65, 0.011));
    panel.setOctave(1.0);
    panel.applyTempo();
    CHECK(session.getTempo() == panel.getDetectedTempo());
    session.getUndoManager().undo();
    CHECK(session.getTempo() == 120.0);

    // Already at that tempo: no question.
    session.setTempo(panel.getDetectedTempo());
    askedDetected = 0.0;
    panel.detect();
    REQUIRE(panel.waitUntilDone(120'000));
    CHECK(askedDetected == 0.0);

    // One track, ticked in the list.
    for (const auto& t : panel.getAudioTracks())
        panel.setTicked(session::track::getId(t), t == panel.getAudioTracks().front());
    panel.detect();
    REQUIRE(panel.waitUntilDone(120'000));
    REQUIRE(panel.getResult().tracks.size() == 1);
    CHECK_THAT(panel.getDetectedTempo(), WithinAbs(121.12, 0.011));
}

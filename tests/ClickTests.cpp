#include "devices/Click.h"
#include "session/Project.h"
#include "devices/DeviceRegistry.h"
#include "ui/EditActions.h"
#include "ui/EditorState.h"
#include "ui/components/LevelMeter.h"
#include "DeviceTestSupport.h"
#include "TestAudio.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace dojo;
using namespace dojo::devices;
using namespace dojo::test::devices;
using Catch::Matchers::WithinAbs;

namespace
{
/** A transport the test moves along block by block. */
struct Transport final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }
    PositionInfo info;
};

/** Runs `seconds` of the click from the timeline start, with the transport `playing` / `recording`. */
std::vector<double> runClick(Click& click, ProcessorNode& node, double seconds, bool playing, bool recording, double bpm = 120.0)
{
    Transport transport;
    node.prepare(kRate, kBlock, nullptr); // a fresh start (no click still ringing from a previous run)
    click.setPlayHead(&transport);
    std::vector<double> out;
    juce::AudioBuffer<double> buffer(2, kBlock);
    juce::MidiBuffer midi;
    const int total = static_cast<int>(seconds * kRate);
    for (int done = 0; done < total; done += kBlock)
    {
        const double ppq = done / kRate * bpm / 60.0;
        transport.info.setIsPlaying(playing);
        transport.info.setIsRecording(recording);
        transport.info.setBpm(bpm);
        transport.info.setPpqPosition(ppq);
        transport.info.setTimeInSeconds(done / kRate);
        transport.info.setPpqPositionOfLastBarStart(std::floor(ppq / 4.0) * 4.0);
        transport.info.setTimeSignature(juce::AudioPlayHead::TimeSignature{4, 4});
        buffer.clear();
        node.process(buffer, kBlock, midi, nullptr);
        for (int i = 0; i < kBlock; ++i)
            out.push_back(buffer.getSample(0, i));
    }
    click.setPlayHead(nullptr);
    return out;
}

/** Sample indices where a click starts (silence before, sound after). */
std::vector<std::size_t> onsets(const std::vector<double>& x)
{
    std::vector<std::size_t> result;
    std::size_t quietFor = 1000;
    for (std::size_t i = 0; i < x.size(); ++i)
    {
        if (std::abs(x[i]) > 1.0e-3 && quietFor >= 200)
            result.push_back(i);
        quietFor = std::abs(x[i]) > 1.0e-3 ? 0 : quietFor + 1;
    }
    return result;
}

double peakAround(const std::vector<double>& x, std::size_t at)
{
    double peak = 0.0;
    for (std::size_t i = at; i < std::min(x.size(), at + 2400); ++i)
        peak = std::max(peak, std::abs(x[i]));
    return peak;
}
} // namespace

TEST_CASE("The Click device ticks on the beat, accents the downbeat and follows the transport", "[devices][click]")
{
    auto [node, click] = makeNode<Click>();

    // 120 bpm: a beat every 0.5 s, sample-accurate, the first of every 4 accented.
    auto out = runClick(*click, *node, 2.6, true, false);
    auto starts = onsets(out);
    REQUIRE(starts.size() == 6);
    for (std::size_t k = 0; k < starts.size(); ++k)
        CHECK(starts[k] - k * 24000 <= 1); // (a click's first sample is sin(0) = 0)
    CHECK(peakAround(out, starts[0]) > peakAround(out, starts[1]) * 1.5); // beat 1 of bar 1
    CHECK(peakAround(out, starts[4]) > peakAround(out, starts[3]) * 1.5); // beat 1 of bar 2
    CHECK_THAT(peakAround(out, starts[1]), WithinAbs(peakAround(out, starts[2]), 1e-3));

    // Without the accent every beat is the same; louder or softer with the volume.
    setParameter(*click, "accent", 0.0f);
    out = runClick(*click, *node, 1.2, true, false);
    starts = onsets(out);
    REQUIRE(starts.size() == 3);
    CHECK_THAT(peakAround(out, starts[0]), WithinAbs(peakAround(out, starts[1]), 1e-3));
    const double quietPeak = peakAround(out, starts[1]);
    setParameter(*click, "volume", -12.0f);
    out = runClick(*click, *node, 0.7, true, false);
    CHECK_THAT(peakAround(out, onsets(out)[1]), WithinAbs(quietPeak * std::pow(10.0, -12.0 / 20.0), 1e-3));
    setParameter(*click, "volume", 0.0f);

    // Eighths: twice as many; at 90 bpm the beats are 2/3 s apart.
    setParameter(*click, "subdivision", 1.0f);
    CHECK(onsets(runClick(*click, *node, 2.1, true, false)).size() == 9);
    setParameter(*click, "subdivision", 0.0f);
    starts = onsets(runClick(*click, *node, 2.1, true, false, 90.0));
    REQUIRE(starts.size() == 4);
    CHECK(starts[1] - 32000 <= 1);

    // Stopped: silence. Record Only: silence while just playing, clicks while recording.
    for (const double v : runClick(*click, *node, 1.0, false, false))
        REQUIRE(v == 0.0);
    setParameter(*click, "when", 1.0f);
    CHECK(onsets(runClick(*click, *node, 0.9, true, false)).empty());
    CHECK(onsets(runClick(*click, *node, 0.9, true, true)).size() == 2);

    // Every sound renders and stays in range.
    setParameter(*click, "when", 0.0f);
    for (int sound = 0; sound < 4; ++sound)
    {
        setParameter(*click, "sound", static_cast<float>(sound));
        out = runClick(*click, *node, 0.9, true, false); // beats at 0 and 0.5 s
        CHECK(onsets(out).size() == 2);
        CHECK(test::peak(out) <= 1.0);
    }
}

TEST_CASE("The Click's Own Tempo runs it at its own tempo, independently of the project", "[devices][click]")
{
    auto [node, click] = makeNode<Click>();
    auto* own = click->getParameterById("ownTempo");
    auto* tempo = click->getParameterById("tempo");
    REQUIRE(own != nullptr);
    REQUIRE(tempo != nullptr);
    // (appended after the earlier parameters: automation lanes refer to parameters by index)
    CHECK(click->getParameters().getLast() == tempo);

    const auto spacedAt = [](const std::vector<std::size_t>& starts, double bpm)
    {
        for (std::size_t k = 0; k < starts.size(); ++k)
            if (std::abs(static_cast<double>(starts[k]) - static_cast<double>(k) * 60.0 / bpm * kRate) > 1.5)
                return false;
        return true;
    };

    // Off (the default): the click follows the project, whatever its own Tempo says.
    CHECK(own->getValue() == 0.0f);
    setParameter(*click, "tempo", 60.0f);
    CHECK(click->getEffectiveTempo(120.0) == 120.0);
    auto starts = onsets(runClick(*click, *node, 2.6, true, false, 120.0));
    CHECK(starts.size() == 6);
    CHECK(spacedAt(starts, 120.0));
    starts = onsets(runClick(*click, *node, 2.1, true, false, 90.0));
    CHECK(starts.size() == 4);
    CHECK(spacedAt(starts, 90.0));

    // On, at double the project's tempo: twice the beats, accents every 4 of its own beats.
    click->setOwnTempo(240.0, true);
    CHECK(own->getValue() == 1.0f);
    CHECK(click->getEffectiveTempo(120.0) == 240.0);
    auto out = runClick(*click, *node, 2.6, true, false, 120.0);
    starts = onsets(out);
    REQUIRE(starts.size() == 11);
    CHECK(spacedAt(starts, 240.0));
    CHECK(peakAround(out, starts[0]) > peakAround(out, starts[1]) * 1.5);
    CHECK(peakAround(out, starts[4]) > peakAround(out, starts[3]) * 1.5);
    CHECK(peakAround(out, starts[8]) > peakAround(out, starts[7]) * 1.5);

    // At half: the same whatever the project tempo is.
    click->setOwnTempo(60.0, true);
    for (const double projectBpm : {120.0, 90.0, 174.0})
    {
        starts = onsets(runClick(*click, *node, 2.1, true, false, projectBpm));
        CHECK(starts.size() == 3);
        CHECK(spacedAt(starts, 60.0));
        CHECK(click->getEffectiveTempo(projectBpm) == 60.0);
    }

    // Any tempo, to two decimals.
    click->setOwnTempo(133.33, true);
    CHECK(tempo->getCurrentValueAsText() == "133.33");
    CHECK_THAT(click->getEffectiveTempo(120.0), WithinAbs(133.33, 1e-4));
    starts = onsets(runClick(*click, *node, 3.0, true, false, 120.0));
    CHECK(starts.size() == 7);
    CHECK(spacedAt(starts, click->getEffectiveTempo(120.0)));

    // Off again: back with the project, the click's own tempo kept for next time.
    setParameter(*click, "ownTempo", 0.0f);
    CHECK(click->getEffectiveTempo(120.0) == 120.0);
    CHECK(spacedAt(onsets(runClick(*click, *node, 2.6, true, false, 120.0)), 120.0));
    CHECK(tempo->getCurrentValueAsText() == "133.33");
    // Setting the tempo without turning it on leaves it off.
    click->setOwnTempo(100.0, false);
    CHECK(own->getValue() == 0.0f);
    CHECK(click->getEffectiveTempo(120.0) == 120.0);

    // Kept in range.
    click->setOwnTempo(1000.0, true);
    CHECK(click->getEffectiveTempo(120.0) == Click::kMaxTempo);
    click->setOwnTempo(1.0, true);
    CHECK(click->getEffectiveTempo(120.0) == Click::kMinTempo);

    // Saved and restored with the device.
    click->setOwnTempo(87.5, true);
    juce::MemoryBlock state;
    click->getStateInformation(state);
    auto [node2, restored] = makeNode<Click>();
    restored->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(restored->getEffectiveTempo(120.0) == 87.5);
}

TEST_CASE("Create Click Track adds a mono aux input with the Click device", "[ui][click][tracks]")
{
    juce::MessageManager::getInstance();
    session::Project project([](std::unique_ptr<engine::RenderGraph>) {},
                             [](const juce::ValueTree& d, double, int, juce::String&)
                             { return devices::createBuiltinDevice(session::device::getType(d)); });
    ui::EditorState editor;
    auto& session = project.getSession();
    auto first = session.addTrack(session::TrackKind::audio, "Vox");
    editor.selectTrack(session::track::getId(first));

    const auto click = ui::actions::createClickTrack(session, editor);
    REQUIRE(click.isValid());
    CHECK(session::track::getName(click) == "Click");
    CHECK(session::track::getKind(click) == session::TrackKind::bus);
    CHECK(session::track::getFormat(click) == session::TrackFormat::mono);
    const auto devices = session::Session::getDevices(click);
    REQUIRE(devices.size() == 1);
    CHECK(session::device::getType(devices[0]) == Click::kTypeId);
    CHECK(session.indexOf(click) == 1);                                // after the selected track
    CHECK(editor.getSelectedTrack() == session::track::getId(click)); // and selected
    auto node = project.getDevices().getNode(devices[0]);
    REQUIRE(node != nullptr);
    CHECK(dynamic_cast<Click*>(&node->getProcessor()) != nullptr);

    // Several at once (Track > New): unique names.
    ui::actions::NewTracks row;
    row.type = ui::actions::NewTrackType::clickTrack;
    row.count = 2;
    const auto more = ui::actions::addNewTracks(session, editor, {row});
    REQUIRE(more.size() == 2);
    CHECK(session::track::getName(more[0]) == "Click 2");
    CHECK(session::track::getName(more[1]) == "Click 3");

    session.getUndoManager().undo();
    session.getUndoManager().undo();
    CHECK(session.getNumTracks() == 1);
}

TEST_CASE("Clear All Clip Indicators clears every meter", "[ui][meters]")
{
    juce::MessageManager::getInstance();
    ui::LevelMeter a, b, c;
    a.update(1.2f, 0.1f, 0.02); // over 0 dBFS on the left
    b.update(0.1f, 1.5f, 0.02); // ... on the right
    c.update(0.9f, 0.9f, 0.02); // under
    CHECK(a.isClipLit());
    CHECK(b.isClipLit());
    CHECK_FALSE(c.isClipLit());

    // The indicators latch...
    a.update(0.0f, 0.0f, 2.0);
    CHECK(a.isClipLit());
    // ... until cleared, all at once.
    ui::LevelMeter::clearAllClipIndicators();
    CHECK_FALSE(a.isClipLit());
    CHECK_FALSE(b.isClipLit());
}

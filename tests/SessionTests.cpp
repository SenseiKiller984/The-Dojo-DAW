#include "session/AudioAssetCache.h"
#include "session/GraphCompiler.h"
#include "session/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_formats/juce_audio_formats.h>

using namespace dojo::session;
using Catch::Matchers::WithinAbs;

namespace
{
/** A scratch folder that is deleted when the test ends. */
struct TempFolder
{
    TempFolder()
        : dir(juce::File::getSpecialLocation(juce::File::tempDirectory)
                  .getNonexistentChildFile("dojo-tests", "", false))
    {
        dir.createDirectory();
    }
    ~TempFolder() { dir.deleteRecursively(); }

    juce::File dir;
};

juce::File writeWav(const juce::File& file, double seconds, double sampleRate, int numChannels, float value)
{
    juce::AudioBuffer<float> buffer(numChannels, static_cast<int>(seconds * sampleRate));
    for (int ch = 0; ch < numChannels; ++ch)
        juce::FloatVectorOperations::fill(buffer.getWritePointer(ch), value, buffer.getNumSamples());

    std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(sampleRate)
                                                  .withNumChannels(numChannels)
                                                  .withBitsPerSample(24));
    REQUIRE(writer != nullptr);
    REQUIRE(writer->writeFromAudioSampleBuffer(buffer, 0, buffer.getNumSamples()));
    return file;
}
} // namespace

TEST_CASE("Tracks get unique ids, default names and palette colours", "[session]")
{
    Session session;
    auto a = session.addTrack();
    auto b = session.addTrack("Bass");

    CHECK(session.getNumTracks() == 2);
    CHECK(track::getId(a) != track::getId(b));
    CHECK(track::getName(a) == "Audio 1");
    CHECK(track::getName(b) == "Bass");
    CHECK(track::getColour(a) == getTrackPalette()[0]);
    CHECK(track::getColour(b) == getTrackPalette()[1]);
}

TEST_CASE("Edits are undoable one transaction at a time", "[session][undo]")
{
    Session session;
    session.beginTransaction("Add track");
    auto t = session.addTrack();

    session.beginTransaction("Volume");
    track::setVolumeDb(t, -6.0, &session.getUndoManager());
    track::setVolumeDb(t, -12.0, &session.getUndoManager()); // same gesture, coalesced

    CHECK(track::getVolumeDb(t) == -12.0);
    REQUIRE(session.getUndoManager().undo());
    CHECK(track::getVolumeDb(t) == 0.0);
    REQUIRE(session.getUndoManager().undo());
    CHECK(session.getNumTracks() == 0);
    REQUIRE(session.getUndoManager().redo());
    CHECK(session.getNumTracks() == 1);
}

TEST_CASE("Clips move between tracks and the move undoes", "[session][clip]")
{
    Session session;
    auto a = session.addTrack();
    auto b = session.addTrack();
    auto c = session.addClip(a, juce::File("/audio/kick.wav"), 1.0, 2.0);

    session.beginTransaction("Move");
    session.moveClip(c, b, 4.0);
    CHECK(Session::getTrackOf(c) == b);
    CHECK(clip::getStart(c) == 4.0);

    session.getUndoManager().undo();
    CHECK(Session::getTrackOf(c) == a);
    CHECK(clip::getStart(c) == 1.0);
}

TEST_CASE("Splitting keeps the audio continuous", "[session][clip]")
{
    Session session;
    auto t = session.addTrack();
    auto c = session.addClip(t, juce::File("/audio/loop.wav"), 2.0, 4.0, 0.5);

    auto right = session.splitClip(c, 3.0);
    REQUIRE(right.isValid());
    CHECK(clip::getLength(c) == 1.0);
    CHECK(clip::getStart(right) == 3.0);
    CHECK(clip::getLength(right) == 3.0);
    CHECK(clip::getOffset(right) == 1.5); // 0.5 + the 1 s that stays on the left
    CHECK(clip::getId(right) != clip::getId(c));

    CHECK_FALSE(session.splitClip(c, 10.0).isValid()); // outside the clip
}

TEST_CASE("Duplicating places the copy right after the original", "[session][clip]")
{
    Session session;
    auto t = session.addTrack();
    auto c = session.addClip(t, juce::File("/audio/a.wav"), 1.0, 2.5);
    auto copy = session.duplicateClip(c);

    CHECK(clip::getStart(copy) == 3.5);
    CHECK(clip::getLength(copy) == 2.5);
    CHECK(t.getNumChildren() == 2);
    CHECK(session.getEndTime() == 6.0);
}

TEST_CASE("Sessions round-trip through a file, with paths relative to it", "[session][file]")
{
    TempFolder temp;
    const auto audioFolder = temp.dir.getChildFile("Audio");
    audioFolder.createDirectory();
    const auto wav = writeWav(audioFolder.getChildFile("tone.wav"), 0.1, 48'000.0, 1, 0.5f);

    Session original;
    original.setTempo(98.5);
    auto t = original.addTrack("Vox");
    track::setPan(t, -0.25, nullptr);
    original.addClip(t, wav, 1.5, 0.1);
    REQUIRE(original.save(temp.dir.getChildFile("song.dojo")).wasOk());

    // Move the whole project folder: the relative path must still find the audio.
    const auto moved = temp.dir.getSiblingFile(temp.dir.getFileName() + "-moved");
    REQUIRE(temp.dir.moveFileTo(moved));
    temp.dir = moved;

    Session loaded;
    REQUIRE(loaded.load(moved.getChildFile("song.dojo")).wasOk());
    CHECK(loaded.getTempo() == 98.5);
    REQUIRE(loaded.getNumTracks() == 1);
    CHECK(track::getName(loaded.getTrack(0)) == "Vox");
    CHECK(track::getPan(loaded.getTrack(0)) == -0.25);

    const auto c = loaded.getTrack(0).getChild(0);
    CHECK(clip::getStart(c) == 1.5);
    CHECK(clip::getFile(c) == moved.getChildFile("Audio/tone.wav"));
    CHECK_FALSE(loaded.getUndoManager().canUndo());
}

TEST_CASE("Loading rejects files that are not sessions", "[session][file]")
{
    TempFolder temp;
    const auto bogus = temp.dir.getChildFile("bogus.dojo");
    REQUIRE(bogus.replaceWithText("<NOT_A_SESSION/>"));

    Session session;
    CHECK(session.load(bogus).failed());
}

TEST_CASE("The asset cache decodes audio and builds waveform peaks", "[session][assets]")
{
    TempFolder temp;
    const auto wav = writeWav(temp.dir.getChildFile("stereo.wav"), 0.5, 44'100.0, 2, -0.25f);

    AudioAssetCache cache;
    juce::String error;
    const auto asset = cache.loadNow(wav, error);

    REQUIRE(asset != nullptr);
    CHECK(error.isEmpty());
    CHECK(asset->sampleRate == 44'100.0);
    CHECK(asset->audio.getNumChannels() == 2);
    CHECK_THAT(asset->getLengthSeconds(), WithinAbs(0.5, 1e-6));
    CHECK(asset->peaks.getNumBuckets() > 0);
    CHECK_THAT(asset->peaks.minimum[0][0], WithinAbs(-0.25, 1e-4));
    CHECK(cache.find(wav) == asset);

    CHECK(cache.loadNow(temp.dir.getChildFile("missing.wav"), error) == nullptr);
    CHECK(error.isNotEmpty());
}

TEST_CASE("The graph resolves mute, solo, pan and phase", "[session][graph]")
{
    TempFolder temp;
    const auto wav = writeWav(temp.dir.getChildFile("a.wav"), 0.2, 48'000.0, 1, 0.5f);

    Session session;
    AudioAssetCache cache;
    juce::String error;
    REQUIRE(cache.loadNow(wav, error) != nullptr);

    auto lead = session.addTrack("Lead");
    auto pad = session.addTrack("Pad");
    auto bass = session.addTrack("Bass");
    session.addClip(lead, wav, 2.0, 0.2);
    session.addClip(lead, wav, 0.0, 0.2); // out of order on purpose
    track::setPan(lead, 0.5, nullptr);
    track::setPhaseInverted(pad, true, nullptr);
    track::setVolumeDb(pad, -6.0, nullptr);
    track::setMuted(bass, true, nullptr);

    GraphBuilder builder;
    auto graph = builder.build(session, cache, nullptr);
    REQUIRE(graph->tracks.size() == 3);

    const auto& leadTrack = graph->tracks[0];
    CHECK_THAT(leadTrack.pan, WithinAbs(0.5, 1e-9));
    CHECK_THAT(leadTrack.volume, WithinAbs(1.0, 1e-9));
    REQUIRE(leadTrack.clips.size() == 2);
    CHECK(leadTrack.clips[0].startSeconds == 0.0); // sorted by start
    CHECK(leadTrack.clips[1].startSeconds == 2.0);

    CHECK_THAT(graph->tracks[1].volume, WithinAbs(0.501, 1e-3)); // -6 dB
    CHECK(graph->tracks[1].phaseInverted);
    CHECK_FALSE(graph->tracks[2].audible); // muted
    CHECK(graph->tracks[0].audible);

    REQUIRE(graph->levels.size() == 1); // no busses: everything renders in parallel
    CHECK(graph->levels[0].size() == 3);
    CHECK(graph->masterSources.size() == 3);

    track::setSoloed(pad, true, nullptr);
    graph = builder.build(session, cache, nullptr);
    CHECK_FALSE(graph->tracks[0].audible); // not soloed while another track is
    CHECK(graph->tracks[1].audible);
}

TEST_CASE("Track slots stay stable and are recycled", "[session][graph]")
{
    Session session;
    AudioAssetCache cache;
    GraphBuilder builder;

    auto a = session.addTrack();
    auto b = session.addTrack();
    (void)builder.build(session, cache, nullptr);
    const int slotA = builder.getSlot(track::getId(a));
    const int slotB = builder.getSlot(track::getId(b));
    CHECK(slotA != slotB);

    session.moveTrack(1, 0); // reordering must not change slots
    (void)builder.build(session, cache, nullptr);
    CHECK(builder.getSlot(track::getId(a)) == slotA);

    session.removeTrack(a);
    auto c = session.addTrack();
    (void)builder.build(session, cache, nullptr);
    CHECK(builder.getSlot(track::getId(a)) == -1);
    CHECK(builder.getSlot(track::getId(c)) == slotA); // freed slot reused by the new track
}

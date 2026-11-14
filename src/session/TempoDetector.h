#pragma once

#include "session/Session.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <cmath>
#include <vector>

/**
    Tempo detection (the Click device's "Detect Tempo"): the tempo of audio to two decimal places.

    1. Onset strength: spectral flux of log-compressed STFT magnitudes (5.8 ms hop at 22.05 kHz), of
       the whole spectrum and of the band below 300 Hz (kick and snare body: the beat, not the
       hi-hats' subdivisions).
    2. Coarse tempo: the autocorrelations of both, scored as a comb over the first four multiples of
       each candidate beat period and weighted by a log-normal tempo prior around 120 BPM (an octave
       wide); folded into the range asked for.
    3. Exact tempo: onsets are picked, their times refined to the sample on the energy envelope (the
       30 % point of each attack, which every hit of a sound shares), numbered on the sixteenth-note
       grid of the coarse tempo, and a least-squares line through (number, time), weighted by onset
       strength and refitted without outliers, gives the grid step. The window grows from a few
       seconds to the whole material, so the numbering never slips. Over 20 s of a steady drum part
       this is accurate to well under 0.01 BPM.
*/
namespace dojo::session::tempo
{

struct Estimate
{
    bool valid = false;
    double bpm = 0.0;
    double confidence = 0.0; ///< 0..1: how much of the material sits on the grid of that tempo
    int onsets = 0;          ///< onsets that fit the grid
    juce::String problem;    ///< why there is no estimate
};

/** The tempo of mono audio, folded into [minBpm, maxBpm] (an octave or more apart). */
[[nodiscard]] Estimate estimate(const float* samples, int numSamples, double sampleRate, double minBpm = 70.0,
                                double maxBpm = 180.0);

/** What analysing a track needs from the session, taken on the message thread. */
struct ClipRegion
{
    juce::File file;
    double start = 0.0;
    double length = 0.0;
    double offset = 0.0;
    double stretch = 1.0;
    double loopLength = 0.0; ///< looped clips repeat this much (0: not looped)
    int sourceChannel = -1;
};
struct TrackMaterial
{
    std::uint32_t trackId = 0;
    juce::String name;
    std::vector<ClipRegion> clips;
    juce::String problem; ///< set when the track has nothing to analyse (MIDI, no audio clips, ...)
};
/** The audio clips of `track` (or why it has none to analyse). */
[[nodiscard]] TrackMaterial takeMaterial(const Session& session, const juce::ValueTree& track);

/** The track's clips mixed to mono at `sampleRate`, from its first clip on, at most `maxSeconds`.
    Reads the files (any thread). */
[[nodiscard]] std::vector<float> mixMaterial(const TrackMaterial& material, juce::AudioFormatManager& formats, double sampleRate,
                                             double maxSeconds, juce::String& error);

/** One track's result. */
struct TrackResult
{
    std::uint32_t trackId = 0;
    juce::String name;
    Estimate estimate;
    double folded = 0.0; ///< the estimate moved by octaves to agree with the others (= bpm if it did)
};

/** Several tracks of the same music: each estimate folded by octaves towards their median, and
    the mean of the valid ones. */
struct Combined
{
    std::vector<TrackResult> tracks;
    bool valid = false;
    double average = 0.0;
    int counted = 0;
};
[[nodiscard]] Combined combine(std::vector<TrackResult> results);

/** Analyses tracks one after the other (any thread): mixMaterial, estimate, then combine. Stops
    early (with what it has) when `cancel` becomes true. */
[[nodiscard]] Combined analyse(const std::vector<TrackMaterial>& tracks, double minBpm = 70.0, double maxBpm = 180.0,
                               const std::atomic<bool>* cancel = nullptr);

/** Rounded to two decimal places, as shown and as set. */
[[nodiscard]] inline double roundBpm(double bpm) { return std::round(bpm * 100.0) / 100.0; }

} // namespace dojo::session::tempo

#pragma once

#include <atomic>
#include <functional>
#include <vector>

/**
    Pitch analysis for the native pitch editor: the notes in audio, with their centre pitches and how
    their pitch and level move.

    Melodic (one voice: vocals, a bass line, a solo): a YIN f0 track (5 ms hops, the difference
    function by FFT, cumulative-mean normalised, parabolic refinement), octave errors and stray frames
    cleaned against the local median; the track is cut into notes where its vibrato-averaged pitch
    leaves the note's centre by more than 0.7 semitones for 40 ms, and at new attacks (6 dB rises)
    on one pitch.

    Polyphonic (chords: piano, guitar, pads): per 10 ms frame of a 93 ms window, the spectrum is
    whitened and the best fundamental found by harmonic summation (weights (f + 52) / (h f + 320),
    16 harmonics, a tenth of a semitone grid from A1 to C7); its partials are cancelled (spectral
    smoothness) and the next one found, until the salience falls under 35 % of the first or six
    voices are found. Frame pitches are linked into notes (within 0.6 semitones, gaps up to 30 ms).

    Plain C++ and juce::dsp::FFT; runs on any thread.
*/
namespace dojo::audioedit::pitch
{

enum class Mode
{
    melodic,
    polyphonic,
};

struct Note
{
    double start = 0.0;       ///< seconds into the analysed audio
    double end = 0.0;
    double pitch = 60.0;      ///< MIDI note number (fractional): the note's centre
    double hop = 0.005;       ///< seconds between the values below
    std::vector<float> drift; ///< semitones from `pitch`, every `hop` from `start`
    std::vector<float> level; ///< amplitude, 1 the loudest in the analysis, every `hop` from `start`

    /** The drift at `t` (seconds into the audio), interpolated; held beyond the ends. */
    [[nodiscard]] double driftAt(double t) const noexcept;
    [[nodiscard]] double levelAt(double t) const noexcept;
};

/** One 5 ms frame of a monophonic track. */
struct Frame
{
    float f0 = 0.0f;      ///< Hz; 0: unvoiced
    float clarity = 0.0f; ///< 1 - the YIN aperiodicity
    float rms = 0.0f;
};

/** The f0 of mono audio every `hopSeconds` (frame i centred at i * hop). */
[[nodiscard]] std::vector<Frame> trackPitch(const float* samples, int numSamples, double sampleRate, double hopSeconds = 0.005,
                                            double minHz = 50.0, double maxHz = 1200.0, const std::atomic<bool>* cancel = nullptr,
                                            const std::function<void(float)>& progress = {});

/** The notes of a monophonic track. */
[[nodiscard]] std::vector<Note> segmentMelodic(const std::vector<Frame>& frames, double hopSeconds);

/** The notes of polyphonic audio. */
[[nodiscard]] std::vector<Note> detectPolyphonic(const float* samples, int numSamples, double sampleRate, int maxVoices = 6,
                                                 const std::atomic<bool>* cancel = nullptr, const std::function<void(float)>& progress = {});

/** The notes of mono audio, in time order. `progress` gets 0..1. */
[[nodiscard]] std::vector<Note> analyse(const float* samples, int numSamples, double sampleRate, Mode mode,
                                        const std::atomic<bool>* cancel = nullptr, const std::function<void(float)>& progress = {});

} // namespace dojo::audioedit::pitch

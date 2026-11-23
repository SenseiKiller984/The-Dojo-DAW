#pragma once

#include "session/audioedit/PitchAnalysis.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <vector>

/**
    Pitch editing resynthesis: analysed notes moved in pitch, their drift scaled, their level and (in
    melodic material) their timing changed, rendered from the original audio.

    A phase vocoder that moves spectral peaks: each frame (a Hann window of about 46 ms in melodic
    material, 93 ms in polyphonic, four times overlapped) is analysed twice, a few milliseconds apart,
    for the exact frequency of every peak. Each peak, with the bins around it down to the valleys on
    either side, is moved to its new frequency and its phase carried on from the same peak in the
    previous output frame (phase-locked, as Laroche and Dolson's peak-shifting vocoder); unchanged
    peaks are steered back to their original phase, so unedited audio comes out as it went in.

      Melodic: every peak of a frame moves with the note sounding there (gliding between notes that
        touch), and the spectral envelope is kept where it was (cepstral, liftered under the pitch
        period), so a voice moved up does not sound smaller. Note boundaries moved in time warp the
        audio between them.
      Polyphonic: each peak belongs to the note it is a harmonic of (within 40 cents, the nearest);
        only those peaks move, by their note's amount, so one note of a chord can be changed alone.
        Timing is not edited (the notes of a chord share their audio's time).

    Only the spans that edits touch are processed (with 60 ms margins, and between unchanged
    neighbours for timing edits); everything else is the input, sample for sample, and each span is
    crossfaded in over 10 ms. Spans are cached by what they depend on, so moving one note re-renders
    only its span.
*/
namespace dojo::audioedit::pitch
{

struct Edit
{
    Note note;           ///< as analysed
    double newStart = 0.0;
    double newEnd = 0.0;
    double newPitch = 60.0;
    double drift = 1.0;  ///< how much of the pitch drift is kept: 0 flat, 1 as performed, up to 2
    double gainDb = 0.0;

    /** An edit of what was analysed (nothing changed yet). */
    static Edit of(const Note& note);
    [[nodiscard]] bool pitchEdited() const noexcept;
    [[nodiscard]] bool timeEdited() const noexcept;
    [[nodiscard]] bool edited() const noexcept;
    /** Semitones the pitch moves at `t` (seconds into the analysed audio) within the note. */
    [[nodiscard]] double offsetAt(double t) const noexcept;
};

struct RenderOptions
{
    Mode mode = Mode::melodic;
    bool preserveFormants = true; ///< melodic material only
};

struct Span
{
    double from = 0.0;
    double to = 0.0;
};

/** Where the edits change the audio (seconds; merged, in order), within [0, duration]. */
[[nodiscard]] std::vector<Span> editedSpans(const std::vector<Edit>& edits, const RenderOptions& options, double duration);

/** What a span's render depends on (for caching). */
[[nodiscard]] std::uint64_t spanKey(const std::vector<Edit>& edits, const RenderOptions& options, Span span);

/** The edits applied to `span` of `input`: that span's samples (all channels). */
[[nodiscard]] juce::AudioBuffer<float> renderSpan(const juce::AudioBuffer<float>& input, double sampleRate, const std::vector<Edit>& edits,
                                                  const RenderOptions& options, Span span, const std::atomic<bool>* cancel = nullptr);

/** Renders whole takes, keeping each span's render for the next time (one per edited material). */
class Renderer
{
public:
    /** The input with every edited span rendered in; false if cancelled. */
    bool render(const juce::AudioBuffer<float>& input, double sampleRate, const std::vector<Edit>& edits, const RenderOptions& options,
                juce::AudioBuffer<float>& output, const std::atomic<bool>* cancel = nullptr);
    [[nodiscard]] int getSpansRendered() const noexcept { return spansRendered_; } ///< by the last render (not from the cache)
    void clear() { cache_.clear(); }

private:
    std::map<std::uint64_t, juce::AudioBuffer<float>> cache_;
    int spansRendered_ = 0;
};

} // namespace dojo::audioedit::pitch

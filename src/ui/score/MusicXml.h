#pragma once

#include "ui/score/Notation.h"

#include <juce_core/juce_core.h>

namespace dojo::score
{

/** A score as MusicXML 4.0 (partwise, one part; a grand staff as two staves), for notation programs
    (Dorico, Sibelius, Finale, MuseScore...): key, time signatures, clefs, pitches with their spelling
    and shown accidentals, durations (16 divisions a quarter), dots, ties, beams (and sixteenth
    beams), stems, rests (whole-bar rests as measure rests). Bars keep the timeline's numbers. */
[[nodiscard]] juce::String toMusicXml(const Score& score, const juce::String& title, const juce::String& partName);
[[nodiscard]] bool writeMusicXml(const Score& score, const juce::File& file, const juce::String& title, const juce::String& partName);

} // namespace dojo::score

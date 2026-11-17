#pragma once

#include "ui/devices/PagedEditor.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace dojo::ui
{

/**
    Editors of the MIDI effects (Arpeggiator, Chord Generator, Scale, MIDI Filter): their controls in
    pages, beside a display of what they do. The Arpeggiator shows its pattern with the step
    playing; the Chord Generator the chord C3 gets and the last chord played, on a keyboard; Scale
    the key's notes and where the last note went; the MIDI Filter its note and velocity ranges and a
    lamp per kind of message (green: let through, red: held back). Null for any other processor.
*/
std::unique_ptr<juce::Component> createMidiEffectEditor(juce::AudioProcessor& processor, int& preferredWidth);

/** The pages of a MIDI effect's editor (tests). Empty for any other processor. */
[[nodiscard]] std::vector<PagedEditor::Page> midiEffectPages(juce::AudioProcessor& processor);

} // namespace dojo::ui

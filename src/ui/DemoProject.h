#pragma once

#include "session/Project.h"

namespace dojo::ui
{

/**
    Builds a small demo song into the (empty) project, using samples synthesised into `folder`:
    a Drum Rack beat (MIDI clip), Sampler chords with a volume fade-in (automation), a bass line
    through an EQ and a compressor ducked by the kick (sidechain), a reverb return fed by sends, a
    folder grouping the rhythm section, a limiter on the master and an 8-bar loop. Everything is
    one undo step. Returns an error if the samples could not be written.
*/
juce::Result createDemoProject(session::Project& project, const juce::File& folder);

} // namespace dojo::ui

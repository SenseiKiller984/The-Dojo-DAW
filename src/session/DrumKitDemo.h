#pragma once

#include "session/Presets.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace dojo::session::presets
{

/**
    A drum kit's demo beat as audio: its own beat (or DemoBeat::standard()) played by a Drum Rack
    loaded with the kit's settings, as it would play on a track: every pad's sample, level, pan,
    pitch and choke group. Its bars at its tempo, swing applied, then the last hits ringing out
    (faded over the last 0.3 s). Stereo at `sampleRate`, its peak kept at -1 dBFS or below.

    Loads the kit's samples (unpacking a built-in machine's first: devices/DrumKits.h), so call it
    where that may take a moment (the message thread, like loading a preset). Empty when `kit` is
    not a drum kit or none of its pads has a sample.
*/
[[nodiscard]] juce::AudioBuffer<float> renderDemo(const Preset& kit, double sampleRate);

/** The demo beat's length before the ring-out, in seconds (its bars at its tempo). */
[[nodiscard]] double demoSeconds(const DemoBeat& beat);

} // namespace dojo::session::presets

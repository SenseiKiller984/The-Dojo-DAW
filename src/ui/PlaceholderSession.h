#pragma once

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace dojo::ui::placeholder
{

/**
    Static demo content for the Phase 1 layout preview.

    The arranger and mixer render this fixed session so the visual language (track hierarchy, colour
    coding, clip styling, channel strips) can be reviewed before the real session model exists.
    It is replaced wholesale by the project/session model in Phase 3; nothing here talks to the engine.
*/
enum class TrackKind
{
    audio,
    instrument,
    groupBus,
    auxReturn,
};

struct Clip
{
    double startBar = 0.0; ///< 0-based
    double lengthBars = 1.0;
    juce::String name;
};

struct Track
{
    juce::String name;
    juce::String detail; ///< instrument / input / routing note shown under the name
    TrackKind kind = TrackKind::audio;
    int depth = 0; ///< folder nesting level
    juce::Colour colour;
    float faderDb = 0.0f;
    float pan = 0.0f; ///< -1 .. +1
    std::vector<Clip> clips;
};

/** The demo session (built once, on first use, on the message thread). */
[[nodiscard]] const std::vector<Track>& getTracks();

[[nodiscard]] juce::String getKindLabel(TrackKind kind);

} // namespace dojo::ui::placeholder

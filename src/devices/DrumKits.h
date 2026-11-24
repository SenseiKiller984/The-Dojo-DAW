#pragma once

#include <juce_core/juce_core.h>

/**
    The built-in drum machines' samples (the drum kits' library).

    Each machine's samples are compiled into The Dojo as one archive (resources/kits/<Machine>.zip)
    and unpacked, all at once, into the library folder the first time one of them is needed:

        <library>/<Machine>/<Sample>.wav       (Documents/The Dojo/Library/Drum Machines by default)

    A built-in kit (a Drum Rack preset: session/Presets.h) names its pads' samples by reference,
    "kit:<Machine>/<Sample>.wav", not by path, and so does a Drum Rack, Impulse or Sampler holding
    such a sample when its state is saved: a project or preset using the kits opens on any computer,
    the samples unpacked there when needed. Samples outside the library are saved by their path.

    Thread-safe (unpacking is serialised).
*/
namespace dojo::devices::kits
{

inline constexpr const char* kReferencePrefix = "kit:";

/** The drum machines whose samples are built in ("AdrenaLinn", "Linn 9000", ...), sorted. */
[[nodiscard]] juce::StringArray getMachines();
/** A machine's sample file names ("Kick 1.wav", ...), sorted; empty for an unknown machine. */
[[nodiscard]] juce::StringArray getSampleNames(const juce::String& machine);

/** Where the samples are unpacked (Documents/The Dojo/Library/Drum Machines unless set). */
[[nodiscard]] juce::File getLibraryFolder();
/** Points the library elsewhere (tests; an empty File restores the default). */
void setLibraryFolder(const juce::File& folder);

/** True for "kit:..." text. */
[[nodiscard]] bool isReference(const juce::String& text);
/** "kit:<Machine>/<Sample>" for a file inside the library folder; otherwise the file's full path. */
[[nodiscard]] juce::String referenceFor(const juce::File& file);
/**
    The file a saved sample refers to: for a kit reference the unpacked sample (unpacking its
    machine's samples first when it is missing), for anything else the path as a file.
    An unknown kit reference (or one that cannot be unpacked) gives an empty File.
*/
[[nodiscard]] juce::File fileFor(const juce::String& reference);

/** Unpacks a machine's samples that are missing or damaged; false (and why) on failure. */
bool unpack(const juce::String& machine, juce::String& error);

} // namespace dojo::devices::kits

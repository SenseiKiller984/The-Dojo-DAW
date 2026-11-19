#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace dojo::ui::browserDrag
{

/**
    Drag-and-drop of audio files from the browser to the arranger, the Devices panel and instrument
    editors (Drum Rack pads, Impulse slots, Sampler). The description carries the files' paths, so
    any target can read them without knowing about the browser's components.
*/
inline constexpr const char* kPrefix = "DojoBrowserFiles:";

[[nodiscard]] inline juce::var describe(const juce::StringArray& paths)
{
    return juce::String(kPrefix) + paths.joinIntoString("\n");
}

[[nodiscard]] inline bool isFileDrag(const juce::var& description)
{
    return description.toString().startsWith(kPrefix);
}

/** The dragged files that still exist. */
[[nodiscard]] inline juce::StringArray getFiles(const juce::var& description)
{
    juce::StringArray files;
    if (!isFileDrag(description))
        return files;
    for (const auto& path : juce::StringArray::fromLines(description.toString().fromFirstOccurrenceOf(kPrefix, false, false)))
        if (path.isNotEmpty() && juce::File::isAbsolutePath(path) && juce::File(path).existsAsFile())
            files.add(path);
    return files;
}

} // namespace dojo::ui::browserDrag

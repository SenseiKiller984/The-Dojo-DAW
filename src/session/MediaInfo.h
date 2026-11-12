#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <optional>

namespace dojo::session
{

/** What the Clip List shows and sorts by about a media file (read from its header only). */
struct MediaFileInfo
{
    bool exists = false;
    bool readable = false;    ///< an audio file the formats can open
    double lengthSeconds = 0.0;
    int numChannels = 0;
    double sampleRate = 0.0;
    std::optional<double> timeReference; ///< BWF time stamp of the first sample (seconds since midnight)
    juce::String scene;                  ///< iXML SCENE, or from the file name ("Sc12 Tk3", "S4_T2", ...)
    juce::String take;
    juce::Time created;
    juce::Time modified;
    juce::String diskName; ///< the volume the file is on
};

/** Reads an audio file's header (and iXML chunk) for the Clip List. Video files: dates and disk only. */
[[nodiscard]] MediaFileInfo readMediaFileInfo(juce::AudioFormatManager& formats, const juce::File& file);

/** The name of the volume `file` is on: its label and drive on Windows, the mount point elsewhere. */
[[nodiscard]] juce::String getDiskName(const juce::File& file);

/** SCENE and TAKE from an iXML document (empty strings if absent). */
void parseIxmlSceneTake(const juce::String& ixml, juce::String& scene, juce::String& take);
/** Scene and take from a file name such as "Sc12 Tk3", "S04_T02" or "Scene 7 Take 1". */
void parseSceneTakeFromName(const juce::String& name, juce::String& scene, juce::String& take);

} // namespace dojo::session

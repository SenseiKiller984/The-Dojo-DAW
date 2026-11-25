#pragma once

#include <juce_core/juce_core.h>

/**
    The free trial's guard against recording The Dojo's output instead of exporting it: programs
    that record or reroute what the sound card plays (screen recorders such as OBS, virtual mixers
    such as Voicemeeter Banana and Potato, audio recorders), and virtual audio devices (VB-Cable,
    Voicemeeter's inputs, BlackHole, "Stereo Mix", PulseAudio monitors...). While the trial sees
    one, its output is muted.

    This raises the bar, it is not a wall: a program can be renamed, and a cable from the
    headphone socket into another computer records anything.
*/
namespace dojo::licensing::capture
{

/** The recording programs running now (display names; empty if none, or where it cannot tell). */
[[nodiscard]] juce::StringArray findRecordingSoftware();

/** Which of these process names (as the system lists them: "obs64.exe", "obs") are recording
    programs; their display names. */
[[nodiscard]] juce::StringArray matchRecordingSoftware(const juce::StringArray& processNames);

/** True for a virtual or loopback audio device (by its name). */
[[nodiscard]] bool isVirtualAudioDevice(const juce::String& deviceName);

/** Windows 10 2004 and later: leaves a window out of screen captures and recordings (it shows black
    there); older Windows: black in captures. Elsewhere it does nothing. False if it could not. */
bool setWindowExcludedFromCapture(void* nativeWindowHandle, bool excluded);

} // namespace dojo::licensing::capture

// The Dojo — process entry point.
//
// START_JUCE_APPLICATION expands to the platform's main()/WinMain(), which initialises JUCE, turns
// the main thread into the message (UI) thread and calls DojoApplication::initialise() on it.
// The audio thread is created by the audio driver when AudioDeviceHost opens a device; it enters
// The Dojo through dojo::engine::AudioEngine::process().

#include "app/DojoApplication.h"

START_JUCE_APPLICATION(dojo::app::DojoApplication)

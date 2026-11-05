// dojo-plugin-scanner: opens one plug-in file in its own process and reports what it contains.
//
// The Dojo runs this for every plug-in file it scans, so a plug-in that crashes or hangs while being
// loaded only takes down this process (and gets blacklisted), never the DAW.
//
//   dojo-plugin-scanner --format <VST3|CLAP> --file <path> --output <result.xml>

#include "plugins/PluginManager.h"

#include <juce_events/juce_events.h>

int main(int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juce; // some plug-ins need a message thread while loading

    juce::StringArray arguments;
    for (int i = 1; i < argc; ++i)
        arguments.add(juce::String::fromUTF8(argv[i]));

    return dojo::plugins::PluginManager::runScannerProcess(arguments);
}

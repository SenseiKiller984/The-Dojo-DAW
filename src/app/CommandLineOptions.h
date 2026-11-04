#pragma once

#include <juce_core/juce_core.h>

namespace dojo::app
{

/**
    Command-line switches (all optional):

      --audio-api=<name>     Prefer an audio back-end, e.g. "JACK", "ALSA", "ASIO", "Windows Audio".
      --test-tone            Start with the diagnostic test tone enabled.
      --smoke-test[=secs]    Start, run for N seconds (default 3), verify the UI and engine, then exit
                             with status 0 on success. Used by CI on both platforms.
      --version              Print the version and exit.
*/
struct CommandLineOptions
{
    juce::String audioApi;
    bool testTone = false;
    bool printVersion = false;
    int smokeTestSeconds = 0; ///< 0 = normal interactive run

    static CommandLineOptions parse(const juce::StringArray& arguments);
};

} // namespace dojo::app

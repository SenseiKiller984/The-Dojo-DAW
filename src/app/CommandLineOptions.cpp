#include "app/CommandLineOptions.h"

namespace dojo::app
{

CommandLineOptions CommandLineOptions::parse(const juce::StringArray& arguments)
{
    CommandLineOptions options;

    for (const auto& raw : arguments)
    {
        const auto argument = raw.unquoted().trim();

        if (argument.startsWith("--audio-api="))
            options.audioApi = argument.fromFirstOccurrenceOf("=", false, false).unquoted();
        else if (argument == "--test-tone")
            options.testTone = true;
        else if (argument == "--version")
            options.printVersion = true;
        else if (argument == "--smoke-test")
            options.smokeTestSeconds = 3;
        else if (argument.startsWith("--smoke-test="))
            options.smokeTestSeconds =
                juce::jlimit(1, 600, argument.fromFirstOccurrenceOf("=", false, false).getIntValue());
    }

    return options;
}

} // namespace dojo::app

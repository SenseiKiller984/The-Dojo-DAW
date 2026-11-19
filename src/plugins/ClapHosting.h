#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

namespace dojo::plugins
{

/**
    Hosting of CLAP plug-ins (https://cleveraudio.org), as a juce::AudioPluginFormat.

    JUCE hosts VST3; this format adds CLAP on the official CLAP SDK so both formats are scanned,
    instantiated, automated, saved and shown through the same juce::AudioPluginFormatManager /
    juce::AudioPluginInstance interfaces, and the engine runs them like any other device.

    Supported host extensions: log, thread-check, params (values, flush, rescan of values), state,
    latency, audio-ports and note-ports (queried at creation), gui (embedded: X11 on Linux, Win32
    on Windows), timer-support and posix-fd-support (Linux GUIs). request_restart re-activates the
    plug-in with the audio thread held off (lock-free hand-over), so latency changes are picked up.

    A .clap file can contain several plug-ins: each becomes one PluginDescription with
    fileOrIdentifier = the file and uniqueId = a hash of the CLAP plug-in id.
*/
class ClapPluginFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* kFormatName = "CLAP";

    ClapPluginFormat() = default;

    juce::String getName() const override { return kFormatName; }
    void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType(const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier(const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning(const juce::PluginDescription& description) override;
    bool doesPluginStillExist(const juce::PluginDescription& description) override;
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins(const juce::FileSearchPath& directoriesToSearch, bool recursive,
                                            bool allowPluginsWhichRequireAsynchronousInstantiation) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation(const juce::PluginDescription&) const override { return false; }

    /** The stable hash used as PluginDescription::uniqueId for a CLAP plug-in id. */
    static int hashPluginId(const juce::String& clapId);

protected:
    void createPluginInstance(const juce::PluginDescription& description, double initialSampleRate, int initialBufferSize,
                              PluginCreationCallback callback) override;
};

} // namespace dojo::plugins

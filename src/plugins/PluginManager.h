#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>

namespace dojo::plugins
{

/**
    Knows which plug-ins are installed and creates them.

    Formats: VST3 (through JUCE) and CLAP (ClapPluginFormat), on Windows and Linux alike.

    Scanning is out-of-process: every plug-in file is opened by the separate `dojo-plugin-scanner`
    executable, so a plug-in that crashes or hangs while being scanned takes down only that child
    process. Such plug-ins are put on a blacklist (with the reason) and skipped by later scans until
    the user clears it. Results are kept in a KnownPluginList that is saved to disk.

    Message thread API, except that scanning itself runs on a background thread.
*/
class PluginManager final : private juce::Thread
{
public:
    struct ScanReport
    {
        int pluginsFound = 0;
        int filesScanned = 0;
        juce::StringArray failed;  ///< "file: reason" — no plug-ins in it, or it could not be loaded
        juce::StringArray crashed; ///< "file: reason" — crashed or hung; now blacklisted
    };

    /** `scannerExecutable`: the dojo-plugin-scanner binary. `listFile`: where the list is stored. */
    PluginManager(juce::File scannerExecutable, juce::File listFile);
    ~PluginManager() override;

    [[nodiscard]] juce::AudioPluginFormatManager& getFormatManager() noexcept { return formats_; }
    [[nodiscard]] juce::KnownPluginList& getKnownPlugins() noexcept { return known_; }

    /** Registers the formats The Dojo hosts (VST3, CLAP) with a format manager. */
    static void addFormats(juce::AudioPluginFormatManager& manager);

    // --- Scanning ----------------------------------------------------------------------------------
    /** Extra folders to search (per format name), on top of each format's default locations. */
    [[nodiscard]] juce::FileSearchPath getSearchPath(const juce::String& formatName) const;
    void setSearchPath(const juce::String& formatName, const juce::FileSearchPath& path);

    /** Scans in the background; `onScanFinished` is called on the message thread. Files already in the
        list are skipped unless `rescanAll`. */
    void startScan(bool rescanAll = false);
    [[nodiscard]] bool isScanning() const noexcept { return isThreadRunning(); }
    [[nodiscard]] float getScanProgress() const noexcept { return progress_.load(); }
    [[nodiscard]] juce::String getCurrentScanFile() const;
    std::function<void(const ScanReport&)> onScanFinished;

    /** Scans the given files of one format out of process, on the calling thread. */
    ScanReport scanFiles(const juce::String& formatName, const juce::StringArray& files, int timeoutMs = 60'000);

    /** Forgets the blacklist (the next scan retries those files). */
    void clearBlacklist();
    [[nodiscard]] juce::String getBlacklistReason(const juce::String& file) const;

    // --- Instances ---------------------------------------------------------------------------------
    /** Creates a plug-in synchronously on the message thread. Null with `error` set on failure. */
    std::unique_ptr<juce::AudioPluginInstance> createInstance(const juce::PluginDescription& description, double sampleRate,
                                                              int blockSize, juce::String& error);

    /** The DEVICE type string used for hosted plug-ins. */
    static constexpr const char* kDeviceType = "plugin";

    // --- Persistence ---------------------------------------------------------------------------------
    void save() const;
    void load();

    // --- The scanner process -------------------------------------------------------------------------
    /** Entry point of dojo-plugin-scanner: `--format <name> --file <path> --output <xml>`.
        Exit codes: 0 = found plug-ins, 2 = none in the file, 1 = bad arguments. */
    static int runScannerProcess(const juce::StringArray& arguments);

private:
    void run() override;

    juce::File scanner_;
    juce::File listFile_;
    juce::AudioPluginFormatManager formats_;
    juce::KnownPluginList known_;
    std::map<juce::String, juce::FileSearchPath> searchPaths_;
    std::map<juce::String, juce::String> blacklistReasons_;

    mutable std::mutex scanMutex_; ///< guards searchPaths_, blacklistReasons_ and currentFile_
    juce::String currentFile_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); ///< message thread only
    std::atomic<float> progress_{0.0f};
    bool rescanAll_ = false;
};

} // namespace dojo::plugins

#include "plugins/PluginManager.h"

#include "plugins/ClapHosting.h"

namespace dojo::plugins
{

PluginManager::PluginManager(juce::File scannerExecutable, juce::File listFile)
    : juce::Thread("Dojo plug-in scan")
    , scanner_(std::move(scannerExecutable))
    , listFile_(std::move(listFile))
{
    addFormats(formats_);
    load();
}

PluginManager::~PluginManager()
{
    *alive_ = false;
    stopThread(30'000);
}

void PluginManager::addFormats(juce::AudioPluginFormatManager& manager)
{
#if JUCE_PLUGINHOST_VST3
    manager.addFormat(std::make_unique<juce::VST3PluginFormat>());
#endif
    manager.addFormat(std::make_unique<ClapPluginFormat>());
}

juce::FileSearchPath PluginManager::getSearchPath(const juce::String& formatName) const
{
    const std::lock_guard lock(scanMutex_);
    const auto it = searchPaths_.find(formatName);
    return it != searchPaths_.end() ? it->second : juce::FileSearchPath();
}

void PluginManager::setSearchPath(const juce::String& formatName, const juce::FileSearchPath& path)
{
    {
        const std::lock_guard lock(scanMutex_);
        searchPaths_[formatName] = path;
    }
    save();
}

juce::String PluginManager::getCurrentScanFile() const
{
    const std::lock_guard lock(scanMutex_);
    return currentFile_;
}

void PluginManager::clearBlacklist()
{
    known_.clearBlacklistedFiles();
    {
        const std::lock_guard lock(scanMutex_);
        blacklistReasons_.clear();
    }
    save();
}

juce::String PluginManager::getBlacklistReason(const juce::String& file) const
{
    const std::lock_guard lock(scanMutex_);
    const auto it = blacklistReasons_.find(file);
    return it != blacklistReasons_.end() ? it->second : juce::String();
}

void PluginManager::startScan(bool rescanAll)
{
    if (isThreadRunning())
        return;
    rescanAll_ = rescanAll;
    progress_ = 0.0f;
    startThread(juce::Thread::Priority::low);
}

PluginManager::ScanReport PluginManager::scanFiles(const juce::String& formatName, const juce::StringArray& files, int timeoutMs)
{
    ScanReport report;
    const auto tempDir = juce::File::getSpecialLocation(juce::File::tempDirectory);

    for (int i = 0; i < files.size(); ++i)
    {
        if (threadShouldExit())
            break;

        const auto& file = files[i];
        {
            const std::lock_guard lock(scanMutex_);
            currentFile_ = file;
        }

        // Unique per scan: several scans (or Dojo instances) may run at once.
        const auto output = tempDir.getChildFile("dojo-scan-" + juce::Uuid().toString() + ".xml");
        juce::ChildProcess child;
        const juce::StringArray command{scanner_.getFullPathName(), "--format", formatName, "--file", file, "--output",
                                        output.getFullPathName()};
        ++report.filesScanned;

        if (!scanner_.existsAsFile() || !child.start(command, 0))
        {
            report.failed.add(file + ": the plug-in scanner could not be started (" + scanner_.getFullPathName() + ")");
            continue;
        }

        const bool finished = child.waitForProcessToFinish(timeoutMs);
        if (!finished)
            child.kill();
        const auto exitCode = finished ? static_cast<int>(child.getExitCode()) : -1;

        juce::OwnedArray<juce::PluginDescription> found;
        if (finished && exitCode == 0)
        {
            if (const auto xml = juce::XmlDocument::parse(output))
                for (const auto* element : xml->getChildIterator())
                {
                    auto description = std::make_unique<juce::PluginDescription>();
                    if (description->loadFromXml(*element))
                        found.add(description.release());
                }
        }
        output.deleteFile();

        if (!found.isEmpty())
        {
            const auto replace = [&](const juce::PluginDescription& d)
            {
                for (const auto& existing : known_.getTypes())
                    if (existing.fileOrIdentifier == d.fileOrIdentifier && existing.uniqueId == d.uniqueId &&
                        existing.pluginFormatName == d.pluginFormatName)
                        known_.removeType(existing);
                known_.addType(d);
            };
            for (const auto* d : found)
                replace(*d);
            report.pluginsFound += found.size();
        }
        else if (finished && exitCode == 2)
        {
            report.failed.add(file + ": no plug-ins found");
        }
        else
        {
            const auto reason = finished ? "crashed while being scanned (exit code " + juce::String(exitCode) + ")"
                                         : "did not finish scanning within " + juce::String(timeoutMs / 1000) + " s";
            known_.addToBlacklist(file);
            {
                const std::lock_guard lock(scanMutex_);
                blacklistReasons_[file] = reason;
            }
            report.crashed.add(file + ": " + reason);
        }

        progress_ = static_cast<float>(i + 1) / static_cast<float>(std::max(1, files.size()));
    }

    {
        const std::lock_guard lock(scanMutex_);
        currentFile_.clear();
    }
    return report;
}

void PluginManager::run()
{
    ScanReport total;

    // Collect every candidate file of every format first, so progress is meaningful.
    std::vector<std::pair<juce::String, juce::StringArray>> work;
    int totalFiles = 0;
    for (auto* format : formats_.getFormats())
    {
        auto path = format->getDefaultLocationsToSearch();
        const auto extra = getSearchPath(format->getName());
        for (int i = 0; i < extra.getNumPaths(); ++i)
            path.add(extra[i]);

        auto files = format->searchPathsForPlugins(path, true, false);
        juce::StringArray toScan;
        for (const auto& file : files)
        {
            if (known_.getBlacklistedFiles().contains(file))
                continue;
            bool known = false;
            for (const auto& type : known_.getTypes())
                known = known || (type.fileOrIdentifier == file && !format->pluginNeedsRescanning(type));
            if (rescanAll_ || !known)
                toScan.add(file);
        }
        totalFiles += toScan.size();
        work.emplace_back(format->getName(), toScan);
    }

    int done = 0;
    for (const auto& [formatName, files] : work)
    {
        for (const auto& file : files)
        {
            if (threadShouldExit())
                break;
            const auto report = scanFiles(formatName, juce::StringArray{file});
            total.pluginsFound += report.pluginsFound;
            total.filesScanned += report.filesScanned;
            total.failed.addArray(report.failed);
            total.crashed.addArray(report.crashed);
            progress_ = static_cast<float>(++done) / static_cast<float>(std::max(1, totalFiles));
        }
    }

    // The manager may be gone by the time the message thread gets to this.
    juce::MessageManager::callAsync([this, alive = alive_, total]
                                    {
                                        if (!*alive)
                                            return;
                                        save();
                                        if (onScanFinished)
                                            onScanFinished(total);
                                    });
}

std::unique_ptr<juce::AudioPluginInstance> PluginManager::createInstance(const juce::PluginDescription& description, double sampleRate,
                                                                         int blockSize, juce::String& error)
{
    return formats_.createPluginInstance(description, sampleRate, blockSize, error);
}

void PluginManager::save() const
{
    if (listFile_ == juce::File())
        return;

    juce::XmlElement root("DOJO_PLUGINS");
    if (auto list = known_.createXml())
        root.addChildElement(list.release());

    const std::lock_guard lock(scanMutex_);
    auto* paths = root.createNewChildElement("SEARCHPATHS");
    for (const auto& [format, path] : searchPaths_)
    {
        auto* element = paths->createNewChildElement("PATH");
        element->setAttribute("format", format);
        element->setAttribute("value", path.toString());
    }

    auto* reasons = root.createNewChildElement("BLACKLIST");
    for (const auto& [file, reason] : blacklistReasons_)
    {
        auto* element = reasons->createNewChildElement("FILE");
        element->setAttribute("path", file);
        element->setAttribute("reason", reason);
    }

    listFile_.getParentDirectory().createDirectory();
    root.writeTo(listFile_);
}

void PluginManager::load()
{
    if (!listFile_.existsAsFile())
        return;

    const auto root = juce::XmlDocument::parse(listFile_);
    if (root == nullptr || !root->hasTagName("DOJO_PLUGINS"))
        return;

    if (const auto* list = root->getChildByName("KNOWNPLUGINS"))
        known_.recreateFromXml(*list);

    if (const auto* paths = root->getChildByName("SEARCHPATHS"))
        for (const auto* element : paths->getChildIterator())
            searchPaths_[element->getStringAttribute("format")] = juce::FileSearchPath(element->getStringAttribute("value"));

    if (const auto* reasons = root->getChildByName("BLACKLIST"))
        for (const auto* element : reasons->getChildIterator())
            blacklistReasons_[element->getStringAttribute("path")] = element->getStringAttribute("reason");
}

int PluginManager::runScannerProcess(const juce::StringArray& arguments)
{
    const auto valueOf = [&](const juce::String& flag)
    {
        const int index = arguments.indexOf(flag);
        return index >= 0 && index + 1 < arguments.size() ? arguments[index + 1] : juce::String();
    };

    const auto formatName = valueOf("--format");
    const auto file = valueOf("--file");
    const auto output = valueOf("--output");
    if (formatName.isEmpty() || file.isEmpty() || output.isEmpty())
        return 1;

    juce::AudioPluginFormatManager formats;
    addFormats(formats);

    for (auto* format : formats.getFormats())
    {
        if (format->getName() != formatName)
            continue;

        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile(found, file);
        if (found.isEmpty())
            return 2;

        juce::XmlElement root("PLUGINS");
        for (const auto* d : found)
            root.addChildElement(d->createXml().release());
        return root.writeTo(juce::File(output)) ? 0 : 1;
    }

    return 1;
}

} // namespace dojo::plugins

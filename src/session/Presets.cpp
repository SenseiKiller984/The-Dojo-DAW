#include "session/Presets.h"

#include "session/DeviceManager.h"
#include "session/Session.h"

#include <DojoBinaryData.h>

#include <algorithm>
#include <iterator>

namespace dojo::session::presets
{
namespace
{
constexpr const char* kPresetTag = "DOJO_PRESET";
constexpr const char* kDeviceTag = "DEVICE";
constexpr const char* kNativeStateTag = "DOJO_DEVICE";
constexpr const char* kBuiltInPrefix = "builtin:";

juce::String& userFolderOverride()
{
    static juce::String path;
    return path;
}

void sortByName(std::vector<Preset>& presets)
{
    std::stable_sort(presets.begin(), presets.end(), [](const Preset& a, const Preset& b) { return a.name.compareNatural(b.name) < 0; });
}

/** Puts a device's state back to `state` (what "Load Preset" does and undoes). */
class LoadPresetAction final : public juce::UndoableAction
{
public:
    LoadPresetAction(Session& session, DeviceManager& devices, std::uint32_t deviceId, juce::MemoryBlock before, juce::MemoryBlock after)
        : session_(session)
        , devices_(devices)
        , deviceId_(deviceId)
        , before_(std::move(before))
        , after_(std::move(after))
    {
    }

    // (Never fails: a device that is gone has nothing to change, and a failed redo would clear the
    // whole undo history.)
    bool perform() override
    {
        load(after_);
        return true;
    }
    bool undo() override
    {
        load(before_);
        return true;
    }
    int getSizeInUnits() override { return static_cast<int>((before_.getSize() + after_.getSize()) / 64 + 1); }

private:
    void load(const juce::MemoryBlock& state)
    {
        auto node = devices_.findNode(deviceId_);
        if (node == nullptr) // (a redo after its device came back: make its instance now)
            node = devices_.getNode(session_.findDevice(deviceId_));
        if (node != nullptr && state.getSize() > 0)
            node->getProcessor().setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }

    Session& session_;
    DeviceManager& devices_;
    std::uint32_t deviceId_;
    juce::MemoryBlock before_;
    juce::MemoryBlock after_;
};
} // namespace

//==================================================================================================
const DeviceSettings* Preset::findFor(const juce::String& key, int occurrence) const
{
    if (key.isEmpty())
        return nullptr;
    const DeviceSettings* first = nullptr;
    int seen = 0;
    for (const auto& d : devices)
    {
        if (d.key != key)
            continue;
        if (first == nullptr)
            first = &d;
        if (!chain || seen++ == occurrence)
            return &d;
    }
    return first;
}

void Preset::add(DeviceSettings settings)
{
    if (settings.key.isNotEmpty())
        devices.push_back(std::move(settings));
}

void Preset::put(DeviceSettings settings)
{
    if (settings.key.isEmpty())
        return;
    const auto it = std::find_if(devices.begin(), devices.end(), [&](const DeviceSettings& d) { return d.key == settings.key; });
    if (it != devices.end())
        *it = std::move(settings);
    else
        devices.push_back(std::move(settings));
}

juce::StringArray Preset::getDeviceNames() const
{
    juce::StringArray names;
    for (const auto& d : devices)
        names.add(d.deviceName.isNotEmpty() ? d.deviceName : d.key.fromLastOccurrenceOf(":", false, false));
    return names;
}

juce::String Preset::getSourceId() const
{
    return builtIn ? juce::String(kBuiltInPrefix) + name : file.getFullPathName();
}

std::unique_ptr<juce::XmlElement> Preset::toXml() const
{
    auto xml = std::make_unique<juce::XmlElement>(kPresetTag);
    xml->setAttribute("version", kFormatVersion);
    xml->setAttribute("name", name);
    if (description.isNotEmpty())
        xml->setAttribute("description", description);
    if (category.isNotEmpty())
        xml->setAttribute("category", category);
    if (chain)
        xml->setAttribute("chain", 1);

    for (const auto& d : devices)
    {
        auto* element = xml->createNewChildElement(kDeviceTag);
        element->setAttribute("key", d.key);
        element->setAttribute("name", d.deviceName);
        // A native device's settings are written out readably; anything else is kept as it is.
        auto native = d.state.getSize() > 0 ? juce::AudioProcessor::getXmlFromBinary(d.state.getData(), static_cast<int>(d.state.getSize()))
                                            : nullptr;
        if (native != nullptr && native->hasTagName(kNativeStateTag))
            element->addChildElement(native.release());
        else
            element->setAttribute("state", d.state.toBase64Encoding());
    }
    return xml;
}

std::optional<Preset> Preset::fromXml(const juce::XmlElement& xml, juce::String& error)
{
    if (!xml.hasTagName(kPresetTag))
    {
        error = "This is not a Dojo preset.";
        return std::nullopt;
    }
    if (xml.getIntAttribute("version", 1) > kFormatVersion)
    {
        error = "This preset was made by a newer version of The Dojo.";
        return std::nullopt;
    }

    Preset preset;
    preset.name = xml.getStringAttribute("name").trim();
    preset.description = xml.getStringAttribute("description");
    preset.category = xml.getStringAttribute("category");
    preset.chain = xml.getBoolAttribute("chain", false);
    for (const auto* element : xml.getChildWithTagNameIterator(kDeviceTag))
    {
        DeviceSettings d;
        d.key = element->getStringAttribute("key");
        d.deviceName = element->getStringAttribute("name");
        if (const auto* native = element->getFirstChildElement())
            juce::AudioProcessor::copyXmlToBinary(*native, d.state);
        else if (!d.state.fromBase64Encoding(element->getStringAttribute("state")))
            d.state.reset();
        if (d.key.isEmpty() || d.state.getSize() == 0)
            continue;
        if (preset.chain)
            preset.add(std::move(d));
        else
            preset.put(std::move(d));
    }

    if (preset.devices.empty())
    {
        error = "The preset holds no device settings.";
        return std::nullopt;
    }
    if (preset.name.isEmpty())
        preset.name = "Untitled Preset";
    return preset;
}

std::optional<Preset> Preset::parse(const juce::String& text, juce::String& error)
{
    const auto xml = juce::parseXML(text);
    if (xml == nullptr)
    {
        error = "The preset is not readable.";
        return std::nullopt;
    }
    return fromXml(*xml, error);
}

juce::Result Preset::save(const juce::File& target) const
{
    const auto destination = target.hasFileExtension(kExtension) ? target : target.withFileExtension(kExtension);
    const auto xml = toXml();
    if (!destination.getParentDirectory().createDirectory())
        return juce::Result::fail("Could not create " + destination.getParentDirectory().getFullPathName());
    juce::TemporaryFile temp(destination);
    if (!xml->writeTo(temp.getFile()) || !temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not write " + destination.getFullPathName());
    return juce::Result::ok();
}

std::optional<Preset> Preset::load(const juce::File& file, juce::String& error)
{
    if (!file.existsAsFile())
    {
        error = file.getFileName() + " does not exist.";
        return std::nullopt;
    }
    const auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr)
    {
        error = file.getFileName() + " is not a readable preset.";
        return std::nullopt;
    }
    auto preset = fromXml(*xml, error);
    if (!preset)
    {
        error = file.getFileName() + ": " + error;
        return std::nullopt;
    }
    preset->file = file;
    if (!xml->hasAttribute("name"))
        preset->name = file.getFileNameWithoutExtension();
    return preset;
}

//==================================================================================================
juce::String keyOf(const juce::ValueTree& device)
{
    const auto type = device::getType(device);
    if (type != "plugin")
        return type;

    juce::PluginDescription description;
    if (const auto xml = juce::parseXML(device.getProperty(ids::description).toString()))
        if (description.loadFromXml(*xml))
            return keyOf(description);
    return "plugin:" + device::getName(device);
}

juce::String keyOf(const juce::PluginDescription& plugin)
{
    return "plugin:" + plugin.pluginFormatName + ":" + plugin.manufacturerName + ":" + plugin.name;
}

DeviceSettings capture(const juce::ValueTree& device, juce::AudioProcessor& processor)
{
    DeviceSettings settings;
    settings.key = keyOf(device);
    settings.deviceName = device::getName(device);
    processor.getStateInformation(settings.state);
    return settings;
}

void restore(const DeviceSettings& settings, juce::AudioProcessor& processor)
{
    if (settings.state.getSize() > 0)
        processor.setStateInformation(settings.state.getData(), static_cast<int>(settings.state.getSize()));
}

int occurrenceOf(const juce::ValueTree& device)
{
    const auto key = keyOf(device);
    int count = 0;
    for (const auto& other : Session::getDevices(device.getParent()))
    {
        if (other == device)
            return count;
        if (keyOf(other) == key)
            ++count;
    }
    return 0;
}

bool applySettings(const DeviceSettings& settings, const juce::ValueTree& device, Session& session, DeviceManager& devices)
{
    const auto node = devices.getNode(device);
    if (node == nullptr || settings.state.getSize() == 0)
        return false;

    juce::MemoryBlock before;
    node->getProcessor().getStateInformation(before);
    return session.getUndoManager().perform(
        new LoadPresetAction(session, devices, device::getId(device), std::move(before), settings.state));
}

bool apply(const Preset& preset, const juce::ValueTree& device, Session& session, DeviceManager& devices)
{
    const auto* settings = preset.findFor(keyOf(device), occurrenceOf(device));
    return settings != nullptr && applySettings(*settings, device, session, devices);
}

bool isPresetFile(const juce::File& file)
{
    return file.hasFileExtension(kExtension);
}

bool hasNonPresetFile(const juce::StringArray& paths)
{
    return std::any_of(paths.begin(), paths.end(), [](const juce::String& p) { return !isPresetFile(juce::File(p)); });
}

//==================================================================================================
juce::File getUserFolder()
{
    if (userFolderOverride().isNotEmpty())
        return juce::File(userFolderOverride());
    return juce::File::getSpecialLocation(juce::File::userDocumentsDirectory).getChildFile("The Dojo").getChildFile("Presets");
}

void setUserFolder(const juce::File& folder)
{
    userFolderOverride() = folder == juce::File() ? juce::String() : folder.getFullPathName();
}

std::vector<Preset> getBuiltIn()
{
    std::vector<Preset> presets;
    for (int i = 0; i < DojoBinaryData::namedResourceListSize; ++i)
    {
        const juce::String original = DojoBinaryData::originalFilenames[i];
        if (!original.endsWithIgnoreCase(kExtension))
            continue;
        int size = 0;
        const auto* data = DojoBinaryData::getNamedResource(DojoBinaryData::namedResourceList[i], size);
        if (data == nullptr || size <= 0)
            continue;
        juce::String error;
        if (auto preset = Preset::parse(juce::String::fromUTF8(data, size), error))
        {
            preset->builtIn = true;
            presets.push_back(std::move(*preset));
        }
        else
        {
            jassertfalse; // a broken built-in preset
        }
    }
    sortByName(presets);
    return presets;
}

std::vector<Preset> getUserPresets()
{
    std::vector<Preset> presets;
    const auto folder = getUserFolder();
    if (!folder.isDirectory())
        return presets;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, true, kWildcard, juce::File::findFiles))
    {
        juce::String error;
        if (auto preset = Preset::load(entry.getFile(), error))
            presets.push_back(std::move(*preset));
    }
    sortByName(presets);
    return presets;
}

std::vector<Preset> getAll()
{
    auto all = getBuiltIn();
    auto user = getUserPresets();
    all.insert(all.end(), std::make_move_iterator(user.begin()), std::make_move_iterator(user.end()));
    return all;
}

std::vector<Preset> getFor(const juce::String& key)
{
    auto all = getAll();
    all.erase(std::remove_if(all.begin(), all.end(), [&](const Preset& p) { return !p.fits(key); }), all.end());
    return all;
}

std::optional<Preset> findBySourceId(const juce::String& sourceId)
{
    if (sourceId.startsWith(kBuiltInPrefix))
    {
        const auto name = sourceId.fromFirstOccurrenceOf(kBuiltInPrefix, false, false);
        for (auto& preset : getBuiltIn())
            if (preset.name == name)
                return std::move(preset);
        return std::nullopt;
    }
    juce::String error;
    return Preset::load(juce::File(sourceId), error);
}

juce::String fileNameFor(const juce::String& name)
{
    auto cleaned = juce::File::createLegalFileName(name.trim());
    if (cleaned.isEmpty())
        cleaned = "Preset";
    return cleaned + kExtension;
}

} // namespace dojo::session::presets

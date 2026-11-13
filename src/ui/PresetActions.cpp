#include "ui/PresetActions.h"

#include "ui/DeviceActions.h"

namespace dojo::ui::presetActions
{
namespace presets = session::presets;

namespace
{
juce::String quoted(const juce::String& name)
{
    return "\"" + name + "\"";
}

juce::String mismatch(const presets::Preset& preset, const juce::ValueTree& device)
{
    return quoted(preset.name) + " has no settings for " + session::device::getName(device) + " (it is for " +
           preset.getDeviceNames().joinIntoString(", ") + ").";
}

/** The device to add for one entry of a preset, if it is available here. */
std::optional<deviceActions::DeviceChoice> choiceFor(const presets::DeviceSettings& entry, plugins::PluginManager& plugins)
{
    for (const auto& choice : deviceActions::getNativeChoices())
        if (choice.type == entry.key)
            return choice;
    if (entry.key.startsWith("plugin:"))
        for (const auto& choice : deviceActions::getPluginChoices(plugins))
        {
            juce::PluginDescription description;
            if (const auto xml = juce::parseXML(choice.descriptionXml); xml != nullptr && description.loadFromXml(*xml))
                if (presets::keyOf(description) == entry.key)
                    return choice;
        }
    return std::nullopt;
}
} // namespace

juce::String toDragDescription(const presets::Preset& preset)
{
    return juce::String(kDragPrefix) + preset.getSourceId();
}

bool isPresetDrag(const juce::var& description)
{
    return description.isString() && description.toString().startsWith(kDragPrefix);
}

std::optional<presets::Preset> fromDragDescription(const juce::var& description)
{
    if (!isPresetDrag(description))
        return std::nullopt;
    return presets::findBySourceId(description.toString().fromFirstOccurrenceOf(kDragPrefix, false, false));
}

Outcome load(session::Project& project, const presets::Preset& preset, const juce::ValueTree& device)
{
    auto& session = project.getSession();
    if (!preset.fits(presets::keyOf(device)))
        return {false, mismatch(preset, device)};
    session.beginTransaction("Load Preset " + quoted(preset.name));
    if (!presets::apply(preset, device, session, project.getDevices()))
        return {false, session::device::getName(device) + " is not available."};
    return {true, "Loaded " + quoted(preset.name)};
}

Outcome loadFirstFitting(session::Project& project, const juce::StringArray& files, const juce::ValueTree& device)
{
    std::optional<presets::Preset> firstRead;
    juce::String error;
    for (const auto& path : files)
    {
        const juce::File file(path);
        if (!presets::isPresetFile(file))
            continue;
        juce::String problem;
        auto preset = presets::Preset::load(file, problem);
        if (!preset)
        {
            if (error.isEmpty())
                error = problem;
            continue;
        }
        if (preset->fits(presets::keyOf(device)))
            return load(project, *preset, device);
        if (!firstRead)
            firstRead = std::move(preset);
    }
    if (firstRead)
        return {false, mismatch(*firstRead, device)};
    return {false, error.isNotEmpty() ? error : juce::String("That is not a Dojo preset (.dojopreset).")};
}

Outcome save(session::Project& project, const juce::ValueTree& device, const juce::File& file)
{
    const auto node = project.getDevices().getNode(device);
    if (node == nullptr)
        return {false, session::device::getName(device) + " is not available."};
    presets::Preset preset;
    preset.name = file.getFileNameWithoutExtension();
    preset.put(presets::capture(device, node->getProcessor()));
    if (const auto result = preset.save(file); result.failed())
        return {false, result.getErrorMessage()};
    return {true, "Saved preset " + quoted(preset.name)};
}

Outcome addToFile(session::Project& project, const juce::ValueTree& device, const juce::File& file)
{
    const auto node = project.getDevices().getNode(device);
    if (node == nullptr)
        return {false, session::device::getName(device) + " is not available."};
    juce::String error;
    auto preset = presets::Preset::load(file, error);
    if (!preset)
        return {false, error};
    const bool replacing = preset->fits(presets::keyOf(device));
    preset->put(presets::capture(device, node->getProcessor()));
    if (const auto result = preset->save(file); result.failed())
        return {false, result.getErrorMessage()};
    const auto name = session::device::getName(device);
    return {true, replacing ? "Replaced the " + name + " settings in " + quoted(preset->name)
                            : "Added " + name + " to " + quoted(preset->name) + " (" + preset->getDeviceNames().joinIntoString(", ") + ")"};
}

Outcome saveChain(session::Project& project, const juce::ValueTree& owner, const juce::File& file)
{
    presets::Preset preset;
    preset.name = file.getFileNameWithoutExtension();
    for (const auto& device : session::Session::getDevices(owner))
    {
        if (preset.fits(presets::keyOf(device)))
            continue; // (one entry per kind of device: the first one on the chain)
        if (const auto node = project.getDevices().getNode(device))
            preset.put(presets::capture(device, node->getProcessor()));
    }
    if (preset.devices.empty())
        return {false, "There are no devices to save."};
    if (const auto result = preset.save(file); result.failed())
        return {false, result.getErrorMessage()};
    return {true, "Saved preset " + quoted(preset.name) + " (" + preset.getDeviceNames().joinIntoString(", ") + ")"};
}

juce::ValueTree addDevice(session::Project& project, plugins::PluginManager& plugins, EditorState& editor, juce::ValueTree owner,
                          const presets::Preset& preset, juce::String* message)
{
    for (const auto& entry : preset.devices)
    {
        const auto choice = choiceFor(entry, plugins);
        if (!choice)
            continue;
        auto& session = project.getSession();
        auto device = deviceActions::addDevice(session, editor, owner, *choice); // (begins the undo step)
        if (!device.isValid())
            break;
        (void) presets::apply(preset, device, session, project.getDevices()); // (same undo step)
        if (message != nullptr)
            *message = "Added " + choice->name + " with " + quoted(preset.name);
        return device;
    }
    if (message != nullptr)
        *message = "None of the devices " + quoted(preset.name) + " is for (" + preset.getDeviceNames().joinIntoString(", ") +
                   ") is available.";
    return {};
}

Outcome useOnSelection(session::Project& project, plugins::PluginManager& plugins, EditorState& editor, const presets::Preset& preset)
{
    return useOn(project, plugins, editor, selectedOwner(project.getSession(), editor), preset);
}

Outcome useOn(session::Project& project, plugins::PluginManager& plugins, EditorState& editor, const juce::ValueTree& owner,
              const presets::Preset& preset)
{
    if (owner.isValid())
        for (const auto& device : session::Session::getDevices(owner))
            if (preset.fits(presets::keyOf(device)))
                return load(project, preset, device);

    juce::String message;
    const auto device = addDevice(project, plugins, editor, owner, preset, &message);
    return {device.isValid(), message};
}

juce::ValueTree selectedOwner(session::Session& session, const EditorState& editor)
{
    if (editor.isMasterSelected())
        return session.getMaster();
    return session.findTrack(editor.getSelectedTrack());
}

} // namespace dojo::ui::presetActions

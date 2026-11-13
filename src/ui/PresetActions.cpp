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
    preset.chain = true;
    for (const auto& device : session::Session::getDevices(owner))
        if (const auto node = project.getDevices().getNode(device))
            preset.add(presets::capture(device, node->getProcessor()));
    if (preset.devices.empty())
        return {false, "There are no devices to save."};
    if (const auto result = preset.save(file); result.failed())
        return {false, result.getErrorMessage()};
    return {true, "Saved chain " + quoted(preset.name) + " (" + preset.getDeviceNames().joinIntoString(", ") + ")"};
}

juce::ValueTree addChain(session::Project& project, plugins::PluginManager& plugins, EditorState& editor, juce::ValueTree owner,
                         const presets::Preset& preset, juce::String* message)
{
    auto& session = project.getSession();
    juce::StringArray missing;
    juce::ValueTree first;
    juce::ValueTree chainOwner;
    int added = 0;
    for (const auto& entry : preset.devices)
    {
        auto choice = choiceFor(entry, plugins);
        if (!choice)
        {
            missing.add(entry.deviceName.isNotEmpty() ? entry.deviceName : entry.key);
            continue;
        }
        if (entry.deviceName.isNotEmpty())
            choice->name = entry.deviceName; // (the device's role in the chain: "Slapback Delay")

        juce::ValueTree device;
        if (!first.isValid())
        {
            // The first device picks the track as adding a device does (a new one when needed) and
            // begins the undo step; the rest follow it on the same chain.
            device = deviceActions::addDevice(session, editor, owner, *choice);
            session.getUndoManager().setCurrentTransactionName("Add Chain " + quoted(preset.name));
            first = device;
            chainOwner = device.getParent();
        }
        else
        {
            device = session.addDevice(chainOwner, choice->type, choice->name);
            if (choice->descriptionXml.isNotEmpty())
                device.setProperty(session::ids::description, choice->descriptionXml, &session.getUndoManager());
        }
        if (!device.isValid())
            break;
        (void) presets::applySettings(entry, device, session, project.getDevices()); // (same undo step)
        ++added;
    }

    if (message != nullptr)
    {
        if (added == 0)
            *message = "None of the devices of " + quoted(preset.name) + " (" + preset.getDeviceNames().joinIntoString(", ") +
                       ") is available.";
        else
            *message = "Added chain " + quoted(preset.name) + " (" + juce::String(added) + (added == 1 ? " device)" : " devices)") +
                       (missing.isEmpty() ? juce::String() : ", without " + missing.joinIntoString(", ") + " (not installed)");
    }
    return first;
}

juce::ValueTree addDevice(session::Project& project, plugins::PluginManager& plugins, EditorState& editor, juce::ValueTree owner,
                          const presets::Preset& preset, juce::String* message)
{
    if (preset.chain)
        return addChain(project, plugins, editor, std::move(owner), preset, message);

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
    if (owner.isValid() && !preset.chain)
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

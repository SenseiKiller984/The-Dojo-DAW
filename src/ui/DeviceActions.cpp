#include "ui/DeviceActions.h"

#include "devices/DeviceRegistry.h"
#include "ui/EditActions.h"

#include <map>

namespace dojo::ui::deviceActions
{
namespace
{
constexpr const char* kPrefix = "DojoDevice:";
constexpr const char* kPluginPrefix = "DojoDevice:plugin:";
} // namespace

std::vector<DeviceChoice> getNativeChoices()
{
    std::vector<DeviceChoice> choices;
    for (const auto& info : devices::getBuiltinDevices())
        choices.push_back({info.type, info.name, info.category, info.isInstrument, {}, {}});
    return choices;
}

std::vector<DeviceChoice> getPluginChoices(plugins::PluginManager& plugins)
{
    std::vector<DeviceChoice> choices;
    for (const auto& description : plugins.getKnownPlugins().getTypes())
    {
        DeviceChoice choice;
        choice.type = plugins::PluginManager::kDeviceType;
        choice.name = description.name;
        choice.category = description.manufacturerName.isNotEmpty() ? description.manufacturerName : description.pluginFormatName;
        choice.instrument = description.isInstrument;
        if (auto xml = description.createXml())
            choice.descriptionXml = xml->toString(juce::XmlElement::TextFormat().singleLine());
        choice.identifier = description.createIdentifierString();
        choices.push_back(std::move(choice));
    }
    std::sort(choices.begin(), choices.end(), [](const auto& a, const auto& b) { return a.name.compareIgnoreCase(b.name) < 0; });
    return choices;
}

juce::String toDragDescription(const DeviceChoice& choice)
{
    if (choice.type == plugins::PluginManager::kDeviceType)
        return juce::String(kPluginPrefix) + choice.identifier;
    return juce::String(kPrefix) + choice.type;
}

std::optional<DeviceChoice> fromDragDescription(const juce::String& description, plugins::PluginManager& plugins)
{
    if (description.startsWith(kPluginPrefix))
    {
        const auto identifier = description.fromFirstOccurrenceOf(kPluginPrefix, false, false);
        for (const auto& choice : getPluginChoices(plugins))
            if (choice.identifier == identifier)
                return choice;
        return std::nullopt;
    }

    if (description.startsWith(kPrefix))
    {
        const auto type = description.fromFirstOccurrenceOf(kPrefix, false, false);
        for (const auto& choice : getNativeChoices())
            if (choice.type == type)
                return choice;
    }
    return std::nullopt;
}

bool isInstrumentDevice(const juce::ValueTree& device)
{
    const auto type = session::device::getType(device);
    if (const auto* info = devices::findBuiltinDevice(type))
        return info->isInstrument;
    if (type == plugins::PluginManager::kDeviceType)
        if (const auto xml = juce::parseXML(device.getProperty(session::ids::description).toString()))
        {
            juce::PluginDescription description;
            return description.loadFromXml(*xml) && description.isInstrument;
        }
    return false;
}

juce::ValueTree addDevice(session::Session& session, EditorState& editor, juce::ValueTree owner, const DeviceChoice& choice)
{
    const bool isMaster = owner.hasType(session::ids::master);

    if (!isMaster)
    {
        const bool ownerIsInstrumentTrack = owner.isValid() && session::track::getKind(owner) == session::TrackKind::instrument;
        const auto ownerKind = owner.isValid() ? session::track::getKind(owner) : session::TrackKind::audio;
        if (choice.instrument && !ownerIsInstrumentTrack)
        {
            // An instrument dropped on a MIDI track gets its own instrument track, played by that MIDI track.
            const auto midiTrack = ownerKind == session::TrackKind::midi ? owner : juce::ValueTree();
            owner = actions::addTrack(session, editor, session::TrackKind::instrument);
            session::track::setName(owner, choice.name, &session.getUndoManager());
            if (midiTrack.isValid())
                session::track::setMidiTarget(midiTrack, session::track::getId(owner), &session.getUndoManager());
        }
        else if (!owner.isValid() || ownerKind == session::TrackKind::vca || ownerKind == session::TrackKind::midi)
        {
            owner = actions::addTrack(session, editor, session::TrackKind::audio);
        }
        else
        {
            session.beginTransaction("Add " + choice.name);
        }
        editor.selectTrack(session::track::getId(owner));
    }
    else
    {
        session.beginTransaction("Add " + choice.name);
    }

    // Instruments lead the chain; a new instrument replaces the track's instrument (as one undo step).
    int index = -1;
    if (choice.instrument)
    {
        index = 0;
        const auto devices = session::Session::getDevices(owner);
        if (!isMaster && !devices.empty() && isInstrumentDevice(devices.front()))
        {
            // A track still named after its instrument follows the new one.
            if (session::track::getName(owner) == session::device::getName(devices.front()))
                session::track::setName(owner, choice.name, &session.getUndoManager());
            session.removeDevice(devices.front());
        }
    }

    auto device = session.addDevice(owner, choice.type, choice.name, index);
    if (choice.descriptionXml.isNotEmpty())
        device.setProperty(session::ids::description, choice.descriptionXml, &session.getUndoManager());
    return device;
}

void buildAddDeviceMenu(juce::PopupMenu& menu, plugins::PluginManager& plugins, bool includeInstruments,
                        std::function<void(const DeviceChoice&)> onChosen)
{
    std::map<juce::String, juce::PopupMenu> nativeByCategory;
    for (const auto& choice : getNativeChoices())
    {
        if (choice.instrument && !includeInstruments)
            continue;
        nativeByCategory[choice.category].addItem(choice.name, [onChosen, choice] { onChosen(choice); });
    }

    menu.addSectionHeader("The Dojo");
    for (auto& [category, sub] : nativeByCategory)
        menu.addSubMenu(category, sub);

    std::map<juce::String, juce::PopupMenu> pluginsByVendor;
    for (const auto& choice : getPluginChoices(plugins))
    {
        if (choice.instrument && !includeInstruments)
            continue;
        pluginsByVendor[choice.category].addItem(choice.name, [onChosen, choice] { onChosen(choice); });
    }

    menu.addSectionHeader("Plug-ins");
    if (pluginsByVendor.empty())
        menu.addItem("No plug-ins found - scan in the browser's Plug-ins tab", false, false, [] {});
    for (auto& [vendor, sub] : pluginsByVendor)
        menu.addSubMenu(vendor, sub);
}

} // namespace dojo::ui::deviceActions

#include "ui/DeviceActions.h"
#include "licensing/Features.h"

#include "devices/DeviceRegistry.h"
#include "devices/HardwareInsert.h"
#include "ui/EditActions.h"

#include <algorithm>
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
        choices.push_back({info.type, info.name, info.category, info.isInstrument, {}, {}, info.keywords, info.isMidiEffect});
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

bool isMidiEffectDevice(const juce::ValueTree& device)
{
    const auto* info = devices::findBuiltinDevice(session::device::getType(device));
    return info != nullptr && info->isMidiEffect;
}

juce::ValueTree findInstrument(const juce::ValueTree& owner)
{
    for (const auto& device : session::Session::getDevices(owner))
        if (isInstrumentDevice(device))
            return device;
    return {};
}

namespace
{
/** Where a device goes in a chain's order: MIDI effects, the instrument, audio effects. */
int rankOf(const juce::ValueTree& device)
{
    return isMidiEffectDevice(device) ? 0 : isInstrumentDevice(device) ? 1 : 2;
}

/** How many MIDI effects lead the chain (where an instrument or the next MIDI effect goes). */
int leadingMidiEffects(const juce::ValueTree& owner)
{
    int count = 0;
    for (const auto& device : session::Session::getDevices(owner))
    {
        if (!isMidiEffectDevice(device))
            break;
        ++count;
    }
    return count;
}
} // namespace

int insertionIndex(const juce::ValueTree& owner, const DeviceChoice& choice)
{
    return choice.midiEffect || choice.instrument ? leadingMidiEffects(owner) : -1;
}

bool canMoveDevice(const std::vector<juce::ValueTree>& devices, int from, int to)
{
    const int count = static_cast<int>(devices.size());
    if (from < 0 || from >= count || to < 0 || to >= count)
        return false;
    std::vector<int> ranks;
    for (const auto& device : devices)
        ranks.push_back(rankOf(device));
    const int moved = ranks[static_cast<std::size_t>(from)];
    ranks.erase(ranks.begin() + from);
    ranks.insert(ranks.begin() + to, moved);
    return std::is_sorted(ranks.begin(), ranks.end());
}

bool isLicensed(const DeviceChoice& choice)
{
    return !licensing::isProDevice(choice.type) || licensing::gate::allows(licensing::Feature::proDevices);
}

juce::ValueTree addDevice(session::Session& session, EditorState& editor, juce::ValueTree owner, const DeviceChoice& choice)
{
    if (!isLicensed(choice))
        return {}; // (the license: the pro devices need Pro)
    const bool isMaster = owner.hasType(session::ids::master);

    if (choice.midiEffect)
    {
        // MIDI effects work on a track's MIDI: an instrument track's or a MIDI track's. Anywhere
        // else they get a MIDI track of their own (which plays the instrument track chosen for it).
        const auto kind = owner.hasType(session::ids::track) ? session::track::getKind(owner) : session::TrackKind::audio;
        if (!owner.hasType(session::ids::track) || (kind != session::TrackKind::instrument && kind != session::TrackKind::midi))
            owner = actions::addTrack(session, editor, session::TrackKind::midi);
        else
            session.beginTransaction("Add " + choice.name);
        editor.selectTrack(session::track::getId(owner));
        return session.addDevice(owner, choice.type, choice.name, leadingMidiEffects(owner));
    }

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

    // Instruments go after the MIDI effects; a new instrument replaces the track's instrument (as one
    // undo step).
    int index = -1;
    if (choice.instrument)
    {
        if (const auto old = findInstrument(owner); !isMaster && old.isValid())
        {
            // A track still named after its instrument follows the new one.
            if (session::track::getName(owner) == session::device::getName(old))
                session::track::setName(owner, choice.name, &session.getUndoManager());
            session.removeDevice(old);
        }
        index = leadingMidiEffects(owner);
    }

    auto device = session.addDevice(owner, choice.type, choice.name, index);
    if (choice.descriptionXml.isNotEmpty())
        device.setProperty(session::ids::description, choice.descriptionXml, &session.getUndoManager());
    return device;
}

bool fitsInLoop(const DeviceChoice& choice)
{
    return !choice.instrument && !choice.midiEffect && choice.type != "builtin:lfo" && choice.type != "builtin:cvin";
}

juce::ValueTree findOrAddBus(session::Session& session, const juce::String& name)
{
    for (const auto& t : session.getAllTracks())
        if (session::track::getKind(t) == session::TrackKind::bus && session::track::getName(t) == name)
            return t;
    return session.addTrack(session::TrackKind::bus, name);
}

juce::ValueTree connectDeviceOutput(session::Session& session, const juce::ValueTree& device, int left, int right, const juce::String& busName)
{
    const auto track = session::Session::getDeviceOwner(device);
    if (!track.isValid() || !track.hasType(session::ids::track))
        return {};
    if (auto existing = session::Session::findDeviceSend(track, session::device::getId(device), left, right); existing.isValid())
        return existing;
    session.beginTransaction("Connect " + session::device::getName(device) + " to " + busName);
    const auto bus = findOrAddBus(session, busName);
    return session.addDeviceSend(track, bus, device, left, right);
}

void disconnectDeviceOutput(session::Session& session, const juce::ValueTree& device, int left, int right)
{
    const auto track = session::Session::getDeviceOwner(device);
    if (auto existing = session::Session::findDeviceSend(track, session::device::getId(device), left, right); existing.isValid())
    {
        session.beginTransaction("Disconnect " + session::device::getName(device));
        session.removeSend(existing);
    }
}

juce::ValueTree connectedBus(const session::Session& session, const juce::ValueTree& device, int left, int right)
{
    const auto track = session::Session::getDeviceOwner(device);
    const auto existing = session::Session::findDeviceSend(track, session::device::getId(device), left, right);
    return existing.isValid() ? session.findTrack(session::send::getTarget(existing)) : juce::ValueTree();
}

juce::ValueTree addDeviceToLoop(session::Session& session, juce::ValueTree host, const DeviceChoice& choice)
{
    if (!host.isValid() || !host.hasType(session::ids::device) || !fitsInLoop(choice) || !isLicensed(choice))
        return {};
    session.beginTransaction("Add " + choice.name + " to the Loop");
    auto device = session.addDevice(host, choice.type, choice.name);
    if (choice.descriptionXml.isNotEmpty())
        device.setProperty(session::ids::description, choice.descriptionXml, &session.getUndoManager());
    return device;
}

void buildAddDeviceMenu(juce::PopupMenu& menu, plugins::PluginManager& plugins, MenuScope scope,
                        std::function<void(const DeviceChoice&)> onChosen)
{
    const auto wanted = [scope](const DeviceChoice& choice)
    { return choice.instrument ? scope.instruments : choice.midiEffect ? scope.midiEffects : scope.audioEffects; };
    std::map<juce::String, juce::PopupMenu> nativeByCategory;
    for (const auto& choice : getNativeChoices())
    {
        if (!wanted(choice))
            continue;
        nativeByCategory[choice.category].addItem(choice.name + (isLicensed(choice) ? "" : "  (Pro)"), isLicensed(choice), false,
                                                  [onChosen, choice] { onChosen(choice); });
    }

    menu.addSectionHeader("The Dojo");
    for (auto& [category, sub] : nativeByCategory)
        menu.addSubMenu(category, sub);

    std::map<juce::String, juce::PopupMenu> pluginsByVendor;
    for (const auto& choice : getPluginChoices(plugins))
    {
        if (!wanted(choice))
            continue;
        pluginsByVendor[choice.category].addItem(choice.name, [onChosen, choice] { onChosen(choice); });
    }

    menu.addSectionHeader("Plug-ins");
    if (pluginsByVendor.empty())
        menu.addItem("No plug-ins found - scan in the browser's Plug-ins tab", false, false, [] {});
    for (auto& [vendor, sub] : pluginsByVendor)
        menu.addSubMenu(vendor, sub);
}

bool usesHardwareInserts(const session::Session& session)
{
    const auto uses = [](const juce::ValueTree& owner)
    {
        for (const auto& device : session::Session::getAllDevices(owner))
            if (session::device::getType(device) == devices::HardwareInsert::kTypeId && !session::device::isBypassed(device))
                return true;
        return false;
    };
    for (const auto& track : session.getAllTracks())
        if (session::track::isActive(track) && uses(track))
            return true;
    return uses(session.getMaster());
}

} // namespace dojo::ui::deviceActions

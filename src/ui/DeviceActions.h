#pragma once

#include "plugins/PluginManager.h"
#include "session/Session.h"
#include "ui/EditorState.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <optional>
#include <vector>

namespace dojo::ui::deviceActions
{

/** Something that can be added to a device chain: a native device or an installed plug-in. */
struct DeviceChoice
{
    juce::String type;           ///< "builtin:..." or PluginManager::kDeviceType
    juce::String name;
    juce::String category;       ///< native category, or the plug-in's manufacturer / format
    bool instrument = false;
    juce::String descriptionXml; ///< plug-ins: the PluginDescription
    juce::String identifier;     ///< plug-ins: PluginDescription::createIdentifierString()
};

/** Every native device. */
std::vector<DeviceChoice> getNativeChoices();
/** Every installed (scanned) plug-in. */
std::vector<DeviceChoice> getPluginChoices(plugins::PluginManager& plugins);

/** Drag-and-drop description for a choice ("DojoDevice:..."). */
juce::String toDragDescription(const DeviceChoice& choice);
/** The choice a drag description refers to (plug-ins are looked up in the known list). */
std::optional<DeviceChoice> fromDragDescription(const juce::String& description, plugins::PluginManager& plugins);

/** True for instruments (native or hosted), which lead a track's chain. */
[[nodiscard]] bool isInstrumentDevice(const juce::ValueTree& device);

/**
    Adds a device to `owner` (a track or the MASTER node) as one undo step and returns the DEVICE.
    Instruments go first in the chain and replace the track's current instrument; an instrument
    dropped onto a track that is not an instrument track (or onto nothing) gets a new instrument
    track. An effect dropped onto nothing gets a new audio track. The (new) track is selected.
*/
juce::ValueTree addDevice(session::Session& session, EditorState& editor, juce::ValueTree owner, const DeviceChoice& choice);

/** Fills `menu` with native devices and plug-ins (instruments only if `includeInstruments`). */
void buildAddDeviceMenu(juce::PopupMenu& menu, plugins::PluginManager& plugins, bool includeInstruments,
                        std::function<void(const DeviceChoice&)> onChosen);

} // namespace dojo::ui::deviceActions

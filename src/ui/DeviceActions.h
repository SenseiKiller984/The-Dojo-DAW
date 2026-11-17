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
    juce::String keywords = {};  ///< more words the search finds it by
    bool midiEffect = false;     ///< plays on the MIDI, before the instrument
};

/** Every native device. */
std::vector<DeviceChoice> getNativeChoices();
/** Every installed (scanned) plug-in. */
std::vector<DeviceChoice> getPluginChoices(plugins::PluginManager& plugins);

/** Drag-and-drop description for a choice ("DojoDevice:..."). */
juce::String toDragDescription(const DeviceChoice& choice);
/** The choice a drag description refers to (plug-ins are looked up in the known list). */
std::optional<DeviceChoice> fromDragDescription(const juce::String& description, plugins::PluginManager& plugins);

/** True for instruments (native or hosted). */
[[nodiscard]] bool isInstrumentDevice(const juce::ValueTree& device);
/** True for MIDI effects (Arpeggiator, Chord Generator, Scale, MIDI Filter). */
[[nodiscard]] bool isMidiEffectDevice(const juce::ValueTree& device);
/** A track's instrument (the first instrument in its chain), or an invalid tree. */
[[nodiscard]] juce::ValueTree findInstrument(const juce::ValueTree& owner);
/**
    A chain's order: its MIDI effects first (they change the MIDI the instrument plays), then the
    instrument, then the audio effects. Whether moving device `from` to position `to` keeps it.
*/
[[nodiscard]] bool canMoveDevice(const std::vector<juce::ValueTree>& devices, int from, int to);
/** Where a new device of this kind goes in `owner`'s chain to keep that order (-1: at the end). */
[[nodiscard]] int insertionIndex(const juce::ValueTree& owner, const DeviceChoice& choice);

/**
    Adds a device to `owner` (a track or the MASTER node) as one undo step and returns the DEVICE.
    - Instruments go after the track's MIDI effects and replace its current instrument; an
      instrument dropped onto a track that is not an instrument track (or onto nothing) gets a new
      instrument track.
    - MIDI effects go after the track's other MIDI effects, before its instrument; on anything but
      an instrument or MIDI track they get a new MIDI track.
    - An effect dropped onto nothing gets a new audio track.
    The (new) track is selected.
*/
juce::ValueTree addDevice(session::Session& session, EditorState& editor, juce::ValueTree owner, const DeviceChoice& choice);

/** Whether a choice can go into a device's insert loop (an accessory / effects loop): audio effects
    only (no instruments or MIDI effects, and no modulators such as the LFO, which move other
    devices' controls). */
[[nodiscard]] bool fitsInLoop(const DeviceChoice& choice);
/** Adds an effect at the end of `host`'s insert loop as one undo step and returns the DEVICE (or
    nothing when it does not fit in a loop). */
juce::ValueTree addDeviceToLoop(session::Session& session, juce::ValueTree host, const DeviceChoice& choice);

// --- A device's own outputs (a console channel's buses, aux and cue sends) to busses ----------------
/** The DAW bus called `name`, made (as a bus at the end of the track list) when there is none yet,
    in the current undo step. */
juce::ValueTree findOrAddBus(session::Session& session, const juce::String& name);
/** Connects a device's extra outputs `left` / `right` to the bus called `busName` (made when needed)
    with a device send, as one undo step; returns the send (the one there already, if any). Null
    when the device is not on a track (the master has no sends) or the bus would close a loop. */
juce::ValueTree connectDeviceOutput(session::Session& session, const juce::ValueTree& device, int left, int right,
                                    const juce::String& busName);
/** Removes that connection (one undo step). */
void disconnectDeviceOutput(session::Session& session, const juce::ValueTree& device, int left, int right);
/** The bus a device's outputs are connected to (invalid if none). */
[[nodiscard]] juce::ValueTree connectedBus(const session::Session& session, const juce::ValueTree& device, int left, int right);

/** What an Add Device menu offers. */
struct MenuScope
{
    bool instruments = false;
    bool midiEffects = false;
    bool audioEffects = true;
};
/** Fills `menu` with the native devices and plug-ins `scope` allows. */
void buildAddDeviceMenu(juce::PopupMenu& menu, plugins::PluginManager& plugins, MenuScope scope,
                        std::function<void(const DeviceChoice&)> onChosen);

} // namespace dojo::ui::deviceActions

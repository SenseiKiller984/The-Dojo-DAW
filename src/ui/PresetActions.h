#pragma once

#include "plugins/PluginManager.h"
#include "session/Presets.h"
#include "session/Project.h"
#include "ui/EditorState.h"

#include <optional>

/**
    What the device cards, the Devices panel and the browser do with presets (session/Presets.h):
    load one into a device, save a device's settings as a preset (on its own, added to an existing
    preset file, or the whole chain in one file), and add a device that starts from a preset.

    Every function returns a short message for the user (what happened, or why nothing did).
*/
namespace dojo::ui::presetActions
{

/** Drag description of a preset dragged from the browser: "DojoPreset:<source id>". */
inline constexpr const char* kDragPrefix = "DojoPreset:";
[[nodiscard]] juce::String toDragDescription(const session::presets::Preset& preset);
[[nodiscard]] bool isPresetDrag(const juce::var& description);
[[nodiscard]] std::optional<session::presets::Preset> fromDragDescription(const juce::var& description);

struct Outcome
{
    bool ok = false;
    juce::String message;
};

/** Loads `preset` into `device` as one undo step; explains a preset that has nothing for it. */
Outcome load(session::Project& project, const session::presets::Preset& preset, const juce::ValueTree& device);
/** Loads the first of `files` (dropped preset files, in order) that fits `device`. */
Outcome loadFirstFitting(session::Project& project, const juce::StringArray& files, const juce::ValueTree& device);

/** Saves `device`'s settings as a new preset file (named after the file). */
Outcome save(session::Project& project, const juce::ValueTree& device, const juce::File& file);
/** Adds (or replaces) `device`'s settings in an existing preset file, keeping the others. */
Outcome addToFile(session::Project& project, const juce::ValueTree& device, const juce::File& file);
/** Saves every device on `owner`'s chain in one preset file (the first device of each kind). */
Outcome saveChain(session::Project& project, const juce::ValueTree& owner, const juce::File& file);

/**
    Adds a device that starts from `preset` to `owner` (as deviceActions::addDevice does): the first
    kind of device in the preset that is available (a native device, or an installed plug-in).
    Adding and loading are one undo step. Returns the new DEVICE (invalid when none was added).
*/
juce::ValueTree addDevice(session::Project& project, plugins::PluginManager& plugins, EditorState& editor, juce::ValueTree owner,
                          const session::presets::Preset& preset, juce::String* message = nullptr);

/**
    What dropping a preset on a track does: loads it into the first device on `owner`'s chain that it
    fits, else adds a device that starts from it.
*/
Outcome useOn(session::Project& project, plugins::PluginManager& plugins, EditorState& editor, const juce::ValueTree& owner,
              const session::presets::Preset& preset);

/** What double-clicking a preset in the browser does: useOn() the selected track (or the master). */
Outcome useOnSelection(session::Project& project, plugins::PluginManager& plugins, EditorState& editor,
                       const session::presets::Preset& preset);

/** The device chain's owner that is selected (a track, the master, or nothing). */
[[nodiscard]] juce::ValueTree selectedOwner(session::Session& session, const EditorState& editor);

} // namespace dojo::ui::presetActions

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

#include <optional>
#include <vector>

namespace dojo::session
{
class DeviceManager;
class Session;
} // namespace dojo::session

/**
    Device presets: The Dojo's ".dojopreset" files.

    A preset file is an XML document (like a ".dojo" session) holding the complete settings of one or
    more devices, all for the same purpose:

        <DOJO_PRESET version="1" name="Synth Bite" description="...">
          <DEVICE key="builtin:eq" name="EQ">
            <DOJO_DEVICE type="builtin:eq" version="1"> <PARAM id="band1Freq" value="85"/> ... </DOJO_DEVICE>
          </DEVICE>
          <DEVICE key="builtin:channeleq" name="Channel EQ"> ... </DEVICE>
        </DOJO_PRESET>

    Each DEVICE entry is for one kind of device, named by its `key`: a native device's type, or
    "plugin:<format>:<manufacturer>:<name>" for a hosted plug-in. Native devices' settings are
    written out readably (every parameter by id and value, plus whatever else the device keeps);
    a plug-in's are its own state, base64-encoded. A file holds at most one entry per kind of
    device, so a device loading a preset always knows which settings are its own: an EQ takes the
    EQ entry, a Channel EQ the Channel EQ entry, and a preset with no entry for a device does not
    fit it.

    Presets come from two places: the built-in ones compiled into The Dojo (resources/presets) and
    the user's presets folder (Documents/The Dojo/Presets, searched with its sub-folders).
*/
namespace dojo::session::presets
{

inline constexpr const char* kExtension = ".dojopreset";
inline constexpr const char* kWildcard = "*.dojopreset";
inline constexpr int kFormatVersion = 1;

/** One device's settings inside a preset. */
struct DeviceSettings
{
    juce::String key;        ///< the kind of device it is for (see keyOf())
    juce::String deviceName; ///< that device's name, for display ("EQ")
    juce::MemoryBlock state; ///< what the device's getStateInformation() gave
};

/** A preset: a name, a description and settings for one or more kinds of device. */
struct Preset
{
    juce::String name;
    juce::String description;
    std::vector<DeviceSettings> devices;
    bool builtIn = false;
    juce::File file; ///< where it was read from (none for a built-in one)

    /** The settings for the device kind `key`, or nullptr when the preset does not fit it. */
    [[nodiscard]] const DeviceSettings* findFor(const juce::String& key) const;
    [[nodiscard]] bool fits(const juce::String& key) const { return findFor(key) != nullptr; }
    /** Adds settings, replacing those already there for the same kind of device. */
    void put(DeviceSettings settings);
    /** Names of the devices it has settings for ("EQ, Channel EQ"). */
    [[nodiscard]] juce::StringArray getDeviceNames() const;
    /** "builtin:<name>" for a built-in preset, else the file's full path. */
    [[nodiscard]] juce::String getSourceId() const;

    [[nodiscard]] std::unique_ptr<juce::XmlElement> toXml() const;
    [[nodiscard]] static std::optional<Preset> fromXml(const juce::XmlElement& xml, juce::String& error);
    [[nodiscard]] static std::optional<Preset> parse(const juce::String& text, juce::String& error);

    /** Writes the preset to `file` (".dojopreset" is added when missing). */
    [[nodiscard]] juce::Result save(const juce::File& file) const;
    [[nodiscard]] static std::optional<Preset> load(const juce::File& file, juce::String& error);
};

/** The kind of device a DEVICE tree is: its native type, or "plugin:<format>:<manufacturer>:<name>". */
[[nodiscard]] juce::String keyOf(const juce::ValueTree& device);
/** The kind of device a hosted plug-in is. */
[[nodiscard]] juce::String keyOf(const juce::PluginDescription& plugin);

/** Captures a device's complete current settings. */
[[nodiscard]] DeviceSettings capture(const juce::ValueTree& device, juce::AudioProcessor& processor);

/** Loads settings into a processor (no undo; see apply()). */
void restore(const DeviceSettings& settings, juce::AudioProcessor& processor);

/**
    Loads the preset's settings for `device` into it, recorded in the session's undo history (in the
    current transaction: begin one first to make it its own step). Undo puts the device back as it
    was. Returns false (and changes nothing) when the preset has no settings for that kind of device
    or the device has no instance.
*/
bool apply(const Preset& preset, const juce::ValueTree& device, Session& session, DeviceManager& devices);

/** True for a ".dojopreset" file. */
[[nodiscard]] bool isPresetFile(const juce::File& file);
/** True when at least one of `paths` is not a preset file (file drops that are for something else). */
[[nodiscard]] bool hasNonPresetFile(const juce::StringArray& paths);

/** The user's presets folder (Documents/The Dojo/Presets unless set otherwise). */
[[nodiscard]] juce::File getUserFolder();
/** Points the user's presets folder elsewhere (tests; an empty File restores the default). */
void setUserFolder(const juce::File& folder);

/** The presets compiled into The Dojo, by name. */
[[nodiscard]] std::vector<Preset> getBuiltIn();
/** Every readable preset in the user's presets folder and its sub-folders, by name. */
[[nodiscard]] std::vector<Preset> getUserPresets();
/** Built-in presets, then the user's. */
[[nodiscard]] std::vector<Preset> getAll();
/** The presets (built-in, then the user's) that fit the device kind `key`. */
[[nodiscard]] std::vector<Preset> getFor(const juce::String& key);
/** A preset by its source id (see Preset::getSourceId()). */
[[nodiscard]] std::optional<Preset> findBySourceId(const juce::String& sourceId);

/** A file name for a preset called `name` (characters a file system rejects are dropped). */
[[nodiscard]] juce::String fileNameFor(const juce::String& name);

} // namespace dojo::session::presets

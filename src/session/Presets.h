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
    more devices:

        <DOJO_PRESET version="1" name="Synth Bite" description="..." category="Mixing">
          <DEVICE key="builtin:eq" name="EQ">
            <DOJO_DEVICE type="builtin:eq" version="1"> <PARAM id="band1Freq" value="85"/> ... </DOJO_DEVICE>
          </DEVICE>
          <DEVICE key="builtin:channeleq" name="Channel EQ"> ... </DEVICE>
        </DOJO_PRESET>

    Each DEVICE entry is for one kind of device, named by its `key`: a native device's type, or
    "plugin:<format>:<manufacturer>:<name>" for a hosted plug-in. Native devices' settings are
    written out readably (every parameter by id and value, plus whatever else the device keeps);
    a plug-in's are its own state, base64-encoded.

    There are two kinds of preset:
    - A device preset (Synth Bite) has at most one entry per kind of device, all for the same
      purpose: an EQ loading it takes the EQ entry, a Channel EQ the Channel EQ entry, and a preset
      with no entry for a device does not fit it.
    - A chain preset (chain="1") is a whole device chain, in order, and may hold the same kind of
      device more than once (a subtractive EQ early and an additive EQ late). Each entry's name is
      the role of that device in the chain ("Slapback Delay"). Using it on a track adds the whole
      chain. A device of the chain loading it takes the entry of its kind at its own position: the
      second EQ on a track takes the chain's second EQ entry.

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

/** A preset: a name, a description and settings for one or more devices (or a whole chain). */
struct Preset
{
    juce::String name;
    juce::String description;
    juce::String category; ///< what it is for ("Drums", "Vocals", "Mixing", ...), may be empty
    bool chain = false;    ///< a device chain (entries in order, kinds may repeat)
    std::vector<DeviceSettings> devices;
    bool builtIn = false;
    juce::File file; ///< where it was read from (none for a built-in one)

    /**
        The settings for the device kind `key`, or nullptr when the preset does not fit it. In a
        chain, `occurrence` picks among entries of the same kind (0 = the first; past the last, the
        first).
    */
    [[nodiscard]] const DeviceSettings* findFor(const juce::String& key, int occurrence = 0) const;
    [[nodiscard]] bool fits(const juce::String& key) const { return findFor(key) != nullptr; }
    /** Device presets: adds settings, replacing those already there for the same kind of device. */
    void put(DeviceSettings settings);
    /** Chain presets: adds a device at the end of the chain. */
    void add(DeviceSettings settings);
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

/** How many devices of the same kind come before `device` in its chain (0 for the first). */
[[nodiscard]] int occurrenceOf(const juce::ValueTree& device);

/** Loads `settings` into `device`, recorded in the session's undo history (see apply()). */
bool applySettings(const DeviceSettings& settings, const juce::ValueTree& device, Session& session, DeviceManager& devices);

/**
    Loads the preset's settings for `device` into it (in a chain preset, the entry of its kind at its
    position: see Preset::findFor() and occurrenceOf()), recorded in the session's undo history (in
    the current transaction: begin one first to make it its own step). Undo puts the device back as
    it was. Returns false (and changes nothing) when the preset has no settings for that kind of
    device or the device has no instance.
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

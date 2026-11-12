#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace dojo::ui
{

/**
    The user's preferences, stored in the application settings file. Changing one sends a change
    message (asynchronously) to listeners: the application applies the engine options, the main
    window re-themes itself.
*/
class Preferences final : public juce::ChangeBroadcaster
{
public:
    /** The tabs of the Preferences window. */
    enum class Tab
    {
        general,
        themes,
        audio,
        surround,
        keyboard,
    };

    /** How a surround master (3.1, 5.1, 7.1, Dolby Digital, DTS) plays (the Surround tab). */
    enum class SurroundMode
    {
        standard,    ///< as it is: an output per channel; a stereo device plays the front pair
        foldDown,    ///< an output per channel if there are enough, else the ITU stereo fold-down
        virtualizer, ///< the advanced virtualizer (headphones or speakers)
        device,      ///< the device's own 3D virtualizer, sent the surround channels
    };

    explicit Preferences(juce::PropertiesFile& settings);

    /** Returning the playhead to the start (Stop twice, the return button, Home) also scrolls the
        arrangement back to the start. Off by default. */
    [[nodiscard]] bool getReturnViewToStart() const;
    void setReturnViewToStart(bool enabled);

    /** Playback stops at the end of the last clip. Off by default. */
    [[nodiscard]] bool getStopAtEnd() const;
    void setStopAtEnd(bool enabled);

    /** Surround monitoring. Classic fold-down by default (what earlier versions did). */
    [[nodiscard]] SurroundMode getSurroundMode() const;
    void setSurroundMode(SurroundMode mode);
    /** The virtualizer renders for speakers (crosstalk cancelled) instead of headphones. */
    [[nodiscard]] bool getVirtualizerSpeakers() const;
    void setVirtualizerSpeakers(bool speakers);
    /** The virtualizer's room (0..1): reflections and reverberation. 0.5 by default. */
    [[nodiscard]] double getVirtualizerRoom() const;
    void setVirtualizerRoom(double room);

    /** The theme's name (see theme::getThemes()). */
    [[nodiscard]] juce::String getThemeName() const;
    void setThemeName(const juce::String& name);

    /** Customised keyboard shortcuts (differences from the defaults), or null. */
    [[nodiscard]] std::unique_ptr<juce::XmlElement> getKeyMappings() const;
    void setKeyMappings(const juce::XmlElement& mappings);

    [[nodiscard]] juce::PropertiesFile& getSettings() noexcept { return settings_; }

private:
    void changed();

    juce::PropertiesFile& settings_;
};

} // namespace dojo::ui

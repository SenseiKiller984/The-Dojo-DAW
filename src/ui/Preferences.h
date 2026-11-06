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
        keyboard,
    };

    explicit Preferences(juce::PropertiesFile& settings);

    /** Returning the playhead to the start (Stop twice, the return button, Home) also scrolls the
        arrangement back to the start. Off by default. */
    [[nodiscard]] bool getReturnViewToStart() const;
    void setReturnViewToStart(bool enabled);

    /** Playback stops at the end of the last clip. Off by default. */
    [[nodiscard]] bool getStopAtEnd() const;
    void setStopAtEnd(bool enabled);

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

#include "ui/Preferences.h"

#include "ui/theme/Themes.h"

namespace dojo::ui
{
namespace
{
constexpr const char* kReturnViewKey = "prefReturnViewToStart";
constexpr const char* kStopAtEndKey = "prefStopAtEnd";
constexpr const char* kThemeKey = "prefTheme";
constexpr const char* kKeyMappingsKey = "prefKeyMappings";
} // namespace

Preferences::Preferences(juce::PropertiesFile& settings)
    : settings_(settings)
{
}

bool Preferences::getReturnViewToStart() const
{
    return settings_.getBoolValue(kReturnViewKey, false);
}

void Preferences::setReturnViewToStart(bool enabled)
{
    if (enabled == getReturnViewToStart())
        return;
    settings_.setValue(kReturnViewKey, enabled);
    changed();
}

bool Preferences::getStopAtEnd() const
{
    return settings_.getBoolValue(kStopAtEndKey, false);
}

void Preferences::setStopAtEnd(bool enabled)
{
    if (enabled == getStopAtEnd())
        return;
    settings_.setValue(kStopAtEndKey, enabled);
    changed();
}

juce::String Preferences::getThemeName() const
{
    return theme::findTheme(settings_.getValue(kThemeKey, theme::getDefaultThemeName())).name;
}

void Preferences::setThemeName(const juce::String& name)
{
    const auto& theme = theme::findTheme(name);
    if (theme.name == getThemeName() && settings_.containsKey(kThemeKey))
        return;
    settings_.setValue(kThemeKey, theme.name);
    changed();
}

std::unique_ptr<juce::XmlElement> Preferences::getKeyMappings() const
{
    return settings_.getXmlValue(kKeyMappingsKey);
}

void Preferences::setKeyMappings(const juce::XmlElement& mappings)
{
    settings_.setValue(kKeyMappingsKey, &mappings);
    settings_.saveIfNeeded();
}

void Preferences::changed()
{
    settings_.saveIfNeeded();
    sendChangeMessage();
}

} // namespace dojo::ui

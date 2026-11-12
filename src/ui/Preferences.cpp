#include "ui/Preferences.h"

#include "ui/theme/Themes.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr const char* kReturnViewKey = "prefReturnViewToStart";
constexpr const char* kStopAtEndKey = "prefStopAtEnd";
constexpr const char* kThemeKey = "prefTheme";
constexpr const char* kKeyMappingsKey = "prefKeyMappings";
constexpr const char* kSurroundModeKey = "prefSurroundMode";
constexpr const char* kVirtualizerSpeakersKey = "prefVirtualizerSpeakers";
constexpr const char* kVirtualizerRoomKey = "prefVirtualizerRoom";
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

Preferences::SurroundMode Preferences::getSurroundMode() const
{
    const int value = settings_.getIntValue(kSurroundModeKey, static_cast<int>(SurroundMode::foldDown));
    return static_cast<SurroundMode>(juce::jlimit(0, static_cast<int>(SurroundMode::device), value));
}

void Preferences::setSurroundMode(SurroundMode mode)
{
    if (mode == getSurroundMode())
        return;
    settings_.setValue(kSurroundModeKey, static_cast<int>(mode));
    changed();
}

bool Preferences::getVirtualizerSpeakers() const
{
    return settings_.getBoolValue(kVirtualizerSpeakersKey, false);
}

void Preferences::setVirtualizerSpeakers(bool speakers)
{
    if (speakers == getVirtualizerSpeakers())
        return;
    settings_.setValue(kVirtualizerSpeakersKey, speakers);
    changed();
}

double Preferences::getVirtualizerRoom() const
{
    return juce::jlimit(0.0, 1.0, settings_.getDoubleValue(kVirtualizerRoomKey, 0.5));
}

void Preferences::setVirtualizerRoom(double room)
{
    room = juce::jlimit(0.0, 1.0, room);
    if (std::abs(room - getVirtualizerRoom()) < 1.0e-6)
        return;
    settings_.setValue(kVirtualizerRoomKey, room);
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

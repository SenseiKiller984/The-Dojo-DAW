#include "ui/Preferences.h"

#include "ui/theme/Themes.h"

#include <cmath>

namespace dojo::ui
{
namespace
{
constexpr const char* kReturnViewKey = "prefReturnViewToStart";
constexpr const char* kStopAtEndKey = "prefStopAtEnd";
constexpr const char* kControlSurfacesKey = "prefControlSurfaces";
constexpr const char* kThemeKey = "prefTheme";
constexpr const char* kKeyMappingsKey = "prefKeyMappings";
constexpr const char* kSurroundModeKey = "prefSurroundMode";
constexpr const char* kVirtualizerSpeakersKey = "prefVirtualizerSpeakers";
constexpr const char* kVirtualizerRoomKey = "prefVirtualizerRoom";
constexpr const char* kMpeInputKey = "prefMpeInput";
constexpr const char* kMpeBendRangeKey = "prefMpeBendRange";
constexpr const char* kRendererOutputsKey = "prefRendererOutputs";
constexpr const char* kRendererOscKey = "prefRendererOsc";
constexpr const char* kRendererHostKey = "prefRendererHost";
constexpr const char* kRendererPortKey = "prefRendererPort";
constexpr const char* kRendererRateKey = "prefRendererRate";
constexpr const char* kRendererCartesianKey = "prefRendererCartesian";
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

bool Preferences::getMpeInput() const
{
    return settings_.getBoolValue(kMpeInputKey, true);
}

void Preferences::setMpeInput(bool enabled)
{
    if (enabled == getMpeInput())
        return;
    settings_.setValue(kMpeInputKey, enabled);
    changed();
}

double Preferences::getMpeBendRange() const
{
    const double value = settings_.getDoubleValue(kMpeBendRangeKey, 48.0);
    for (const double range : {2.0, 12.0, 24.0, 48.0})
        if (std::abs(value - range) < 0.5)
            return range;
    return 48.0;
}

void Preferences::setMpeBendRange(double semitones)
{
    if (std::abs(semitones - getMpeBendRange()) < 0.5 && settings_.containsKey(kMpeBendRangeKey))
        return;
    settings_.setValue(kMpeBendRangeKey, semitones);
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

Preferences::ExternalRenderer Preferences::getExternalRenderer() const
{
    ExternalRenderer r;
    r.objectOutputs = settings_.getBoolValue(kRendererOutputsKey, false);
    r.osc = settings_.getBoolValue(kRendererOscKey, false);
    r.host = settings_.getValue(kRendererHostKey, "127.0.0.1").trim();
    if (r.host.isEmpty())
        r.host = "127.0.0.1";
    r.port = juce::jlimit(1, 65535, settings_.getIntValue(kRendererPortKey, 9000));
    r.rate = juce::jlimit(10, 100, settings_.getIntValue(kRendererRateKey, 50));
    r.cartesian = settings_.getBoolValue(kRendererCartesianKey, false);
    return r;
}

void Preferences::setExternalRenderer(const ExternalRenderer& renderer)
{
    const auto before = getExternalRenderer();
    auto r = renderer;
    r.host = r.host.trim().isEmpty() ? juce::String("127.0.0.1") : r.host.trim();
    r.port = juce::jlimit(1, 65535, r.port);
    r.rate = juce::jlimit(10, 100, r.rate);
    if (r.objectOutputs == before.objectOutputs && r.osc == before.osc && r.host == before.host && r.port == before.port
        && r.rate == before.rate && r.cartesian == before.cartesian)
        return;
    settings_.setValue(kRendererOutputsKey, r.objectOutputs);
    settings_.setValue(kRendererOscKey, r.osc);
    settings_.setValue(kRendererHostKey, r.host);
    settings_.setValue(kRendererPortKey, r.port);
    settings_.setValue(kRendererRateKey, r.rate);
    settings_.setValue(kRendererCartesianKey, r.cartesian);
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

juce::String Preferences::getControlSurfaces() const
{
    return settings_.getValue(kControlSurfacesKey, {});
}

void Preferences::setControlSurfaces(const juce::String& surfaces)
{
    if (surfaces == getControlSurfaces())
        return;
    settings_.setValue(kControlSurfacesKey, surfaces);
    changed();
}

void Preferences::changed()
{
    settings_.saveIfNeeded();
    sendChangeMessage();
}

} // namespace dojo::ui

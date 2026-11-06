#include "ui/MixerValues.h"

#include "devices/BuiltinProcessor.h"
#include "session/Session.h"

#include <algorithm>
#include <cmath>

namespace dojo::ui::mixer
{

juce::String formatDecibels(double decibels)
{
    if (decibels <= session::kMinusInfinityDb + 0.001)
        return "-inf";
    return (decibels > 0.0 ? "+" : "") + juce::String(decibels, 1);
}

juce::String formatPan(double pan)
{
    const int percent = juce::roundToInt(std::abs(pan) * 100.0);
    return percent == 0 ? juce::String("C") : (pan < 0.0 ? "L" : "R") + juce::String(percent);
}

std::optional<double> parseVolume(const juce::String& text)
{
    const auto t = text.trim().toLowerCase();
    if (t.contains("inf"))
        return session::kMinusInfinityDb;
    const auto number = devices::parseParameterNumber(t, "dB");
    if (!number.has_value())
        return std::nullopt;
    return std::clamp(*number, session::kMinusInfinityDb, session::kMaxVolumeDb);
}

std::optional<double> parsePan(const juce::String& text)
{
    auto t = text.trim().toLowerCase().removeCharacters(" %");
    if (t.isEmpty())
        return std::nullopt;
    if (t == "c" || t == "center" || t == "centre" || t == "mid")
        return 0.0;

    int side = 0; // -1 left, +1 right, 0 = from the number's sign
    if (t.startsWithChar('l') || t.endsWithChar('l'))
        side = -1;
    else if (t.startsWithChar('r') || t.endsWithChar('r'))
        side = 1;
    t = t.removeCharacters("lr");

    if (t.isEmpty())
        return side != 0 ? std::optional<double>(side) : std::nullopt; // "L" / "R": hard
    const auto number = devices::parseParameterNumber(t, "");
    if (!number.has_value())
        return std::nullopt;
    const double amount = side != 0 ? side * std::abs(*number) : *number;
    return std::clamp(amount / 100.0, -1.0, 1.0);
}

} // namespace dojo::ui::mixer

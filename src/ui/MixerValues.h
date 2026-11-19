#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace dojo::ui::mixer
{

/** "-inf", "+3.0", "-12.5" (dB, one decimal). */
[[nodiscard]] juce::String formatDecibels(double decibels);
/** "C", "L20", "R35" (percent). */
[[nodiscard]] juce::String formatPan(double pan);

/** A typed volume in dB: "-6", "-6 dB", "+3.5", "-inf" / "inf" (silence). Clamped to the fader's
    range. Empty if the text is not a volume. */
[[nodiscard]] std::optional<double> parseVolume(const juce::String& text);
/** A typed pan, -1 (left) .. 1 (right): "C" / "center" / "0", "L20" / "20L" / "-20" (20 % left),
    "R35" / "35R" / "35" (35 % right), "L" / "R" alone for hard left / right. Clamped. Empty if
    the text is not a pan. */
[[nodiscard]] std::optional<double> parsePan(const juce::String& text);

} // namespace dojo::ui::mixer

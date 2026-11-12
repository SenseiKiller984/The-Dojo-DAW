#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <vector>

namespace dojo::ui::theme
{

/** Every colour a theme defines (the tokens in theme::colour). */
struct Palette
{
    juce::Colour windowBackground, panel, panelRaised, panelHeader, control, controlHover, outline, outlineStrong;
    juce::Colour textPrimary, textSecondary, textMuted;
    juce::Colour accent, accentBright, gold, play, record;
    juce::Colour meterBackground, meterLow, meterMid, meterHigh, meterClip;
    juce::Colour laneEven, laneOdd, gridBar, gridBeat, playhead;
};

struct Theme
{
    juce::String name;
    juce::String family; ///< "Standard", "Japanese", "Cherry Blossom", "Manga", "Sour"
    juce::String description;
    bool dark = true;
    Palette palette;
};

/** The built-in themes, standard ones first. */
[[nodiscard]] const std::vector<Theme>& getThemes();
/** The theme called `name`, or the default theme. */
[[nodiscard]] const Theme& findTheme(const juce::String& name);
[[nodiscard]] const juce::String& getDefaultThemeName();

/** Makes `theme` the active palette (theme::colour). The UI must then be refreshed: see
    DojoLookAndFeel::refreshColours() and MainWindow::applyTheme(). */
void applyTheme(const Theme& theme);
[[nodiscard]] const juce::String& getActiveThemeName();

} // namespace dojo::ui::theme

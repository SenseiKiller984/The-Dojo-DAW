#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <array>
#include <cmath>

/**
    The Dojo design tokens.

    One palette and one set of metrics, used by every component, so the whole UI can be re-themed
    from here: the palette is the active theme's (theme::applyTheme in Themes.h). Fonts are embedded (see resources/) and mapped by DojoLookAndFeel, so `sans()` and
    `mono()` render identically on Windows and Linux.
*/
namespace dojo::ui::theme
{

namespace colour
{
// The active theme's palette (see Themes.h: applyTheme() rewrites these). Defaults: "Dojo Night".
// Surfaces — "sumi ink" blacks, from deepest to most raised.
inline juce::Colour windowBackground{0xff0b0c0f};
inline juce::Colour panel{0xff111318};
inline juce::Colour panelRaised{0xff171a20};
inline juce::Colour panelHeader{0xff1c1f26};
inline juce::Colour control{0xff242831};
inline juce::Colour controlHover{0xff2d323c};
inline juce::Colour outline{0xff262a33};
inline juce::Colour outlineStrong{0xff363b46};

// Text — warm "rice paper" whites.
inline juce::Colour textPrimary{0xffece8e1};
inline juce::Colour textSecondary{0xffa4a9b4};
inline juce::Colour textMuted{0xff656b77};

// Accents.
inline juce::Colour accent{0xffe0533d}; // vermilion, as on a hanko seal
inline juce::Colour accentBright{0xffff6b50};
inline juce::Colour gold{0xffd6a645}; // black-belt embroidery gold
inline juce::Colour play{0xff4cc38a};
inline juce::Colour record{0xffff3b3b};

// Metering.
inline juce::Colour meterBackground{0xff08090b};
inline juce::Colour meterLow{0xff3fbf7f};
inline juce::Colour meterMid{0xffd9c24a};
inline juce::Colour meterHigh{0xffe58a3d};
inline juce::Colour meterClip{0xffff3b3b};

// Timeline.
inline juce::Colour laneEven{0xff101216};
inline juce::Colour laneOdd{0xff13151a};
inline juce::Colour gridBar{0xff2b2f38};
inline juce::Colour gridBeat{0xff1b1e24};
inline juce::Colour playhead{0xffff6b50};
} // namespace colour

/** WCAG contrast ratio of two colours (1 .. 21). */
[[nodiscard]] inline double contrastRatio(juce::Colour a, juce::Colour b)
{
    const auto luminance = [](juce::Colour c)
    {
        const auto channel = [](juce::uint8 v)
        {
            const double s = v / 255.0;
            return s <= 0.03928 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4);
        };
        return 0.2126 * channel(c.getRed()) + 0.7152 * channel(c.getGreen()) + 0.0722 * channel(c.getBlue());
    };
    const double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

/** `preferred` if it reads well enough on `background` (contrast >= 3), else near-black or
    near-white, whichever reads better. For text on lit buttons and coloured surfaces. */
[[nodiscard]] inline juce::Colour readableOn(juce::Colour background, juce::Colour preferred)
{
    if (contrastRatio(preferred, background) >= 3.0)
        return preferred;
    const juce::Colour dark{0xff121212}, light{0xfff4f2ee};
    return contrastRatio(dark, background) >= contrastRatio(light, background) ? dark : light;
}

/** Readable text over `background`: near-black on light colours, near-white on dark ones. */
[[nodiscard]] inline juce::Colour textOn(juce::Colour background)
{
    return background.getPerceivedBrightness() > 0.42f ? juce::Colour(0xff121212) : juce::Colour(0xfff4f2ee);
}

/** Default colours offered for tracks/channels (cycled as tracks are created). The same in every
    theme: track colours belong to the project. */
inline const std::array<juce::Colour, 8> trackPalette{
    juce::Colour{0xffe0533d}, // vermilion
    juce::Colour{0xffe58a3d}, // persimmon
    juce::Colour{0xffd6a645}, // gold
    juce::Colour{0xff8fb35a}, // matcha
    juce::Colour{0xff3fb59b}, // jade
    juce::Colour{0xff4f8fd9}, // indigo
    juce::Colour{0xff8a6fd6}, // wisteria
    juce::Colour{0xffd66fa7}, // sakura
};

namespace metrics
{
inline constexpr int transportHeight = 60;
inline constexpr int statusBarHeight = 26;
inline constexpr int browserWidth = 232;
inline constexpr int mixerHeight = 270;
inline constexpr int trackHeaderWidth = 232;
inline constexpr int rulerHeight = 30;
inline constexpr int trackHeight = 58;
inline constexpr int channelStripWidth = 78;
inline constexpr int panelGap = 1;
inline constexpr float cornerRadius = 4.0f;
} // namespace metrics

// Fonts: resolved to the embedded Inter / JetBrains Mono typefaces by DojoLookAndFeel.
inline juce::Font sans(float height)
{
    return juce::Font(juce::FontOptions(height));
}

inline juce::Font sansBold(float height)
{
    return juce::Font(juce::FontOptions(height, juce::Font::bold));
}

inline juce::Font mono(float height)
{
    return juce::Font(
        juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
}

/** Small-caps style section label: semibold, tracked out. */
inline juce::Font label(float height = 11.0f)
{
    return sansBold(height).withExtraKerningFactor(0.08f);
}

} // namespace dojo::ui::theme

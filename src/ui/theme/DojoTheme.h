#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

/**
    The Dojo design tokens.

    One palette and one set of metrics, used by every component, so the whole UI can be re-themed
    from here. Fonts are embedded (see resources/) and mapped by DojoLookAndFeel, so `sans()` and
    `mono()` render identically on Windows and Linux.
*/
namespace dojo::ui::theme
{

namespace colour
{
// Surfaces — "sumi ink" blacks, from deepest to most raised.
inline const juce::Colour windowBackground{0xff0b0c0f};
inline const juce::Colour panel{0xff111318};
inline const juce::Colour panelRaised{0xff171a20};
inline const juce::Colour panelHeader{0xff1c1f26};
inline const juce::Colour control{0xff242831};
inline const juce::Colour controlHover{0xff2d323c};
inline const juce::Colour outline{0xff262a33};
inline const juce::Colour outlineStrong{0xff363b46};

// Text — warm "rice paper" whites.
inline const juce::Colour textPrimary{0xffece8e1};
inline const juce::Colour textSecondary{0xffa4a9b4};
inline const juce::Colour textMuted{0xff656b77};

// Accents.
inline const juce::Colour accent{0xffe0533d}; // vermilion, as on a hanko seal
inline const juce::Colour accentBright{0xffff6b50};
inline const juce::Colour gold{0xffd6a645}; // black-belt embroidery gold
inline const juce::Colour play{0xff4cc38a};
inline const juce::Colour record{0xffff3b3b};

// Metering.
inline const juce::Colour meterBackground{0xff08090b};
inline const juce::Colour meterLow{0xff3fbf7f};
inline const juce::Colour meterMid{0xffd9c24a};
inline const juce::Colour meterHigh{0xffe58a3d};
inline const juce::Colour meterClip{0xffff3b3b};

// Timeline.
inline const juce::Colour laneEven{0xff101216};
inline const juce::Colour laneOdd{0xff13151a};
inline const juce::Colour gridBar{0xff2b2f38};
inline const juce::Colour gridBeat{0xff1b1e24};
inline const juce::Colour playhead{0xffff6b50};
} // namespace colour

/** Default colours offered for tracks/channels (cycled as tracks are created). */
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

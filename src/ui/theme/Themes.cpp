#include "ui/theme/Themes.h"

#include "ui/theme/DojoTheme.h"

namespace dojo::ui::theme
{
namespace
{
juce::Colour c(juce::uint32 argb)
{
    return juce::Colour(argb);
}

Theme makeTheme(const char* name, const char* family, const char* description, bool dark, Palette palette)
{
    return {name, family, description, dark, palette};
}

std::vector<Theme> buildThemes()
{
    std::vector<Theme> themes;

    // --- Standard -----------------------------------------------------------------------------------
    themes.push_back(makeTheme("Standard Black", "Standard", "Neutral greys with a blue accent.", true,
                               {c(0xff0e0e0e), c(0xff161616), c(0xff1d1d1d), c(0xff232323), c(0xff2c2c2c), c(0xff363636), c(0xff2a2a2a), c(0xff3d3d3d),
                                c(0xffeeeeee), c(0xffaaaaaa), c(0xff6e6e6e),
                                c(0xff3d8bfd), c(0xff63a4ff), c(0xfff0b429), c(0xff4caf50), c(0xfff44336),
                                c(0xff080808), c(0xff4caf50), c(0xffe0c341), c(0xffef8a3a), c(0xfff44336),
                                c(0xff141414), c(0xff181818), c(0xff333333), c(0xff1f1f1f), c(0xff63a4ff)}));
    themes.push_back(makeTheme("Standard White", "Standard", "Light greys with a blue accent.", false,
                               {c(0xffe4e4e4), c(0xfff4f4f4), c(0xfffafafa), c(0xffe2e2e2), c(0xffe6e6e6), c(0xffd8d8d8), c(0xffcfcfcf), c(0xffb0b0b0),
                                c(0xff1e1e1e), c(0xff555555), c(0xff8a8a8a),
                                c(0xff1f6feb), c(0xff3b82f6), c(0xffb7791f), c(0xff2e7d32), c(0xffd32f2f),
                                c(0xffdcdcdc), c(0xff2e9d57), c(0xffc9a227), c(0xffde7a2c), c(0xffd32f2f),
                                c(0xfff8f8f8), c(0xfff1f1f1), c(0xffc2c2c2), c(0xffe1e1e1), c(0xff1f6feb)}));

    // --- Japanese -----------------------------------------------------------------------------------
    themes.push_back(makeTheme("Dojo Night", "Japanese", "Sumi-ink blacks, rice-paper text, a vermilion hanko seal.", true,
                               {c(0xff0b0c0f), c(0xff111318), c(0xff171a20), c(0xff1c1f26), c(0xff242831), c(0xff2d323c), c(0xff262a33), c(0xff363b46),
                                c(0xffece8e1), c(0xffa4a9b4), c(0xff656b77),
                                c(0xffe0533d), c(0xffff6b50), c(0xffd6a645), c(0xff4cc38a), c(0xffff3b3b),
                                c(0xff08090b), c(0xff3fbf7f), c(0xffd9c24a), c(0xffe58a3d), c(0xffff3b3b),
                                c(0xff101216), c(0xff13151a), c(0xff2b2f38), c(0xff1b1e24), c(0xffff6b50)}));
    themes.push_back(makeTheme("Washi", "Japanese", "Warm washi paper, sumi ink and shu-red.", false,
                               {c(0xffe6dfcf), c(0xfff2ece0), c(0xfff7f2e8), c(0xffe1d8c5), c(0xffdcd2bd), c(0xffd1c5ac), c(0xffcbbfa6), c(0xffb3a68b),
                                c(0xff23201b), c(0xff5c554a), c(0xff8c8372),
                                c(0xffc23a2b), c(0xffd94a37), c(0xffa87b20), c(0xff3c8a5f), c(0xffd32f2f),
                                c(0xffd8cfbc), c(0xff3f9f6f), c(0xffc29a22), c(0xffd2702a), c(0xffd32f2f),
                                c(0xfff0eadd), c(0xffebe4d5), c(0xffc6b9a0), c(0xffded5c3), c(0xffc23a2b)}));
    themes.push_back(makeTheme("Aizome", "Japanese", "Deep indigo dye with kiniro gold.", true,
                               {c(0xff0a0f1e), c(0xff0f172e), c(0xff141e3b), c(0xff192547), c(0xff213057), c(0xff2a3b68), c(0xff223055), c(0xff324575),
                                c(0xffe9eef8), c(0xffa7b3cf), c(0xff6a7698),
                                c(0xfff2c14e), c(0xffffd36b), c(0xffe2604a), c(0xff5fc9a0), c(0xffff4b55),
                                c(0xff060a15), c(0xff4fbf8f), c(0xffe8c84e), c(0xffeb8f45), c(0xffff4b55),
                                c(0xff0e152b), c(0xff111a33), c(0xff2a3963), c(0xff172242), c(0xffffd36b)}));

    // --- Cherry blossom -----------------------------------------------------------------------------
    themes.push_back(makeTheme("Sakura Night", "Cherry Blossom", "Blossoms under lanterns: plum night, petal pink.", true,
                               {c(0xff150c12), c(0xff1d1118), c(0xff25151e), c(0xff2c1924), c(0xff39212f), c(0xff46293a), c(0xff3a2330), c(0xff553345),
                                c(0xfffbe9f0), c(0xffd4a9ba), c(0xff8c6577),
                                c(0xffff8fb3), c(0xffffb0c9), c(0xfff2c27b), c(0xff8fd6a8), c(0xffff5470),
                                c(0xff0e070b), c(0xff7fd0a0), c(0xfff0cf6e), c(0xffff9a6b), c(0xffff5470),
                                c(0xff1a0f15), c(0xff1e1219), c(0xff4a2c3b), c(0xff2a1823), c(0xffffb0c9)}));
    themes.push_back(makeTheme("Sakura Day", "Cherry Blossom", "Hanami in spring: pale petals and deep pink.", false,
                               {c(0xfff4e1e8), c(0xfffbeff3), c(0xfffff6f8), c(0xfff0d8e1), c(0xffedcfda), c(0xffe5c0cd), c(0xffe3c4d0), c(0xffd2a5b6),
                                c(0xff3b1f2b), c(0xff7a5363), c(0xffa78292),
                                c(0xffd94f7f), c(0xffec6d97), c(0xffb8862b), c(0xff3f9b6a), c(0xffd92b4b),
                                c(0xffeacfd9), c(0xff3fa06e), c(0xffc69a2a), c(0xffdc7a3a), c(0xffd92b4b),
                                c(0xfffaedf1), c(0xfff6e5ec), c(0xffd9b4c3), c(0xffedd8e0), c(0xffd94f7f)}));

    // --- Manga --------------------------------------------------------------------------------------
    themes.push_back(makeTheme("Manga Ink", "Manga", "Black ink on white paper with a red splash.", false,
                               {c(0xfff3f3f0), c(0xffffffff), c(0xffffffff), c(0xffe9e9e6), c(0xffefefec), c(0xffdedede), c(0xff2a2a2a), c(0xff000000),
                                c(0xff0d0d0d), c(0xff3a3a3a), c(0xff767676),
                                c(0xffe0201c), c(0xffff3b30), c(0xff1a1a1a), c(0xff1f9d49), c(0xffe0201c),
                                c(0xffe6e6e3), c(0xff1f9d49), c(0xffd4a21a), c(0xffe56a1c), c(0xffe0201c),
                                c(0xfffbfbf9), c(0xfff1f1ee), c(0xff9a9a9a), c(0xffdadada), c(0xffe0201c)}));
    themes.push_back(makeTheme("Manga Noir", "Manga", "Inverted ink: white lines on black, yellow speed lines.", true,
                               {c(0xff000000), c(0xff0a0a0a), c(0xff131313), c(0xff1b1b1b), c(0xff222222), c(0xff2e2e2e), c(0xff4d4d4d), c(0xffbdbdbd),
                                c(0xffffffff), c(0xffc8c8c8), c(0xff7d7d7d),
                                c(0xffffe600), c(0xfffff35c), c(0xffff3b30), c(0xff00e0ff), c(0xffff2d2d),
                                c(0xff000000), c(0xff36d977), c(0xffffe600), c(0xffff9a1f), c(0xffff2d2d),
                                c(0xff060606), c(0xff0d0d0d), c(0xff3a3a3a), c(0xff1a1a1a), c(0xffffe600)}));
    themes.push_back(makeTheme("Shonen Pop", "Manga", "Bold panels: cream, ink outlines, red and blue.", false,
                               {c(0xfffff4d6), c(0xfffffcf0), c(0xffffffff), c(0xffffe066), c(0xfffff0b3), c(0xffffe680), c(0xff1b1b1b), c(0xff000000),
                                c(0xff141414), c(0xff3d3d3d), c(0xff7a7466),
                                c(0xffff3b2f), c(0xffff6150), c(0xff1e6bff), c(0xff00a651), c(0xffff1f1f),
                                c(0xfff1e6bf), c(0xff00a651), c(0xffe6b800), c(0xffff7a1a), c(0xffff1f1f),
                                c(0xfffffaf0), c(0xfffff3da), c(0xffb8a978), c(0xffefe3bd), c(0xff1e6bff)}));
    return themes;
}

juce::String& activeName()
{
    static juce::String name = getDefaultThemeName();
    return name;
}
} // namespace

const std::vector<Theme>& getThemes()
{
    static const std::vector<Theme> themes = buildThemes();
    return themes;
}

const juce::String& getDefaultThemeName()
{
    static const juce::String name = "Dojo Night";
    return name;
}

const Theme& findTheme(const juce::String& name)
{
    const auto& themes = getThemes();
    for (const auto& theme : themes)
        if (theme.name == name)
            return theme;
    for (const auto& theme : themes)
        if (theme.name == getDefaultThemeName())
            return theme;
    return themes.front();
}

void applyTheme(const Theme& theme)
{
    const auto& p = theme.palette;
    colour::windowBackground = p.windowBackground;
    colour::panel = p.panel;
    colour::panelRaised = p.panelRaised;
    colour::panelHeader = p.panelHeader;
    colour::control = p.control;
    colour::controlHover = p.controlHover;
    colour::outline = p.outline;
    colour::outlineStrong = p.outlineStrong;
    colour::textPrimary = p.textPrimary;
    colour::textSecondary = p.textSecondary;
    colour::textMuted = p.textMuted;
    colour::accent = p.accent;
    colour::accentBright = p.accentBright;
    colour::gold = p.gold;
    colour::play = p.play;
    colour::record = p.record;
    colour::meterBackground = p.meterBackground;
    colour::meterLow = p.meterLow;
    colour::meterMid = p.meterMid;
    colour::meterHigh = p.meterHigh;
    colour::meterClip = p.meterClip;
    colour::laneEven = p.laneEven;
    colour::laneOdd = p.laneOdd;
    colour::gridBar = p.gridBar;
    colour::gridBeat = p.gridBeat;
    colour::playhead = p.playhead;
    activeName() = theme.name;
}

const juce::String& getActiveThemeName()
{
    return activeName();
}

} // namespace dojo::ui::theme

#include "ui/components/BrowserPanel.h"

#include "ui/theme/DojoTheme.h"

#include <vector>

namespace dojo::ui
{
namespace
{
struct Section
{
    const char* title;
    std::vector<const char*> items;
};

const std::vector<Section>& getSections()
{
    static const std::vector<Section> sections{
        {"INSTRUMENTS", {"Drum Rack", "Sampler"}},
        {"AUDIO EFFECTS", {"Compressor", "Limiter", "Reverb", "Parametric EQ", "Delay", "Speed & Pitch"}},
        {"PLUG-INS", {"VST3", "CLAP"}},
        {"SAMPLES", {"Drop .wav / .flac folders here"}},
    };
    return sections;
}

constexpr int kHeaderHeight = 34;
constexpr int kSectionHeight = 26;
constexpr int kItemHeight = 24;
} // namespace

BrowserPanel::BrowserPanel()
{
    setOpaque(true);
}

void BrowserPanel::paint(juce::Graphics& g)
{
    using namespace theme;

    g.fillAll(colour::panel);

    auto area = getLocalBounds();

    // Header
    auto header = area.removeFromTop(kHeaderHeight);
    g.setColour(colour::panelHeader);
    g.fillRect(header);
    g.setColour(colour::textSecondary);
    g.setFont(label(11.0f));
    g.drawText("BROWSER", header.reduced(14, 0), juce::Justification::centredLeft, false);
    g.setColour(colour::outline);
    g.fillRect(header.removeFromBottom(1));

    area.removeFromTop(6);

    for (const auto& section : getSections())
    {
        auto sectionHeader = area.removeFromTop(kSectionHeight);
        g.setColour(colour::textMuted);
        g.setFont(label(10.0f));
        g.drawText(section.title, sectionHeader.reduced(14, 0), juce::Justification::centredLeft, false);

        for (const auto* item : section.items)
        {
            auto row = area.removeFromTop(kItemHeight).reduced(8, 1);

            g.setColour(colour::accent.withAlpha(0.85f));
            g.fillEllipse(juce::Rectangle<float>(4.0f, 4.0f)
                              .withCentre({static_cast<float>(row.getX()) + 10.0f,
                                           static_cast<float>(row.getCentreY())}));

            g.setColour(colour::textPrimary.withAlpha(0.85f));
            g.setFont(sans(13.0f));
            g.drawText(item, row.withTrimmedLeft(20), juce::Justification::centredLeft, true);
        }

        area.removeFromTop(8);
    }

    // Right-hand separator.
    g.setColour(colour::outline);
    g.fillRect(getLocalBounds().removeFromRight(1));
}

} // namespace dojo::ui

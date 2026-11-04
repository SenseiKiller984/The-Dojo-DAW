#include "ui/PlaceholderSession.h"

#include "ui/theme/DojoTheme.h"

namespace dojo::ui::placeholder
{

const std::vector<Track>& getTracks()
{
    static const std::vector<Track> tracks = []
    {
        const auto& palette = theme::trackPalette;
        std::vector<Track> t;

        t.push_back({"Drum Bus", "Group bus", TrackKind::groupBus, 0, palette[0], -2.0f, 0.0f, {}});
        t.push_back({"Kick",
                     "Drum Rack - pad 1",
                     TrackKind::audio,
                     1,
                     palette[0],
                     -4.0f,
                     0.0f,
                     {{0.0, 4.0, "Kick A"}, {4.0, 4.0, "Kick A"}, {8.0, 8.0, "Kick B"}}});
        t.push_back({"Snare",
                     "Drum Rack - pad 2",
                     TrackKind::audio,
                     1,
                     palette[1],
                     -6.0f,
                     0.0f,
                     {{4.0, 4.0, "Snare"}, {8.0, 8.0, "Snare Fill"}}});
        t.push_back({"Hi-Hat",
                     "Drum Rack - pad 3",
                     TrackKind::instrument,
                     1,
                     palette[2],
                     -10.0f,
                     0.25f,
                     {{0.0, 8.0, "Hats 16th"}, {8.0, 8.0, "Hats Open"}}});
        t.push_back({"Bass",
                     "Sampler",
                     TrackKind::instrument,
                     0,
                     palette[4],
                     -5.0f,
                     0.0f,
                     {{4.0, 12.0, "Sub Bass"}}});
        t.push_back({"Lead Synth",
                     "VST3 / CLAP instrument",
                     TrackKind::instrument,
                     0,
                     palette[5],
                     -8.0f,
                     -0.2f,
                     {{8.0, 8.0, "Hook"}}});
        t.push_back({"Vocals",
                     "Input 1",
                     TrackKind::audio,
                     0,
                     palette[7],
                     -3.0f,
                     0.0f,
                     {{8.0, 4.0, "Verse Vox"}, {12.0, 4.0, "Chorus Vox"}}});
        t.push_back({"Reverb Return", "Aux return", TrackKind::auxReturn, 0, palette[6], -12.0f, 0.0f, {}});

        return t;
    }();

    return tracks;
}

juce::String getKindLabel(TrackKind kind)
{
    switch (kind)
    {
    case TrackKind::audio:
        return "AUDIO";
    case TrackKind::instrument:
        return "INST";
    case TrackKind::groupBus:
        return "BUS";
    case TrackKind::auxReturn:
        return "AUX";
    }

    return {};
}

} // namespace dojo::ui::placeholder

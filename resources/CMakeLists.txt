# Embedded resources. Fonts are compiled into the binary so that text renders with identical
# metrics on Windows and Linux (system UI fonts differ: Segoe UI vs. whatever fontconfig picks),
# which keeps every layout pixel-identical across platforms.
#
# Built-in device presets and chains (presets/*.dojopreset) are compiled in too: every
# ".dojopreset" listed here shows up as a built-in preset (session/Presets.h).
#
# Licences: both families are SIL Open Font License 1.1 — see fonts/LICENSE-*.txt.

juce_add_binary_data(dojo_resources
    NAMESPACE   DojoBinaryData
    HEADER_NAME DojoBinaryData.h
    SOURCES
        fonts/Inter-Regular.ttf
        fonts/Inter-SemiBold.ttf
        fonts/JetBrainsMono-Medium.ttf
        "presets/Synth Bite.dojopreset"
        "presets/Punchy Drum Bus.dojopreset"
        "presets/Dusty Lo-Fi Drums.dojopreset"
        "presets/Pop Vocal Polish.dojopreset"
        "presets/Upfront Rap Vocal.dojopreset"
        "presets/Drivers License Vocal Saturation.dojopreset"
        "presets/Dreamy Wide Space.dojopreset"
        "presets/Old Radio.dojopreset"
        "presets/Gritty Bass Weight.dojopreset"
        "presets/Shimmer Bloom.dojopreset")

add_library(dojo::resources ALIAS dojo_resources)
set_target_properties(dojo_resources PROPERTIES FOLDER "Resources")

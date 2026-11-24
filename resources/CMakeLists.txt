# Embedded resources. Fonts are compiled into the binary so that text renders with identical
# metrics on Windows and Linux (system UI fonts differ: Segoe UI vs. whatever fontconfig picks),
# which keeps every layout pixel-identical across platforms.
#
# Built-in device presets and chains (presets/*.dojopreset) are compiled in too: every
# ".dojopreset" listed here shows up as a built-in preset (session/Presets.h).
#
# Built-in drum kits: presets/kits/*.dojopreset (Drum Rack presets with a demo beat) and the
# drum machines' samples, one archive per machine (kits/<Machine>.zip, unpacked into the
# library folder when first used: devices/DrumKits.h). Both are made by
# tools/drumkits/make_kits.py from the original sample sets.
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
        "presets/Shimmer Bloom.dojopreset"
        "presets/kits/LM-1 Kit.dojopreset"
        "presets/kits/LM-1 Alt Kit.dojopreset"
        "presets/kits/LM-2 Kit.dojopreset"
        "presets/kits/LM-2 Soft Kit.dojopreset"
        "presets/kits/LinnDrum Kit.dojopreset"
        "presets/kits/LinnDrum Tuned Kit.dojopreset"
        "presets/kits/LinnDrum Percussion Kit.dojopreset"
        "presets/kits/AdrenaLinn Kit.dojopreset"
        "presets/kits/AdrenaLinn Kit 2.dojopreset"
        "presets/kits/AdrenaLinn Sub Kit.dojopreset"
        "presets/kits/Linn 9000 Kit.dojopreset"
        "presets/kits/Linn 9000 Kit 2.dojopreset"
        "presets/kits/Linn 9000 Kit 3.dojopreset"
        "presets/kits/Linn 9000 Kit 4.dojopreset"
        "presets/kits/Linn 9000 Kit 5.dojopreset"
        "presets/kits/Linn 9000 Selector Kit.dojopreset"
        "kits/Linn LM-1.zip"
        "kits/Linn LM-2.zip"
        "kits/LinnDrum.zip"
        "kits/AdrenaLinn.zip"
        "kits/Linn 9000.zip")

add_library(dojo::resources ALIAS dojo_resources)
set_target_properties(dojo_resources PROPERTIES FOLDER "Resources")
